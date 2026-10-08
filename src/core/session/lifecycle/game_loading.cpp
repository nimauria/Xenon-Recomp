#include <algorithm>
#include <iostream>
#include <mutex>

#include "xenon/core/session.hpp"
#include "xenon/xam/content_graph.hpp"

namespace xenon::core {

SessionResult XenonSession::load_game(std::span<const std::byte> xex_bytes,
                                     std::string_view game_id,
                                     std::span<const std::byte> title_update_bytes) {
  if (!is_initialized()) {
    return SessionResult::failure("Session not initialized");
  }

  if (state() != SessionState::Ready) {
    return SessionResult::failure("Session not in ready state");
  }

  set_state(SessionState::LoadingGame, "Loading game...");
  game_id_ = std::string(game_id);

  // Parse the immutable base image first - always, even when a title update
  // is selected, since apply_title_update() validates the update against it
  // (title/media identity, base-signature digest, source version) and needs
  // its header/effective-image bytes to do so. xex_bytes itself is never
  // modified.
  xbox::XexImage base_image{};
  std::string error;
  if (!xbox::parse_xex_image(xex_bytes, base_image, &error)) {
    set_error("Failed to parse base XEX: " + error);
    return SessionResult::failure(last_error_);
  }

  // When a title update was selected (see mount_content_graph()/Content
  // Services), apply it through XEX Loader V2's canonical XEXP patcher and
  // make the resulting *effective* image - not the base image - what
  // actually gets mapped and executed. A malformed/incompatible update is a
  // hard, explicit launch failure here: never a silent fallback to the base
  // XEX (see docs/runtime/RUNTIME_SESSION.md's title-update integration section).
  const bool has_title_update = !title_update_bytes.empty();
  xbox::XexImage patched_image{};
  const xbox::XexImage* effective_image = &base_image;
  if (has_title_update) {
    if (!xbox::apply_title_update(base_image, title_update_bytes, patched_image, &error)) {
      set_error("Failed to apply title update: " + error);
      return SessionResult::failure(last_error_);
    }
    effective_image = &patched_image;
  }

  // Map the effective image into memory.
  xbox::LoadedXex loaded{};
  if (!xbox::map_xex_image(*memory_, *effective_image, loaded, memory::kXex64KBase, &error)) {
    set_error("Failed to map effective XEX image: " + error);
    return SessionResult::failure(last_error_);
  }

  loaded_xex_ = std::move(loaded);
  effective_identity_ =
      xbox::compute_effective_identity(base_image, has_title_update ? &patched_image : nullptr);
  if (config_.enable_logging) {
    std::scoped_lock console_log_lock(console_log_mutex());
    std::cout << "[XenonSession] Effective executable: title_id=0x" << std::hex
              << effective_identity_->title_id << " media_id=0x" << effective_identity_->media_id
              << std::dec << " title_update_applied=" << (has_title_update ? "yes" : "no")
              << " hash=" << xbox::format_effective_image_hash(effective_identity_->effective_image_hash)
              << std::endl;
  }

  // Bind true native variable imports to guest-backed system-module
  // variables before any guest code or callbacks can observe the IAT slots.
  if (!bind_xex_variable_imports()) {
    return SessionResult::failure(last_error_.empty()
                                      ? "Failed to bind XEX variable imports"
                                      : last_error_);
  }

  // Resolve imports
  if (!resolve_xex_imports()) {
    return SessionResult::failure("Failed to resolve XEX imports");
  }

  // Bind compiled code (if any)
  if (!bind_compiled_code()) {
    return SessionResult::failure("Failed to bind compiled code");
  }

  // Create guest process/main thread
  if (!create_guest_process()) {
    return SessionResult::failure("Failed to create guest process");
  }
  if (!refresh_dynamic_kernel_variables()) {
    set_error("Failed to initialize dynamic xboxkrnl variable exports");
    return SessionResult::failure(last_error_);
  }

  set_state(SessionState::Ready, "Game loaded successfully");
  reach_boot_checkpoint(BootCheckpoint::XexLoaded);
  return SessionResult::ok("Game loaded", SessionState::Ready);
}

SessionResult XenonSession::mount_content_graph(
    std::uint32_t title_id,
    const std::filesystem::path& base_content_path,
    const std::filesystem::path& title_update_path,
    const std::filesystem::path& dlc_path,
    std::uint64_t profile_xuid) {
  
  if (!filesystem_) {
    return SessionResult::failure("Filesystem not initialized");
  }
  
  if (!xam_) {
    return SessionResult::failure("XAM not initialized");
  }
  
  // Initialize content services if not already done
  auto& content_manager = xam_->content();
  if (!content_manager.save_manager()) {
    // The launcher resolves savePath for the active profile/game and forwards
    // it through SessionConfig. Direct/headless callers that do not provide
    // one fall back to a private temp root rather than guessing a platform
    // Documents directory from temp_directory_path().
    const auto save_dir = config_.save_root_path.empty()
                              ? (std::filesystem::temp_directory_path() / "Xenon" / "Saves")
                              : config_.save_root_path;
    content_manager.initialize_content_services(save_dir);
  }
  
  // Build content graph
  content_graph_ = content_manager.build_content_graph(
      title_id,
      base_content_path,
      title_update_path,
      dlc_path,
      profile_xuid
  );
  
  if (!content_graph_) {
    return SessionResult::failure("Failed to build content graph");
  }
  
  // Mount content graph to VFS
  if (!content_manager.mount_content_graph(*filesystem_, *content_graph_)) {
    return SessionResult::failure("Failed to mount content graph");
  }
  
  set_state(SessionState::Ready, "Content graph mounted successfully");
  return SessionResult::ok("Content mounted", SessionState::Ready);
}

SessionResult XenonSession::mount_content(std::string_view host_path,
                                         std::string_view guest_mount_point) {
  if (!filesystem_) {
    return SessionResult::failure("Filesystem not initialized");
  }

  // Simple legacy content mounting - just mount a host path
  // For production use, prefer mount_content_graph()
  
  return SessionResult::ok("Use mount_content_graph() for full content support");
}

bool XenonSession::resolve_xex_imports() {
  if (!loaded_xex_) {
    return false;
  }

  // XEX-native function imports contain both a type-0 address record and a
  // type-1 callable thunk. Only the callable thunk participates in function
  // export resolution. Standalone type-0 records are genuine variable
  // imports and resolve through the guest-backed variable-export registry.
  unresolved_imports_.clear();
  for (const auto& import : loaded_xex_->image.imports) {
    if (import.is_function_address()) continue;

    bool resolved = false;
    if (import.is_variable()) {
      resolved = !import.symbol.empty()
                     ? export_registry_.resolve_variable(import.module, import.symbol).has_value()
                     : export_registry_.contains_variable(import.module, import.ordinal);
    } else if (import.callable()) {
      resolved = !import.symbol.empty()
                     ? export_registry_.contains(import.module, import.symbol)
                     : export_registry_.contains(import.module, import.ordinal);
    }
    if (resolved) continue;

    const bool already_reported = std::any_of(
        unresolved_imports_.begin(), unresolved_imports_.end(),
        [&](const UnresolvedImport& existing) {
          return existing.library == import.module && existing.symbol == import.symbol &&
                 existing.ordinal == import.ordinal;
        });
    if (already_reported) continue;

    unresolved_imports_.push_back(
        UnresolvedImport{import.module, import.symbol, import.ordinal});
    if (config_.enable_export_diagnostics) {
      std::scoped_lock console_log_lock(console_log_mutex());
      std::cout << "[XenonSession] Unresolved "
                << (import.is_variable() ? "variable import: " : "import: ")
                << import.module << " '" << import.symbol << "' ordinal "
                << import.ordinal << std::endl;
    }
  }
  return true;
}

bool XenonSession::bind_compiled_code() {
  if (!loaded_xex_) {
    return false;
  }

  load_native_extension();
  // A missing/failed native extension is not a load failure: the session
  // still loads (imports resolved, content mounted) so status/UI can report
  // exactly what is missing. start() refuses to run without a bound
  // registry rather than silently doing nothing.
  return true;
}

}  // namespace xenon::core

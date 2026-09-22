#include "status_writer.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "xenon/core/json.hpp"
#include "xenon/core/session.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace xenon::runtime_host {

namespace {

std::int64_t now_epoch_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::uint64_t current_process_id() {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

std::string hex32(std::uint32_t value) {
  std::ostringstream out;
  out << std::hex << std::uppercase << value;
  return out.str();
}

std::string state_name(core::SessionState state) {
  switch (state) {
    case core::SessionState::Uninitialized: return "uninitialized";
    case core::SessionState::Initializing: return "initializing";
    case core::SessionState::Ready: return "ready";
    case core::SessionState::LoadingGame: return "loading";
    case core::SessionState::Running: return "running";
    case core::SessionState::Paused: return "paused";
    case core::SessionState::Stopping: return "stopping";
    case core::SessionState::Stopped: return "stopped";
    case core::SessionState::Failed: return "failed";
  }
  return "unknown";
}

}  // namespace

const char* launch_failure_category_name(LaunchFailureCategory category) noexcept {
  switch (category) {
    case LaunchFailureCategory::None: return "None";
    case LaunchFailureCategory::SessionInitFailed: return "SessionInitFailed";
    case LaunchFailureCategory::MissingRequiredContent: return "MissingRequiredContent";
    case LaunchFailureCategory::ContentMountFailed: return "ContentMountFailed";
    case LaunchFailureCategory::GameLoadFailed: return "GameLoadFailed";
    case LaunchFailureCategory::ModuleRevisionMismatch: return "ModuleRevisionMismatch";
    case LaunchFailureCategory::StartFailed: return "StartFailed";
    case LaunchFailureCategory::WindowCreationFailed: return "WindowCreationFailed";
    case LaunchFailureCategory::GraphicsPresentationFailed: return "GraphicsPresentationFailed";
    case LaunchFailureCategory::AudioBackendFailed: return "AudioBackendFailed";
    case LaunchFailureCategory::InputBackendFailed: return "InputBackendFailed";
  }
  return "Unknown";
}

StatusWriter::StatusWriter(std::string session_dir, LaunchConfig config)
    : session_dir_(std::move(session_dir)),
      config_(std::move(config)),
      started_at_epoch_ms_(now_epoch_ms()) {
  std::filesystem::create_directories(session_dir_);
}

std::string StatusWriter::status_path() const {
  return (std::filesystem::path(session_dir_) / "status.json").string();
}

std::string StatusWriter::stop_signal_path() const {
  return (std::filesystem::path(session_dir_) / "stop.signal").string();
}

bool StatusWriter::stop_requested() const {
  std::error_code ignored;
  return std::filesystem::exists(stop_signal_path(), ignored);
}

void StatusWriter::write(core::XenonSession& session, const std::string& phase_message) {
  core::JsonValue root = core::JsonValue::make_object();
  root.set("available", true);
  root.set("pid", static_cast<double>(current_process_id()));
  root.set("sessionId", config_.session_id);
  root.set("gameId", config_.game_id);
  root.set("title", config_.title);
  root.set("moduleId", config_.module_id);
  root.set("moduleName", config_.module_name);
  root.set("moduleVersion", config_.module_version);
  root.set("requestedRenderer", config_.renderer);
  root.set("startedAtEpochMs", static_cast<double>(started_at_epoch_ms_));
  root.set("updatedAtEpochMs", static_cast<double>(now_epoch_ms()));
  if (!phase_message.empty()) root.set("phaseMessage", phase_message);

  const auto state = session.state();
  root.set("state", static_cast<double>(static_cast<int>(state)));
  root.set("stateName", state_name(state));
  root.set("initialized", session.is_initialized());
  root.set("running", session.is_running());
  root.set("executionActive", session.execution_active());
  root.set("lastError", session.last_error());

  core::JsonValue native_extension = core::JsonValue::make_object();
  native_extension.set("path", config_.native_extension_path);
  native_extension.set("bound", session.native_extension_bound());
  native_extension.set("error", session.native_extension_error());
  root.set("nativeExtension", std::move(native_extension));

  core::JsonValue unresolved = core::JsonValue::make_array();
  for (const auto& import : session.unresolved_imports()) {
    core::JsonValue entry = core::JsonValue::make_object();
    entry.set("library", import.library);
    entry.set("symbol", import.symbol);
    entry.set("ordinal", static_cast<double>(import.ordinal));
    unresolved.append(std::move(entry));
  }
  root.set("unresolvedImports", std::move(unresolved));

  if (const auto* loaded = session.loaded_xex()) {
    core::JsonValue xex = core::JsonValue::make_object();
    xex.set("loaded", true);
    xex.set("titleId", hex32(loaded->image.title_id));
    xex.set("mediaId", hex32(loaded->image.media_id));
    xex.set("entryPoint", hex32(loaded->image.entry_point));
    xex.set("imageBase", hex32(loaded->image.image_base));
    xex.set("executableRanges", static_cast<double>(loaded->executable_ranges.size()));
    if (const auto& identity = session.effective_identity(); identity.has_value()) {
      xex.set("titleUpdateApplied", identity->title_update_applied);
      xex.set("baseVersion", hex32(identity->base_version.value));
      xex.set("effectiveVersion", hex32(identity->effective_version.value));
      xex.set("effectiveImageHash", xenon::xbox::format_effective_image_hash(identity->effective_image_hash));
    }
    root.set("loadedXex", std::move(xex));
  } else {
    core::JsonValue xex = core::JsonValue::make_object();
    xex.set("loaded", false);
    root.set("loadedXex", std::move(xex));
  }

  core::JsonValue subsystems = core::JsonValue::make_object();
  subsystems.set("memory", session.memory() != nullptr);
  subsystems.set("filesystem", session.filesystem() != nullptr);
  subsystems.set("input", session.input() != nullptr);
  subsystems.set("gpu", session.gpu() != nullptr);
  subsystems.set("xam", session.xam() != nullptr);
  root.set("subsystems", std::move(subsystems));

  // Atomic-ish publish: write to a temp file in the same directory then
  // rename over the published path, so a launcher poll never observes a
  // half-written status.json.
  const auto final_path = std::filesystem::path(status_path());
  const auto temp_path = final_path.string() + ".tmp";
  {
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    out << root.dump();
  }
  std::error_code rename_error;
  std::filesystem::rename(temp_path, final_path, rename_error);
}

void StatusWriter::write_fatal(const std::string& message, LaunchFailureCategory category) {
  core::JsonValue root = core::JsonValue::make_object();
  root.set("available", true);
  root.set("pid", static_cast<double>(current_process_id()));
  root.set("sessionId", config_.session_id);
  root.set("gameId", config_.game_id);
  root.set("state", static_cast<double>(static_cast<int>(core::SessionState::Failed)));
  root.set("stateName", state_name(core::SessionState::Failed));
  root.set("initialized", false);
  root.set("running", false);
  root.set("executionActive", false);
  root.set("lastError", message);
  if (category != LaunchFailureCategory::None) {
    root.set("errorCategory", std::string(launch_failure_category_name(category)));
  }
  root.set("startedAtEpochMs", static_cast<double>(started_at_epoch_ms_));
  root.set("updatedAtEpochMs", static_cast<double>(now_epoch_ms()));

  const auto final_path = std::filesystem::path(status_path());
  const auto temp_path = final_path.string() + ".tmp";
  {
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    out << root.dump();
  }
  std::error_code rename_error;
  std::filesystem::rename(temp_path, final_path, rename_error);
}

}  // namespace xenon::runtime_host

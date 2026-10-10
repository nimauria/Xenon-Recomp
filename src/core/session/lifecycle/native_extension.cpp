#include <iostream>
#include <mutex>
#include <string>
#include <string_view>

#include "core/session/session_internal.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace xenon::core {

namespace {

using detail::ascii_lower;

void* load_native_library(const std::string& path, std::string* error) {
#if defined(_WIN32)
  HMODULE handle = ::LoadLibraryA(path.c_str());
  if (!handle && error) *error = "LoadLibrary failed for '" + path + "'";
  return static_cast<void*>(handle);
#else
  void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle && error) *error = std::string("dlopen failed: ") + dlerror();
  return handle;
#endif
}

void* resolve_native_symbol(void* handle, const char* name) {
#if defined(_WIN32)
  return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(handle), name));
#else
  return ::dlsym(handle, name);
#endif
}

void unload_native_library(void* handle) noexcept {
  if (!handle) return;
#if defined(_WIN32)
  ::FreeLibrary(static_cast<HMODULE>(handle));
#else
  ::dlclose(handle);
#endif
}

// Contract a native extension library exports so XenonSession can bind its
// recomp-driver-generated compiled-code registry into a CPU V2
// ExecutionContext. Documented in docs/runtime/RUNTIME_HOST.md; module authors
// (e.g. Project Gracemeria) implement this once around their generated
// registry.cpp's bind_compiled_registry(ExecutionContext&).
using XenonBindCompiledRegistryFn = void (*)(cpu::ExecutionContext&);
constexpr const char* kBindCompiledRegistrySymbol = "Xenon_BindCompiledRegistry";

// Optional, additive module-identity export: a module built by the Recomp
// Driver from a specific effective XEX (base, or base+title-update -
// generate_project() emits this automatically) may export this to declare
// which effective-image SHA1 hash(es) (xbox::format_effective_image_hash()
// hex form, semicolon-separated for more than one) its compiled registry is
// valid for. A module with no such export is not identity-checked - this is
// opt-in enforcement layered on top of the required
// Xenon_BindCompiledRegistry contract, not a requirement on every module
// (see docs/runtime/RUNTIME_HOST.md "Native extension contract"). Real hardware has
// no equivalent concept; this exists purely so Xenon itself never runs code
// generated from one guest executable revision against a different one.
using XenonSupportedExecutableRevisionsFn = const char* (*)();
constexpr const char* kSupportedExecutableRevisionsSymbol = "Xenon_SupportedExecutableRevisions";

// `declared` is a semicolon-separated list of hex SHA1 hashes (case-
// insensitive); an empty entry between separators is ignored rather than
// treated as a (never-matching) wildcard-less empty declaration.
bool declared_revisions_include(std::string_view declared, const std::string& effective_hash_hex) {
  std::size_t start = 0u;
  while (start <= declared.size()) {
    auto end = declared.find(';', start);
    if (end == std::string_view::npos) end = declared.size();
    const auto token = ascii_lower(declared.substr(start, end - start));
    if (!token.empty() && token == effective_hash_hex) return true;
    start = end + 1u;
  }
  return false;
}

}  // namespace

void XenonSession::load_native_extension() {
  native_extension_bound_ = false;
  native_extension_error_.clear();
  compiled_registry_binder_ = nullptr;
  unload_native_extension();

  if (config_.native_extension_path.empty()) {
    native_extension_error_ = "no native extension configured for this game";
    return;
  }

  std::string load_error;
  native_extension_handle_ = load_native_library(config_.native_extension_path, &load_error);
  if (!native_extension_handle_) {
    native_extension_error_ = load_error.empty()
                                   ? "failed to load native extension library"
                                   : load_error;
    if (config_.enable_logging) {
      std::scoped_lock console_log_lock(console_log_mutex());
      std::cout << "[XenonSession] Native extension load failed: " << native_extension_error_
                << std::endl;
    }
    return;
  }

  auto* symbol = resolve_native_symbol(native_extension_handle_, kBindCompiledRegistrySymbol);
  if (!symbol) {
    native_extension_error_ =
        std::string("native extension does not export '") + kBindCompiledRegistrySymbol + "'";
    if (config_.enable_logging) {
      std::scoped_lock console_log_lock(console_log_mutex());
      std::cout << "[XenonSession] Native extension load failed: " << native_extension_error_
                << std::endl;
    }
    unload_native_extension();
    return;
  }

  // Module-compatibility validation: if this module declares which
  // effective-executable revision(s) it was compiled for, the currently
  // loaded effective image (base, or base+title-update - see load_game())
  // must be one of them. A module that declares nothing is not checked
  // (back-compat with modules that predate this contract); a module that
  // does declare revisions and does not include the running one is rejected
  // outright rather than silently run against code it was never generated
  // from (see docs/runtime/RUNTIME_HOST.md's "Effective executable identity"
  // section).
  if (effective_identity_) {
    if (auto* revisions_symbol =
            resolve_native_symbol(native_extension_handle_, kSupportedExecutableRevisionsSymbol)) {
      auto* revisions_fn = reinterpret_cast<XenonSupportedExecutableRevisionsFn>(revisions_symbol);
      const char* declared_raw = revisions_fn();
      const std::string declared = declared_raw ? declared_raw : "";
      if (!declared.empty()) {
        const auto effective_hash_hex =
            ascii_lower(xbox::format_effective_image_hash(effective_identity_->effective_image_hash));
        if (!declared_revisions_include(declared, effective_hash_hex)) {
          native_extension_error_ =
              "native extension does not declare compatibility with the effective executable "
              "revision (hash " + effective_hash_hex + "); module declares: " + declared;
          if (config_.enable_logging) {
            std::scoped_lock console_log_lock(console_log_mutex());
            std::cout << "[XenonSession] Native extension rejected: " << native_extension_error_
                      << std::endl;
          }
          unload_native_extension();
          return;
        }
      }
    }
  }

  auto* bind_fn = reinterpret_cast<XenonBindCompiledRegistryFn>(symbol);
  compiled_registry_binder_ = [bind_fn](cpu::ExecutionContext& context) { bind_fn(context); };
  native_extension_bound_ = true;
  if (config_.enable_logging) {
    std::scoped_lock console_log_lock(console_log_mutex());
    std::cout << "[XenonSession] Native extension bound: " << config_.native_extension_path
              << std::endl;
  }
}

void XenonSession::unload_native_extension() noexcept {
  if (native_extension_handle_) {
    unload_native_library(native_extension_handle_);
    native_extension_handle_ = nullptr;
  }
}

}  // namespace xenon::core

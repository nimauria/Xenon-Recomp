#include "xenon/core/session.hpp"

#include <iostream>
#include <mutex>

// Complete types for the subsystems XenonSession owns through unique_ptr.
#include "xenon/xam/content_graph.hpp"
#if defined(XENON_HAS_AUDIO)
#include "xenon/audio/system.hpp"
#endif

namespace xenon::core {

XenonSession::XenonSession() = default;
XenonSession::~XenonSession() {
  shutdown();
  stop_memory_watch_poll();
}

SessionState XenonSession::state() const noexcept {
  std::lock_guard<std::mutex> lock(status_mutex_);
  return state_;
}

std::string XenonSession::last_error() const {
  std::lock_guard<std::mutex> lock(status_mutex_);
  return last_error_;
}

bool XenonSession::is_initialized() const noexcept {
  const auto current = state();
  return current != SessionState::Uninitialized && current != SessionState::Failed;
}

bool XenonSession::is_running() const noexcept {
  return state() == SessionState::Running;
}

filesystem::VirtualFileSystem* XenonSession::filesystem() noexcept {
  return filesystem_.get();
}

kernel::KernelIoManager* XenonSession::kernel_io() noexcept {
  return kernel_io_.get();
}

const xbox::LoadedXex* XenonSession::loaded_xex() const noexcept {
  return loaded_xex_ ? &(*loaded_xex_) : nullptr;
}

void XenonSession::set_state(SessionState new_state, std::string message) {
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    state_ = new_state;
  }
  if (config_.enable_logging && !message.empty()) {
    std::scoped_lock console_log_lock(console_log_mutex());
    std::cout << "[XenonSession] " << message << std::endl;
  }
}

void XenonSession::set_error(std::string error) {
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    last_error_ = error;
    state_ = SessionState::Failed;
  }
  if (config_.enable_logging) {
    std::scoped_lock console_log_lock(console_log_mutex());
    std::cout << "[XenonSession] " << error << std::endl;
  }
}

SessionResult XenonSession::initialize(const SessionConfig& config) {
  if (is_initialized()) {
    return SessionResult::failure("Session already initialized");
  }

  set_state(SessionState::Initializing, "Initializing session...");
  config_ = config;
  export_trace_.clear();
  export_trace_.set_enabled(config_.enable_export_trace);
  memory_watch_.configure(config_.memory_watch_addresses, config_.memory_watch_history);
  {
    std::lock_guard<std::mutex> observation_lock(adaptive_observation_mutex_);
    adaptive_observation_seen_.clear();
    dynamic_fallback_observation_seen_.clear();
  }
  boot_checkpoints_.reset();

  if (!init_memory()) {
    return SessionResult::failure("Failed to initialize memory subsystem");
  }

  if (!init_filesystem()) {
    return SessionResult::failure("Failed to initialize filesystem subsystem");
  }

  if (!init_kernel()) {
    return SessionResult::failure("Failed to initialize kernel subsystem");
  }

  if (!init_cpu()) {
    return SessionResult::failure("Failed to initialize CPU subsystem");
  }

  if (config_.enable_graphics && !init_gpu()) {
    return SessionResult::failure("Failed to initialize GPU subsystem");
  }

  if (config_.enable_input && !init_input()) {
    return SessionResult::failure("Failed to initialize input subsystem");
  }

  if (config_.enable_audio && !init_audio()) {
    return SessionResult::failure("Failed to initialize audio subsystem");
  }

  if (!init_xam()) {
    return SessionResult::failure("Failed to initialize XAM subsystem");
  }

  if (!init_exports()) {
    return SessionResult::failure("Failed to initialize export registry");
  }

  set_state(SessionState::Ready, "Session initialized successfully");
  return SessionResult::ok("Session ready", SessionState::Ready);
}

}  // namespace xenon::core

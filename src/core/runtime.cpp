#include "xenon/core/runtime.hpp"

#include "xenon/core/session.hpp"

#include <iostream>

namespace xenon {

Runtime::Runtime() = default;

Runtime::~Runtime() {
  shutdown();
}

bool Runtime::initialize(const RuntimeConfig& config) {
  if (initialized_) {
    return true;
  }

  config_ = config;

  if (config_.enable_logging) {
    std::cout << "[Xenon] Initializing runtime...\n";
  }

  // Create and initialize the session
  session_ = std::make_unique<core::XenonSession>();
  
  core::SessionConfig session_config{};
  session_config.enable_graphics = config_.enable_graphics;
  session_config.enable_input = config_.enable_input;
  session_config.enable_audio = config_.enable_audio;
  session_config.enable_network = config_.enable_network;
  session_config.enable_logging = config_.enable_logging;

  auto result = session_->initialize(session_config);
  if (!result.success) {
    std::cerr << "[Xenon] Failed to initialize session: " << result.message << "\n";
    session_.reset();
    return false;
  }

  // If legacy VFS was set, use the session's filesystem instead
  if (vfs_ && session_->filesystem()) {
    // Legacy VFS compatibility - not fully implemented yet
    if (config_.enable_logging) {
      std::cout << "[Xenon] Note: Legacy VFS is deprecated. Use session()->filesystem() instead.\n";
    }
  }

  initialized_ = true;

  if (config_.enable_logging) {
    std::cout << "[Xenon] Runtime initialized.\n";
  }

  return true;
}

void Runtime::shutdown() {
  if (!initialized_) {
    return;
  }

  if (config_.enable_logging) {
    std::cout << "[Xenon] Shutting down runtime...\n";
  }

  if (session_) {
    session_->shutdown();
    session_.reset();
  }

  initialized_ = false;
}

bool Runtime::is_initialized() const noexcept {
  return initialized_;
}

void Runtime::set_filesystem(std::shared_ptr<filesystem::VirtualFileSystem> vfs) {
  vfs_ = vfs;

  if (config_.enable_logging && vfs) {
    std::cout << "[Xenon] VFS connected to runtime (legacy).\n";
  }
}

std::shared_ptr<filesystem::VirtualFileSystem> Runtime::filesystem() const {
  // Try to return the session's filesystem first
  if (session_ && session_->filesystem()) {
    // Wrap it in a shared_ptr - this is a hack for legacy compatibility
    // In the future, callers should use session()->filesystem() directly
    return std::shared_ptr<filesystem::VirtualFileSystem>(
        session_->filesystem(), [](filesystem::VirtualFileSystem*) {
          // Don't delete - owned by session
        });
  }
  return vfs_;
}

}  // namespace xenon

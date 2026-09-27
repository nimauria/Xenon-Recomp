#pragma once

#include <cstdint>
#include <string>

#include "launch_config.hpp"

namespace xenon::core {
class XenonSession;
}

namespace xenon::runtime_host {

// Structured launch-failure categories (Part 5.7 of the Gracemeria readiness
// pass). Written into status.json's "errorCategory" field alongside the
// existing free-text "lastError"/message, so a launcher/dashboard can branch
// on failure kind (e.g. offer "reinstall content" for MissingRequiredContent)
// without string-matching human-readable text.
enum class LaunchFailureCategory {
  None,
  SessionInitFailed,
  MissingRequiredContent,
  ContentMountFailed,
  GameLoadFailed,
  ModuleRevisionMismatch,
  StartFailed,
  WindowCreationFailed,
  GraphicsPresentationFailed,
  AudioBackendFailed,
  InputBackendFailed,
};

[[nodiscard]] const char* launch_failure_category_name(LaunchFailureCategory category) noexcept;

// Periodically-written session status snapshot the launcher polls instead of
// talking to XenonSession directly (there is no cross-process XenonSession
// access - see docs/runtime/RUNTIME_HOST.md). Written atomically (temp file +
// rename) so the launcher never observes a half-written file.
class StatusWriter {
 public:
  StatusWriter(std::string session_dir, LaunchConfig config);

  // Not const: XenonSession's subsystem accessors (memory(), input(), ...)
  // are non-const by design (they hand out mutable subsystem pointers), so a
  // read-only status snapshot still needs a non-const reference.
  void write(core::XenonSession& session, const std::string& phase_message = {});
  void write_fatal(const std::string& message,
                   LaunchFailureCategory category = LaunchFailureCategory::None);

  [[nodiscard]] std::string status_path() const;
  [[nodiscard]] std::string stop_signal_path() const;
  [[nodiscard]] bool stop_requested() const;

 private:
  std::string session_dir_;
  LaunchConfig config_;
  std::int64_t started_at_epoch_ms_;
};

}  // namespace xenon::runtime_host

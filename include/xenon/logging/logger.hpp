#pragma once

// Minimal leveled, category-tagged diagnostic logger (Phase 0 of the AC6
// Runtime Readiness / Platform Fidelity pass). Every later diagnostic
// addition in that pass (import capability audit, fallback accounting, GPU
// telemetry, boot checkpoints, kernel-object liveness, ...) routes its
// human-readable log lines through this instead of raw std::cout, so log
// verbosity is controlled in one place and message construction can be
// skipped entirely when a level is disabled.

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace xenon::logging {

enum class Level : std::uint8_t {
  Trace = 0,
  Debug = 1,
  Info = 2,
  Warning = 3,
  Error = 4,
  Fatal = 5,
};

// Below the configured minimum level, logging costs one relaxed atomic load
// plus a comparison - no allocation, no formatting. Callers that build
// expensive messages should use log_if_enabled() (message built by a lambda,
// only invoked when the level actually passes) rather than log(), so a
// disabled Trace/Debug call is genuinely free in a release build. Default
// minimum is Warning: routine diagnostics stay silent unless a caller opts
// in, matching this pass's "diagnostics must be cheap in release" constraint
// (spec Part 19).
class Logger {
 public:
  static Logger& instance();

  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  void set_min_level(Level level) noexcept { min_level_.store(level, std::memory_order_relaxed); }
  [[nodiscard]] Level min_level() const noexcept {
    return min_level_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] bool enabled(Level level) const noexcept {
    return level >= min_level_.load(std::memory_order_relaxed);
  }

  // Logs unconditionally. Prefer log_if_enabled() at call sites where
  // `message` is not already a cheap string_view/literal.
  void log(Level level, std::string_view category, std::string_view message);

  template <typename MessageFn>
  void log_if_enabled(Level level, std::string_view category, MessageFn&& make_message) {
    if (!enabled(level)) return;
    log(level, category, std::forward<MessageFn>(make_message)());
  }

  // Overrides where log entries go; the default (no sink set) writes one
  // JSON object per line to stdout (stderr for Error/Fatal). Tests and the
  // capability-report machinery can install a sink to capture entries
  // in-process instead of parsing stdout.
  using Sink = std::function<void(Level, std::string_view, std::string_view)>;
  void set_sink(Sink sink);

  // Total log() invocations that actually reached a sink (i.e. passed the
  // enabled() gate). Used by tests to prove a disabled-level call chain did
  // no work.
  [[nodiscard]] std::uint64_t call_count() const noexcept {
    return call_count_.load(std::memory_order_relaxed);
  }

 private:
  Logger() = default;

  std::atomic<Level> min_level_{Level::Warning};
  std::atomic<std::uint64_t> call_count_{0};
  std::mutex sink_mutex_;
  Sink sink_;
};

}  // namespace xenon::logging

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
#include <fstream>
#include <functional>
#include <map>
#include <memory>
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

  // Also appends every entry at `min_level` or above to a file per category
  // in `directory` ("gpu" -> gpu.log, "filesystem" -> filesystem.log), one
  // "<UTC time> <level>: <message>" line each, flushed as written, so a run's
  // problems can be read back subsystem by subsystem after it ends. Works
  // alongside the sink. Characters other than letters, digits, '-' and '_' in
  // a category become '_' in its file name; an empty category goes to
  // general.log. Returns false (keeping no files) if `directory` cannot be
  // created; an empty path closes every file.
  bool set_error_directory(const std::string& directory, Level min_level = Level::Warning);
  // Lines written to the per-category error files so far.
  [[nodiscard]] std::uint64_t error_file_lines() const noexcept {
    return error_file_lines_.load(std::memory_order_relaxed);
  }
  // The file a category's entries go to, relative to the error directory.
  [[nodiscard]] static std::string error_file_name(std::string_view category);

  // Total log() invocations that actually reached a sink (i.e. passed the
  // enabled() gate). Used by tests to prove a disabled-level call chain did
  // no work.
  [[nodiscard]] std::uint64_t call_count() const noexcept {
    return call_count_.load(std::memory_order_relaxed);
  }

  // The one lock every writer of std::cout/std::cerr in this process must
  // hold for the full duration of a write. Concurrent, unsynchronized writes
  // to a freopen'd stdout/stderr from multiple guest threads have been
  // observed (via a real debugger stack trace: MSVCP140 basic_filebuf::xsputn
  // -> ucrtbase!fwrite -> isatty_proc -> invalid_parameter_noinfo) to race the
  // CRT's own lazy console-vs-file buffering-mode detection and fast-fail the
  // whole process with STATUS_STACK_BUFFER_OVERRUN. log()'s own default
  // (no-sink) path uses this; any other subsystem writing directly to
  // std::cout/std::cerr (see core::XenonSession::console_log_mutex(), which
  // forwards here) must use the same lock rather than one of its own.
  [[nodiscard]] static std::mutex& stream_mutex() noexcept {
    return instance().stream_mutex_;
  }

 private:
  Logger() = default;

  std::atomic<Level> min_level_{Level::Warning};
  std::atomic<std::uint64_t> call_count_{0};
  std::mutex sink_mutex_;
  std::mutex stream_mutex_;
  Sink sink_;
  std::mutex error_file_mutex_;
  std::string error_directory_;
  std::map<std::string, std::unique_ptr<std::ofstream>, std::less<>> error_files_;
  Level error_file_level_{Level::Warning};
  std::atomic<std::uint64_t> error_file_lines_{0};
};

}  // namespace xenon::logging

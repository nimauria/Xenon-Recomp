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
#include <unordered_map>
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
  //
  // A problem hit every frame must not fill the disk or bury the rest of the
  // file: an entry identical to an earlier one (same category, level and
  // message) is written for its first kRepeatsWritten occurrences, then once
  // per kRepeatInterval further ones with "(repeated N times)" appended. A
  // category file stops at `file_byte_limit` with a final line saying so.
  bool set_error_directory(const std::string& directory, Level min_level = Level::Warning,
                           std::uint64_t file_byte_limit = kErrorFileByteLimit);
  [[nodiscard]] std::string error_directory() const;
  static constexpr std::uint64_t kRepeatsWritten = 20u;
  static constexpr std::uint64_t kRepeatInterval = 1000u;
  static constexpr std::uint64_t kErrorFileByteLimit = std::uint64_t{64} << 20;

  // Writes one entry to its category's error file only - not to the sink or
  // the console - for problems a caller already prints in its own form.
  // Same level filter, repeat handling and size limit as log().
  void record(Level level, std::string_view category, std::string_view message);
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

  struct ErrorFile {
    std::ofstream stream;
    std::uint64_t bytes{};
  };
  void write_error_file(Level level, std::string_view category, std::string_view message);

  std::atomic<Level> min_level_{Level::Warning};
  std::atomic<std::uint64_t> call_count_{0};
  std::mutex sink_mutex_;
  std::mutex stream_mutex_;
  Sink sink_;
  mutable std::mutex error_file_mutex_;
  std::string error_directory_;
  std::map<std::string, std::unique_ptr<ErrorFile>, std::less<>> error_files_;
  // Occurrences of each distinct entry this run, keyed by
  // "<level>\n<category>\n<message>".
  std::unordered_map<std::string, std::uint64_t> error_repeats_;
  Level error_file_level_{Level::Warning};
  std::uint64_t error_file_byte_limit_{kErrorFileByteLimit};
  std::atomic<std::uint64_t> error_file_lines_{0};
};

}  // namespace xenon::logging

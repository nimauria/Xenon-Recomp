#pragma once

// Host crash reports: an unhandled host exception (a stack overflow, an
// access violation outside guest memory handling, an uncaught C++ exception)
// ends the process without reaching any of the session's own reporting, so
// the runtime host writes one line about it to crash.log in the run's error
// folder before the process goes. The writer allocates nothing and formats
// into static storage, since it may run on an exhausted stack.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace xenon::runtime_host {

struct CrashInfo {
  std::uint32_t code{};        // NTSTATUS / exception code, or the signal number
  std::uint64_t address{};     // where it happened
  std::string_view module{};   // file name of the module holding `address`
  std::uint64_t module_offset{};
  std::uint64_t thread_id{};
  // For an access violation: 0 read, 1 write, 8 execute, and the address used.
  bool has_access{};
  std::uint32_t access_kind{};
  std::uint64_t access_address{};
  bool is_signal{};
};

// A name for a crash code: "stack overflow", "access violation", ... or empty.
[[nodiscard]] std::string_view crash_code_name(std::uint32_t code, bool is_signal) noexcept;

// Formats "<UTC stamp> fatal: host process crashed: ..." plus a newline into
// `out` (truncating to `capacity`), returning the length written. Uses no
// heap and little stack.
std::size_t format_crash_line(char* out, std::size_t capacity, const CrashInfo& info,
                              std::int64_t unix_seconds, std::uint32_t milliseconds) noexcept;

// Appends one formatted line for `info` to the installed crash.log.
void write_crash_report(const CrashInfo& info) noexcept;

// Points crash reports at <run_dir>/crash.log and installs the process-wide
// handlers (an unhandled-exception filter on Windows, fatal-signal handlers
// with an alternate stack elsewhere, and a std::terminate handler). The
// handlers record the crash and let it continue to end the process with its
// own code. Returns false if `run_dir` is empty.
bool install_crash_report(const std::filesystem::path& run_dir);

// The crash.log path crash reports go to, or empty before installation.
[[nodiscard]] std::filesystem::path crash_report_path();

}  // namespace xenon::runtime_host

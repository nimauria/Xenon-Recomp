#pragma once

// Investigation probes: append-only diagnostic files written to the process
// working directory (for example `vsync_cb_diag.log`). They were added while
// tracing Ace Combat 6 runtime stalls and are kept as evidence for that
// investigation. High-volume paths record bounded events instead
// (xenon/logging/diagnostic_events.hpp).
//
// Probes are off unless XENON_PROBE_LOGS=1 is set in the environment (or
// set_probe_logs_enabled(true) is called). Each file is truncated by its
// first record in a process and holds at most kProbeLogByteLimit bytes per
// run; the last line then says the log was truncated.

#include <cstddef>
#include <cstdint>

#if defined(__GNUC__) || defined(__clang__)
#define XENON_PROBE_LOG_FORMAT(format_index, args_index) \
  __attribute__((format(printf, format_index, args_index)))
#else
#define XENON_PROBE_LOG_FORMAT(format_index, args_index)
#endif

namespace xenon::logging {

inline constexpr std::uint64_t kProbeLogByteLimit = std::uint64_t{4} << 20;

[[nodiscard]] bool probe_logs_enabled() noexcept;
void set_probe_logs_enabled(bool enabled) noexcept;

// Writes one printf-formatted record (at most 1 KiB) to `file_name`. A no-op
// while probes are off or once the file reached its limit; a file that cannot
// be opened is skipped.
void append_probe_log(const char* file_name, const char* format, ...)
    XENON_PROBE_LOG_FORMAT(2, 3);

}  // namespace xenon::logging

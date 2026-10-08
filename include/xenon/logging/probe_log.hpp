#pragma once

// Investigation probes: unconditional, append-only diagnostic files written
// to the process working directory (for example `vsync_cb_diag.log`). They
// were added while tracing Ace Combat 6 runtime stalls and are kept as
// evidence for that investigation. They are not part of the leveled Logger
// and are expected to be removed or folded into it once that investigation
// closes.

#if defined(__GNUC__) || defined(__clang__)
#define XENON_PROBE_LOG_FORMAT(format_index, args_index) \
  __attribute__((format(printf, format_index, args_index)))
#else
#define XENON_PROBE_LOG_FORMAT(format_index, args_index)
#endif

namespace xenon::logging {

// Opens `file_name` for append, writes one printf-formatted record and closes
// it again. A file that cannot be opened is silently skipped.
void append_probe_log(const char* file_name, const char* format, ...)
    XENON_PROBE_LOG_FORMAT(2, 3);

}  // namespace xenon::logging

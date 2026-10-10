#include <atomic>
#include <chrono>
#include <string>

#include "core/session/session_internal.hpp"
#include "xenon/logging/probe_log.hpp"

namespace xenon::core::detail {

// Periodic dump (every ~30s) of EVERY known thread's recent kernel-call
// history, to get a holistic picture of what the whole system is doing
// over time - not just the two known-stuck render-setup threads - in
// case some other thread (asset/content loader, async I/O) stalls
// partway through and is the real upstream blocker.
void snapshot_thread_export_history(const ExportTrace& export_trace) {
  using Clock = std::chrono::steady_clock;
  static std::atomic<int> _all_threads_dump_count{0};
  static const auto _all_threads_trace_start = Clock::now();
  const auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(
                             Clock::now() - _all_threads_trace_start)
                             .count();
  const int expected_dumps = static_cast<int>(elapsed_s / 30) + 1;
  if (logging::probe_logs_enabled() && elapsed_s >= 8 &&
      _all_threads_dump_count.load() < expected_dumps) {
    _all_threads_dump_count.fetch_add(1);
    constexpr const char* kFile = "all_threads_export_trace_diag.log";
    logging::append_probe_log(kFile, "=== snapshot at t=%llds ===\n", (long long)elapsed_s);
    for (std::uint32_t tid = 0; tid <= 30u; ++tid) {
      const auto recent = export_trace.recent_for_thread(tid, 8u);
      if (recent.empty()) continue;
      logging::append_probe_log(kFile, "--- thread_id=%u (last %zu calls) ---\n", tid,
                                recent.size());
      for (const auto& trace : recent) {
        logging::append_probe_log(
            kFile, "  %s!%s lr=0x%08llX r3=0x%08llX r4=0x%08llX -> 0x%08llX\n",
            std::string(trace.library_view()).c_str(),
            trace.name_view().empty() ? "?" : std::string(trace.name_view()).c_str(),
            (unsigned long long)trace.lr, (unsigned long long)trace.arguments[0],
            (unsigned long long)trace.arguments[1], (unsigned long long)trace.result_r3);
      }
    }
  }
}

}  // namespace xenon::core::detail

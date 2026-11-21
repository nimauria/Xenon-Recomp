// Performance watch: hitches.log and slow_events.log (see session.hpp,
// start_perf_watch()). Everything expensive - snapshots of the kernel-call
// trace, symbol folding, file writes - happens on the watch thread; the
// presenting thread and the kernel-call path only hand it a verdict or a
// timed event.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#include "xenon/core/session.hpp"
#include "xenon/gpu/perf_stats.hpp"
#include "xenon/logging/probe_log.hpp"

namespace xenon::core {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kSlowEventNs = 5'000'000u;
constexpr auto kSampleInterval = std::chrono::milliseconds(2);
constexpr std::uint64_t kTallyIntervalNs = 5'000'000'000u;
// A full report (calls, threads) at most this often; other hitches get one line.
constexpr std::uint64_t kFullReportIntervalNs = 1'000'000'000u;
constexpr std::uint64_t kListedCallNs = 1'000'000u;

std::uint64_t to_ns(Clock::time_point when) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(when.time_since_epoch()).count());
}

bool env_off(const char* name) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)  // read once at start, before any thread could set it
#endif
  const char* value = std::getenv(name);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
  return value && std::string_view(value) == "0";
}

}  // namespace

void XenonSession::start_perf_watch() {
  stop_perf_watch();
  if (env_off("XENON_HITCH_LOG")) return;
  perf_watch_start_ = Clock::now();
  perf_watch_enabled_.store(true, std::memory_order_release);
  perf_watch_running_.store(true, std::memory_order_release);
  perf_watch_thread_ = std::thread([this] {
    auto next_tally = to_ns(Clock::now()) + kTallyIntervalNs;
    std::uint64_t last_full_report = 0;
    while (perf_watch_running_.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(kSampleInterval);
      const auto now = to_ns(Clock::now());
      {
        // Where each guest thread is: its last recorded instruction, or 0
        // while it is inside a kernel call. Racy reads of live register
        // files, as in the guest profiler: diagnostic only.
        const auto in_flight = in_flight_exports_.snapshot();
        std::scoped_lock lock(guest_thread_states_mutex_);
        for (const auto& [thread_id, state] : guest_thread_states_) {
          const auto pc = in_flight.contains(thread_id) || !state
                              ? 0u
                              : static_cast<std::uint32_t>(state->cia);
          hitch_monitor_.add_sample({now, thread_id, pc});
        }
      }

      std::vector<PendingHitch> hitches;
      {
        std::scoped_lock lock(pending_hitches_mutex_);
        hitches.swap(pending_hitches_);
      }
      for (const auto& hitch : hitches) {
        const double at_s = static_cast<double>(hitch.at_ns - to_ns(perf_watch_start_)) / 1e9;
        bottleneck_ledger_.add_hitch(hitch.verdict.interval_ms, hitch.verdict.expected_ms);
        logging::append_run_log("hitches.log", "+%.3fs frame took %.1f ms (recent median %.1f ms)\n",
                                at_s, hitch.verdict.interval_ms, hitch.verdict.expected_ms);
        if (hitch.at_ns - last_full_report >= kFullReportIntervalNs) {
          last_full_report = hitch.at_ns;
          try {
            write_hitch_report(hitch.at_ns, hitch.verdict, hitch.frame_line);
          } catch (...) {
            // Diagnostics must never take the session down.
          }
        }
      }

      if (now >= next_tally) {
        next_tally = now + kTallyIntervalNs;
        const auto tallies = hitch_monitor_.take_slow_tallies();
        if (!tallies.empty()) {
          const double at_s = static_cast<double>(now - to_ns(perf_watch_start_)) / 1e9;
          logging::append_run_log("slow_events.log", "+%.3fs over 5 ms in the last 5 s:\n", at_s);
          for (const auto& tally : tallies) {
            bottleneck_ledger_.add_slow(tally.category, tally.name, tally.count,
                                        static_cast<double>(tally.total_ns) / 1e6,
                                        static_cast<double>(tally.max_ns) / 1e6);
            logging::append_run_log(
                "slow_events.log", "    %-8s %-40s x%-5llu total %8.1f ms  max %7.1f ms\n",
                tally.category.c_str(), tally.name.c_str(),
                static_cast<unsigned long long>(tally.count),
                static_cast<double>(tally.total_ns) / 1e6, static_cast<double>(tally.max_ns) / 1e6);
          }
        }
        write_bottleneck_report();
      }
    }
  });
}

void XenonSession::stop_perf_watch() noexcept {
  perf_watch_enabled_.store(false, std::memory_order_release);
  perf_watch_running_.store(false, std::memory_order_release);
  if (perf_watch_thread_.joinable()) {
    perf_watch_thread_.join();
    try {
      for (const auto& tally : hitch_monitor_.take_slow_tallies())
        bottleneck_ledger_.add_slow(tally.category, tally.name, tally.count,
                                    static_cast<double>(tally.total_ns) / 1e6,
                                    static_cast<double>(tally.max_ns) / 1e6);
      write_bottleneck_report();
    } catch (...) {
      // Diagnostics must never take the session down while it stops.
    }
  }
}

void XenonSession::note_presented_frame_for_hitches(Clock::time_point when) {
  if (!perf_watch_enabled_.load(std::memory_order_acquire)) return;
  const auto at = to_ns(when);
  const auto verdict = hitch_monitor_.note_frame(at);
  if (!verdict.hitch) return;
  std::scoped_lock lock(pending_hitches_mutex_);
  if (pending_hitches_.size() < 64u)
    pending_hitches_.push_back({at, verdict, gpu::perf::last_frame_timeline_line()});
}

void XenonSession::note_timed_event(const char* category, std::string_view name,
                                    std::uint64_t duration_ns, std::uint32_t thread_id,
                                    const std::string& detail) {
  if (duration_ns < kSlowEventNs || !perf_watch_enabled_.load(std::memory_order_acquire)) return;
  const std::string label(name);
  if (!hitch_monitor_.note_slow(category, label, duration_ns)) return;
  const double at_s =
      static_cast<double>(to_ns(Clock::now()) - to_ns(perf_watch_start_)) / 1e9;
  std::string thread_name;
  if (thread_id != 0u && kernel_process_) {
    if (const auto* thread = kernel_process_->thread_manager().get_thread_ptr(thread_id))
      thread_name = thread->name();
  }
  logging::append_run_log("slow_events.log", "+%.3fs %-8s %s took %.2f ms on t%u%s%s%s %s\n", at_s,
                          category, label.c_str(), static_cast<double>(duration_ns) / 1e6,
                          static_cast<unsigned>(thread_id), thread_name.empty() ? "" : " (",
                          thread_name.c_str(), thread_name.empty() ? "" : ")", detail.c_str());
}

void XenonSession::write_hitch_report(std::uint64_t at_ns,
                                      const HitchMonitor::FrameVerdict& verdict,
                                      const std::string& frame_line) {
  const auto from = at_ns - static_cast<std::uint64_t>(verdict.interval_ms * 1e6);
  const auto start = to_ns(perf_watch_start_);
  const auto rel = [&](std::uint64_t ns) { return static_cast<double>(ns - start) / 1e9; };
  const auto write = [](const char* format, auto... args) {
    logging::append_run_log("hitches.log", format, args...);
  };
  write("  report for the frame from +%.3fs to +%.3fs:\n", rel(from), rel(at_ns));
  if (!frame_line.empty()) write("    gpu: %s", frame_line.c_str());

  std::unordered_map<std::uint32_t, std::string> names;
  const auto name_of = [&](std::uint32_t thread_id) -> const std::string& {
    auto [it, inserted] = names.try_emplace(thread_id);
    if (inserted && kernel_process_) {
      if (const auto* thread = kernel_process_->thread_manager().get_thread_ptr(thread_id))
        it->second = thread->name();
    }
    return it->second;
  };

  // Kernel calls that overlapped the frame and took over a millisecond.
  if (export_trace_.enabled()) {
    auto calls = export_trace_.recent_global(export_trace_.capacity());
    std::erase_if(calls, [&](const ExportTraceRecord& call) {
      return call.duration_ns < kListedCallNs || call.start_ns + call.duration_ns < from ||
             call.start_ns >= at_ns;
    });
    // Waits are how idle worker threads spend a frame; they are counted, not
    // listed, so the calls that held a thread up stand out.
    const auto is_wait = [](const ExportTraceRecord& call) {
      const auto name = call.name_view();
      return name.find("Wait") != std::string_view::npos ||
             name.find("Delay") != std::string_view::npos ||
             name.find("Sleep") != std::string_view::npos;
    };
    std::size_t waits = 0;
    double wait_ms = 0;
    for (const auto& call : calls) {
      if (!is_wait(call)) continue;
      ++waits;
      wait_ms += static_cast<double>(call.duration_ns) / 1e6;
    }
    std::erase_if(calls, is_wait);
    std::sort(calls.begin(), calls.end(),
              [](const auto& a, const auto& b) { return a.duration_ns > b.duration_ns; });
    write("    waits over 1 ms in the frame: %zu (%.1f ms in all threads)\n", waits, wait_ms);
    write("    other kernel calls over 1 ms in the frame: %zu\n", calls.size());
    for (std::size_t i = 0; i < calls.size() && i < 24u; ++i) {
      const auto& call = calls[i];
      write("      t%-3u %-16s %-36s %8.2f ms from +%.3fs lr=%08llX r3=%llX r4=%llX\n",
            static_cast<unsigned>(call.thread_id), name_of(call.thread_id).c_str(),
            std::string(call.name_view().empty() ? call.library_view() : call.name_view()).c_str(),
            static_cast<double>(call.duration_ns) / 1e6, rel(call.start_ns),
            static_cast<unsigned long long>(call.lr),
            static_cast<unsigned long long>(call.arguments[0]),
            static_cast<unsigned long long>(call.arguments[1]));
    }
  }

  // Calls still in progress (a thread blocked across the whole frame).
  const auto now = Clock::now();
  for (const auto& [thread_id, call] : in_flight_exports_.snapshot()) {
    const auto held = std::chrono::duration<double, std::milli>(now - call.since).count();
    if (held < 5.0) continue;
    if (call.descriptor) {
      const std::string_view name = call.descriptor->name;
      if (name.find("Wait") != std::string_view::npos ||
          name.find("Delay") != std::string_view::npos ||
          name.find("Sleep") != std::string_view::npos)
        continue;
    }
    write("    t%-3u %-16s inside %s for %.1f ms (lr=%08llX)\n", static_cast<unsigned>(thread_id),
          name_of(thread_id).c_str(),
          call.descriptor ? call.descriptor->name.c_str() : "a kernel call", held,
          static_cast<unsigned long long>(call.lr));
  }

  // What each guest thread ran during the frame, by guest function (the
  // executable's .pdata), from the 2 ms samples.
  const auto fold = [this](std::uint32_t pc) -> std::uint32_t {
    if (pc == 0u || !loaded_xex_) return pc;
    if (const auto* function = xbox::function_metadata_containing(loaded_xex_->image, pc))
      return static_cast<std::uint32_t>(function->begin);
    return pc;
  };
  const auto activity = hitch_monitor_.activity_between(from, at_ns, 4, fold);
  write("    guest threads (a sample every 2 ms; \"kernel\" = inside a kernel call):\n");
  for (const auto& thread : activity) {
    std::string line;
    char part[64];
    for (const auto& [function, count] : thread.top) {
      const auto percent = 100u * count / std::max<std::uint32_t>(thread.samples, 1u);
      if (function == 0u)
        std::snprintf(part, sizeof(part), " %u%% kernel", percent);
      else
        std::snprintf(part, sizeof(part), " %u%% %08X", percent, function);
      line += part;
    }
    write("      t%-3u %-16s %3u samples:%s\n", static_cast<unsigned>(thread.thread),
          name_of(thread.thread).c_str(), thread.samples, line.c_str());
  }
}

}  // namespace xenon::core

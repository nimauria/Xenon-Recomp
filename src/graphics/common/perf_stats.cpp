#include "xenon/gpu/perf_stats.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <mutex>
#include <vector>

#include "xenon/logging/probe_log.hpp"

namespace xenon::gpu::perf {
namespace {

using Clock = std::chrono::steady_clock;

std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Counter::Count)> g_counters{};

// Frame intervals of the current window, kept by the presenting thread and
// taken by the flushing one.
std::mutex g_frames_mutex;
std::vector<double> g_frame_ms;
Clock::time_point g_last_frame{};

Clock::time_point g_window_start{};

std::uint64_t take(Counter counter) noexcept {
  return g_counters[static_cast<std::size_t>(counter)].exchange(0u, std::memory_order_relaxed);
}

double ms(std::uint64_t ns) { return static_cast<double>(ns) / 1e6; }

double percentile(const std::vector<double>& sorted, double p) {
  if (sorted.empty()) return 0.0;
  const auto index = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1) + 0.5);
  return sorted[std::min(index, sorted.size() - 1)];
}

}  // namespace

void add(Counter counter, std::uint64_t value) noexcept {
  g_counters[static_cast<std::size_t>(counter)].fetch_add(value, std::memory_order_relaxed);
}

std::uint64_t peek(Counter counter) noexcept {
  return g_counters[static_cast<std::size_t>(counter)].load(std::memory_order_relaxed);
}

void note_draw_run(std::uint64_t draws) noexcept {
  const auto bucket = draws == 0u     ? Counter::DrawRuns0
                      : draws == 1u   ? Counter::DrawRuns1
                      : draws < 4u    ? Counter::DrawRuns2to3
                      : draws < 16u   ? Counter::DrawRuns4to15
                      : draws < 64u   ? Counter::DrawRuns16to63
                      : draws < 256u  ? Counter::DrawRuns64to255
                                      : Counter::DrawRuns256Plus;
  add(bucket, 1u);
}

void note_presented_frame(Clock::time_point when) {
  std::scoped_lock lock(g_frames_mutex);
  if (g_last_frame != Clock::time_point{})
    g_frame_ms.push_back(std::chrono::duration<double, std::milli>(when - g_last_frame).count());
  g_last_frame = when;
}

std::string take_line(double seconds) {
  std::vector<double> frames;
  {
    std::scoped_lock lock(g_frames_mutex);
    frames.swap(g_frame_ms);
  }
  std::sort(frames.begin(), frames.end());
  const double per = seconds > 0.0 ? 1.0 / seconds : 0.0;
  char line[2048];
  std::snprintf(line, sizeof(line),
                "frames %.1f/s ms p50 %.1f p95 %.1f p99 %.1f max %.1f | swaps %.1f/s | ring %.0f ms "
                "ir %.0f ms present %.0f ms | fence %.0f ms (%llu waits) | shader %llu (%.0f ms) "
                "pso %llu (%.0f ms) | draws %.0f/s resolves %.0f/s | readback %.1f MB wait %.0f ms "
                "(%llu) window %.0f ms | backend %.0f ms wait %.0f ms (%llu) | submit %llu "
                "(%.0f ms) | holds barrier %llu sync %llu read %llu drain %llu | bindings reused "
                "%llu written %llu | shader wait %.0f ms (%llu) | gpu busy %.0f ms idle %.0f ms "
                "(%llu batches) | indices reused %llu converted %llu (%.1f MB) | bounds %llu "
                "(%.1f ms) | resident %llu (%.1f MB) | edram cpu %llu (%.1f MB, %.0f ms) | rptr "
                "early %llu held %llu | runs 0:%llu 1:%llu 2-3:%llu 4-15:%llu 16-63:%llu "
                "64-255:%llu 256+:%llu\n",
                static_cast<double>(frames.size()) * per, percentile(frames, 0.5),
                percentile(frames, 0.95), percentile(frames, 0.99), frames.empty() ? 0.0 : frames.back(),
                static_cast<double>(take(Counter::Swaps)) * per, ms(take(Counter::RingNs)),
                ms(take(Counter::IrNs)), ms(take(Counter::PresentNs)),
                ms(take(Counter::FenceWaitNs)),
                static_cast<unsigned long long>(take(Counter::FenceWaits)),
                static_cast<unsigned long long>(take(Counter::ShaderCompiles)),
                ms(take(Counter::ShaderCompileNs)),
                static_cast<unsigned long long>(take(Counter::PipelineCreates)),
                ms(take(Counter::PipelineCreateNs)),
                static_cast<double>(take(Counter::Draws)) * per,
                static_cast<double>(take(Counter::Resolves)) * per,
                static_cast<double>(take(Counter::ReadbackBytes)) / (1024.0 * 1024.0),
                ms(take(Counter::ReadbackWaitNs)),
                static_cast<unsigned long long>(take(Counter::ReadbackWaits)),
                ms(take(Counter::ReadbackWindowNs)), ms(take(Counter::BackendNs)),
                ms(take(Counter::BackendWaitNs)),
                static_cast<unsigned long long>(take(Counter::BackendWaits)),
                static_cast<unsigned long long>(take(Counter::Submits)),
                ms(take(Counter::SubmitNs)),
                static_cast<unsigned long long>(take(Counter::HoldsBarrier)),
                static_cast<unsigned long long>(take(Counter::HoldsSync)),
                static_cast<unsigned long long>(take(Counter::HoldsRead)),
                static_cast<unsigned long long>(take(Counter::HoldsDrain)),
                static_cast<unsigned long long>(take(Counter::BindingsReused)),
                static_cast<unsigned long long>(take(Counter::BindingsWritten)),
                ms(take(Counter::ShaderWaitNs)),
                static_cast<unsigned long long>(take(Counter::ShaderWaits)),
                ms(take(Counter::GpuBusyNs)), ms(take(Counter::GpuIdleNs)),
                static_cast<unsigned long long>(take(Counter::GpuBatches)),
                static_cast<unsigned long long>(take(Counter::IndexCacheHits)),
                static_cast<unsigned long long>(take(Counter::IndexCacheMisses)),
                static_cast<double>(take(Counter::IndexBytesConverted)) / (1024.0 * 1024.0),
                static_cast<unsigned long long>(take(Counter::BoundsEvaluations)),
                ms(take(Counter::BoundsNs)),
                static_cast<unsigned long long>(take(Counter::ResidentIndexDraws)),
                static_cast<double>(take(Counter::ResidentIndexBytes)) / (1024.0 * 1024.0),
                static_cast<unsigned long long>(take(Counter::EdramCpuTransfers)),
                static_cast<double>(take(Counter::EdramCpuBytes)) / (1024.0 * 1024.0),
                ms(take(Counter::EdramCpuNs)),
                static_cast<unsigned long long>(take(Counter::ReadPointerEarly)),
                static_cast<unsigned long long>(take(Counter::ReadPointerHeld)),
                static_cast<unsigned long long>(take(Counter::DrawRuns0)),
                static_cast<unsigned long long>(take(Counter::DrawRuns1)),
                static_cast<unsigned long long>(take(Counter::DrawRuns2to3)),
                static_cast<unsigned long long>(take(Counter::DrawRuns4to15)),
                static_cast<unsigned long long>(take(Counter::DrawRuns16to63)),
                static_cast<unsigned long long>(take(Counter::DrawRuns64to255)),
                static_cast<unsigned long long>(take(Counter::DrawRuns256Plus)));
  return line;
}

void flush_if_due(Clock::time_point now) {
  if (g_window_start == Clock::time_point{}) {
    g_window_start = now;
    return;
  }
  const double seconds = std::chrono::duration<double>(now - g_window_start).count();
  if (seconds < 1.0) return;
  g_window_start = now;
  const auto line = take_line(seconds);
  logging::append_run_log("perf_frame.log", "%s", line.c_str());
}

}  // namespace xenon::gpu::perf

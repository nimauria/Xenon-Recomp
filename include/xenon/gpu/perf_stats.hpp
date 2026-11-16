#pragma once

// Always-on GPU frame statistics: where a frame's time goes, one line a
// second in the run's perf_frame.log (logging::append_run_log).
//
// Counters are process-wide relaxed atomics, so recording costs one atomic
// add and timing one steady_clock read at each end; nothing is recorded per
// draw beyond counting it. The GPU worker calls flush_if_due() once per tick,
// which writes the line for the second that has passed and resets the
// counters.
//
// Columns:
//   frames      presented frames in the second; ms p50/p95/p99/max between them
//   swaps       guest VdSwap calls (the frames the title finished)
//   ring/ir     time the worker spent decoding the ring / playing IR
//   present     time in the host present (DXGI/Vulkan, including its wait)
//   fence       time the CPU blocked on the host GPU, and how many waits
//   shader      shader compiles on the draw path: count and time
//   pso         pipeline creations: count and time
//   shader wait time draws waited for a shader still being lowered or
//               compiled in the background (and how often)
//   gpu         GPU timestamps around every submitted batch: time the GPU
//               spent executing batches ("busy"), the gaps between one
//               batch ending and the next starting ("idle": the GPU waiting
//               for work), and how many batches were measured
//   draws/resolves  per second
//   readback    MB of GPU-written bytes copied into guest memory; the time the
//               GPU worker waited for them (and how often); the time guest
//               CPU writes were held off while they were written ("window")
//   backend     time the backend thread spent consuming IR, recording and
//               presenting (BackendExecutor); the time the frontend blocked
//               waiting for it (and how often) at guest-visible barriers
//   submit      native queue submissions (ExecuteCommandLists / vkQueueSubmit2)
//               and the time spent in them
//   bindings    draws that reused an earlier draw's descriptor tables / set,
//               and draws that copied or wrote their own
//   indices     processed-index cache: draws whose converted indices were
//               reused, draws that converted them, and the MiB converted
//   bounds      draws whose vertices ran on the CPU to bound their render
//               target (VertexPositionEvaluator), and the time spent
//   resident    indexed draws drawn from GPU-resident indices (no upload),
//               and MiB of indices copied into the resident buffer
//   edram cpu   ownership changes that went through canonical EDRAM on the
//               CPU (readback, repack, upload): count, MiB and time
//   edram gpu   canonical EDRAM loaded into native images on the GPU (tiles
//               copied as they are, unpacked in a shader): count, MiB of
//               tiles and the host time to record them
//   holds       times the frontend had to wait for a busy backend, by cause:
//               a guest-visible barrier (interrupt, flip, wait, IM_STORE), a
//               sync point, a read of guest memory behind a deferred write,
//               and the end of a drain
//   rptr        ring drains whose read pointer went back to the guest as
//               soon as they were decoded ("early"), and those held behind
//               their work (a draw reading command memory, or an earlier
//               held write-back still pending)
//   runs        draw runs - the draws between two resolves - by size: how
//               much work each render pass gives a native batch

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace xenon::gpu::perf {

enum class Counter : std::uint32_t {
  Swaps,
  RingNs,
  IrNs,
  PresentNs,
  FenceWaitNs,
  FenceWaits,
  ShaderCompileNs,
  ShaderCompiles,
  PipelineCreateNs,
  PipelineCreates,
  Draws,
  Resolves,
  ReadbackBytes,
  ReadbackWaitNs,
  ReadbackWaits,
  ReadbackWindowNs,
  BackendNs,
  BackendWaitNs,
  BackendWaits,
  Submits,
  SubmitNs,
  HoldsBarrier,
  HoldsSync,
  HoldsRead,
  HoldsDrain,
  BindingsReused,
  BindingsWritten,
  ShaderWaitNs,
  ShaderWaits,
  GpuBusyNs,
  GpuIdleNs,
  GpuBatches,
  IndexCacheHits,
  IndexCacheMisses,
  IndexBytesConverted,
  BoundsEvaluations,
  BoundsNs,
  ResidentIndexDraws,
  ResidentIndexBytes,
  EdramCpuTransfers,
  EdramCpuBytes,
  EdramCpuNs,
  EdramGpuLoads,
  EdramGpuLoadBytes,
  EdramGpuLoadNs,
  ReadPointerEarly,
  ReadPointerHeld,
  // Draw-run histogram buckets (note_draw_run()): 0, 1, 2-3, 4-15, 16-63,
  // 64-255, 256+ draws.
  DrawRuns0,
  DrawRuns1,
  DrawRuns2to3,
  DrawRuns4to15,
  DrawRuns16to63,
  DrawRuns64to255,
  DrawRuns256Plus,
  Count,
};

void add(Counter counter, std::uint64_t value) noexcept;
[[nodiscard]] std::uint64_t peek(Counter counter) noexcept;

// Adds the scope's duration in nanoseconds to `counter` (and one to `count`,
// when given) on destruction.
class ScopedTime {
 public:
  explicit ScopedTime(Counter counter, Counter count = Counter::Count) noexcept
      : counter_(counter), count_(count), start_(std::chrono::steady_clock::now()) {}
  ~ScopedTime() {
    add(counter_, static_cast<std::uint64_t>(
                      std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - start_)
                          .count()));
    if (count_ != Counter::Count) add(count_, 1u);
  }
  ScopedTime(const ScopedTime&) = delete;
  ScopedTime& operator=(const ScopedTime&) = delete;

 private:
  Counter counter_;
  Counter count_;
  std::chrono::steady_clock::time_point start_;
};

// A run of `draws` draws ended at a resolve: counted in its size bucket.
void note_draw_run(std::uint64_t draws) noexcept;

// A frame reached the screen (called by the presenting thread only).
void note_presented_frame(std::chrono::steady_clock::time_point when);

// Formats the line for the window since the last flush and resets it;
// exposed for tests. `seconds` is the window's length.
[[nodiscard]] std::string take_line(double seconds);

// Writes take_line() to perf_frame.log once a second has passed since the
// last write (called by the GPU worker each tick).
void flush_if_due(std::chrono::steady_clock::time_point now);

}  // namespace xenon::gpu::perf

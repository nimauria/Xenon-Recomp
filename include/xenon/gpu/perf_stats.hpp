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
//   edram gpu store  native images flushed to canonical EDRAM on the GPU
//               (packed in a compute shader, tiles read back as they are):
//               count, MiB of tiles and the time including the GPU wait
//   holds      times the frontend had to wait for a busy backend, by cause:
//               a guest-visible barrier (interrupt, flip, wait, IM_STORE), a
//               sync point, a read of guest memory behind a deferred write,
//               and the end of a drain
//   rptr        ring drains whose read pointer went back to the guest as
//               soon as they were decoded ("early"), and those held behind
//               their work (a draw reading command memory, or an earlier
//               held write-back still pending)
//   runs        draw runs - the draws between two resolves - by size: how
//               much work each render pass gives a native batch
//   queue       backend work queue: the deepest it got (tasks waiting) and
//               its mean depth when work was posted
//   dedup       constant slots uploaded vs reused, command-list state sets
//               recorded vs skipped, texture-tracker writes (and textures
//               they touched), IM_LOAD cache hits/misses, mirror copy
//               batches (and the dirty ranges they carried)
//   barriers    the guest-visible barriers that held the frontend for a busy
//               backend, by kind - CP interrupt, front-buffer flip, wait on
//               memory or a register, guest write or instruction store -
//               with the time each kind held it
//
// Frame timeline (XENON_FRAME_TIMELINE=1, or with XENON_HOST_PROFILE):
// frame_timeline.log gets one line per presented frame - when it was
// presented, the interval since the last, and for the work between the two
// the draws, resolves, GPU busy and idle time, backend time, frontend waits
// for the backend (holds) and fence waits - so a slow frame shows whether
// the GPU, the backend thread or a wait held it.

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
  EdramGpuStores,
  EdramGpuStoreBytes,
  EdramGpuStoreNs,
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
  BackendQueueMax,       // maximum (note_max())
  BackendQueueDepthSum,  // depth summed at each post
  BackendPosts,
  BarrierInterrupts,
  BarrierInterruptNs,
  BarrierSwaps,
  BarrierSwapNs,
  BarrierWaits,
  BarrierWaitNs,
  BarrierWrites,
  BarrierWriteNs,
  // Per-draw redundancy removed (see bind_shadow.hpp):
  ConstantUploads,     // draws that wrote their own shader-constant slot
  ConstantReuses,      // draws that pointed at an earlier draw's identical slot
  BindsSet,            // per-draw command-list state sets recorded
  BindsSkipped,        // per-draw command-list state sets skipped (already held)
  TextureMarkCalls,    // GPU/CPU writes passed to the texture dirty tracker
  TextureMarkTouched,  // textures those writes newly made stale
  ImLoadHits,          // IM_LOAD reloads served from the decoded-program cache
  ImLoadMisses,        // IM_LOAD reloads that swapped, hashed and decoded
  MirrorBatches,       // guest-memory mirror copy batches submitted
  MirrorRanges,        // dirty ranges those batches carried
  ConstantFillsSkipped,  // draws whose staged constants needed no CPU fill at all
  ConstantBlocksFilled,  // 256-byte constant blocks the CPU fill rewrote
  TextureConsumeJournal,  // texture staleness checks answered from the write journal
  TextureConsumeWalks,    // ... that had to walk the texture's page epochs
  PipelineWaitNs,      // recording waited this long for an async pipeline build
  PipelineWaits,       // draws that had to wait for their pipeline
  PipelinesQueued,     // pipelines handed to the async build workers
  PipelineFailures,    // pipeline builds that failed
  Count,
};

void add(Counter counter, std::uint64_t value) noexcept;
// Raises `counter` to `value` if it is lower (a per-window maximum).
void note_max(Counter counter, std::uint64_t value) noexcept;
[[nodiscard]] std::uint64_t peek(Counter counter) noexcept;
// Everything ever added to `counter`, never reset (the frame timeline's
// per-frame deltas).
[[nodiscard]] std::uint64_t lifetime(Counter counter) noexcept;

// The frame timeline (see above): on when `enabled`; set_frame_timeline()
// from the environment at session start.
void set_frame_timeline(bool enabled) noexcept;
[[nodiscard]] bool frame_timeline_enabled() noexcept;
// The timeline line for a frame presented at `when` (exposed for tests);
// note_presented_frame() writes it to frame_timeline.log when enabled.
[[nodiscard]] std::string take_frame_timeline_line(std::chrono::steady_clock::time_point when);

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
// The frame timeline line of the most recently presented frame (kept whether
// or not frame_timeline.log is being written); empty before the first.
[[nodiscard]] std::string last_frame_timeline_line();

// Formats the line for the window since the last flush and resets it;
// exposed for tests. `seconds` is the window's length.
[[nodiscard]] std::string take_line(double seconds);

// Writes take_line() to perf_frame.log once a second has passed since the
// last write (called by the GPU worker each tick).
void flush_if_due(std::chrono::steady_clock::time_point now);

}  // namespace xenon::gpu::perf

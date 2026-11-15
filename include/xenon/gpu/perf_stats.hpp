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
//   frames      presented frames in the second; ms p50/p99/max between them
//   swaps       guest VdSwap calls (the frames the title finished)
//   ring/ir     time the worker spent decoding the ring / playing IR
//   present     time in the host present (DXGI/Vulkan, including its wait)
//   fence       time the CPU blocked on the host GPU, and how many waits
//   shader      shader compiles on the draw path: count and time
//   pso         pipeline creations: count and time
//   draws/resolves  per second
//   readback    MB of GPU-written bytes copied into guest memory; the time the
//               GPU worker waited for them (and how often); the time guest
//               CPU writes were held off while they were written ("window")

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

// A frame reached the screen (called by the presenting thread only).
void note_presented_frame(std::chrono::steady_clock::time_point when);

// Formats the line for the window since the last flush and resets it;
// exposed for tests. `seconds` is the window's length.
[[nodiscard]] std::string take_line(double seconds);

// Writes take_line() to perf_frame.log once a second has passed since the
// last write (called by the GPU worker each tick).
void flush_if_due(std::chrono::steady_clock::time_point now);

}  // namespace xenon::gpu::perf

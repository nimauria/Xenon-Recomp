#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "xenon/memory/address_space.hpp"
#include "xenon/memory/gpu_coherency.hpp"

namespace xenon::gpu {

// Writes GPU-authored bytes (resolves, memory exports) that a discrete mirror
// copied into a host readback buffer into Xbox physical RAM, on a thread of
// its own, as soon as the GPU has finished each copy.
//
// The guest-visible contract is unchanged: before the guest can observe the
// GPU getting past a point (a fence, an interrupt, a memory poll), the command
// processor still waits until every readback queued before it is in guest
// memory (wait_published()). What moves off the GPU thread is the copy itself
// - AC6's cutscenes resolve ~20 MiB a frame, and copying it on the thread that
// also parses the ring and records every draw cost ~15% of that thread.
//
// Only bytes still owned by the readback's exact GPU generation are written
// (GuestMemoryGpuCoherency::prepare_gpu_download()), inside AddressSpace's
// short physical-write window, exactly as the GPU thread did before.
class ReadbackPublisher {
 public:
  // Returns true once the GPU has completed `value`; may return false after
  // roughly `timeout_ms` when it has not (the value may not be submitted yet).
  // Must be callable from the publisher's thread.
  using CompletionWait = std::function<bool(std::uint64_t value, std::uint32_t timeout_ms)>;

  ReadbackPublisher() = default;
  ~ReadbackPublisher() { stop(); }
  ReadbackPublisher(const ReadbackPublisher&) = delete;
  ReadbackPublisher& operator=(const ReadbackPublisher&) = delete;

  // `readback` is the persistently mapped host buffer the GPU copies into;
  // `max_range_bytes` bounds each guest write (the mirror's transfer size).
  void start(memory::AddressSpace& memory, memory::GuestMemoryGpuCoherency& coherency,
             std::span<const std::byte> readback, std::uint32_t max_range_bytes,
             CompletionWait completion_wait);
  // Joins the thread. Readbacks still queued are dropped: the mirror they
  // belong to is being torn down.
  void stop() noexcept;
  [[nodiscard]] bool running() const noexcept { return thread_.joinable(); }

  // Queues the readback of `range`, at `offset` in the readback buffer, which
  // the GPU has finished once it completes `value`. Values are nondecreasing.
  void push(const memory::GpuReadbackRange& range, std::uint64_t offset, std::uint64_t value);

  // Every queued readback whose GPU value is <= the result is in guest memory
  // (so its readback-buffer region may be reused); max() when none is queued.
  [[nodiscard]] std::uint64_t published_through() const;
  // Blocks until every readback queued with a value <= `value` is in guest
  // memory, or the publisher failed. The caller has submitted that work.
  [[nodiscard]] bool wait_published(std::uint64_t value);
  // Blocks until every queued readback is in guest memory.
  [[nodiscard]] bool wait_all();
  [[nodiscard]] bool pending() const;
  [[nodiscard]] std::uint64_t last_value() const;

  // The readbacks written into guest memory since the last call.
  [[nodiscard]] std::vector<memory::GpuPublishedWrite> take_published_writes();
  // Non-empty once a write failed; the publisher stops publishing then.
  [[nodiscard]] std::string error() const;

 private:
  struct Entry {
    memory::GpuReadbackRange range{};
    std::uint64_t offset{};
    std::uint64_t value{};
    std::uint64_t sequence{};
  };

  void run();
  [[nodiscard]] bool wait_sequence(std::unique_lock<std::mutex>& lock, std::uint64_t sequence);

  memory::AddressSpace* memory_{};
  memory::GuestMemoryGpuCoherency* coherency_{};
  std::span<const std::byte> readback_{};
  std::uint32_t max_range_bytes_{};
  CompletionWait completion_wait_{};

  mutable std::mutex mutex_{};
  std::condition_variable work_{};
  std::condition_variable done_{};
  std::deque<Entry> queue_{};
  // Entries taken by the thread and not yet written; their smallest value.
  std::uint64_t in_flight_min_value_{(std::numeric_limits<std::uint64_t>::max)()};
  std::uint64_t pushed_sequence_{};
  std::uint64_t taken_sequence_{};
  std::uint64_t done_sequence_{};
  std::uint64_t last_value_{};
  std::vector<memory::GpuPublishedWrite> published_{};
  std::string error_{};
  bool running_{};
  std::thread thread_{};
};

}  // namespace xenon::gpu

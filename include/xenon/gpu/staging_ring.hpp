#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>

namespace xenon::gpu {

// Allocation bookkeeping for a persistently mapped upload buffer that the GPU
// copies from asynchronously. Each reserved region stays in flight until the
// queue's completed value reaches the retirement value recorded for it; a new
// reservation that would overwrite an in-flight region waits for it first.
// The CPU therefore only stalls when the ring wraps onto data the GPU has not
// consumed, rather than draining the queue for every upload.
//
// API-agnostic: the backend passes its queue's completed value and a wait
// function that submits any open batch and blocks until a value completes.
class StagingRing {
 public:
  static constexpr std::uint64_t kAlignment = 256;

  void reset(std::uint64_t capacity) noexcept {
    capacity_ = capacity;
    head_ = 0;
    in_flight_.clear();
  }
  [[nodiscard]] std::uint64_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::size_t in_flight() const noexcept { return in_flight_.size(); }

  // Returns the offset of `size` free bytes, or nullopt when `size` is zero,
  // exceeds the capacity, or `wait` fails. `wait(value)` must return once the
  // queue has completed `value`.
  template <typename Wait>
  [[nodiscard]] std::optional<std::uint64_t> reserve(std::uint64_t size,
                                                     std::uint64_t completed,
                                                     Wait&& wait) {
    if (!size || size > capacity_) return std::nullopt;
    retire(completed);
    std::uint64_t offset = head_;
    if (offset + size > capacity_) offset = 0;
    const auto end = offset + size;
    std::uint64_t blocking = 0;
    for (const auto& region : in_flight_)
      if (region.begin < end && offset < region.end)
        blocking = (std::max)(blocking, region.value);
    if (blocking) {
      if (!wait(blocking)) return std::nullopt;
      retire(blocking);
    }
    head_ = (std::min)(capacity_, (end + kAlignment - 1) / kAlignment * kAlignment);
    return offset;
  }

  // Records the queue value that retires a region returned by reserve().
  void commit(std::uint64_t offset, std::uint64_t size, std::uint64_t value) {
    in_flight_.push_back({offset, offset + size, value});
  }

 private:
  struct Region {
    std::uint64_t begin{};
    std::uint64_t end{};
    std::uint64_t value{};
  };

  void retire(std::uint64_t completed) {
    std::erase_if(in_flight_, [&](const Region& region) { return region.value <= completed; });
  }

  std::uint64_t capacity_{};
  std::uint64_t head_{};
  std::deque<Region> in_flight_{};
};

}  // namespace xenon::gpu

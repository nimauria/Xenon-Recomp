#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace xenon::gpu {

// Bump allocator for one command-batch slot's transient upload buffer (draw
// index data). Allocations belong to the batch recorded into the slot, named
// by the fence value that will retire it; the first allocation of a new batch
// restarts the arena. Restarting on the batch's first *draw* was wrong once
// resolves, texture loads and mirror uploads recorded into the same batches:
// a batch they opened never restarted it, so its offset grew across batches
// until every indexed draw failed.
class TransientArena {
 public:
  static constexpr std::size_t kAlignment = 4u;

  explicit TransientArena(std::size_t capacity = 0) noexcept : capacity_(capacity) {}

  void reset_capacity(std::size_t capacity) noexcept {
    capacity_ = capacity;
    batch_ = 0;
    offset_ = 0;
  }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  // Whether `size` bytes still fit in `batch`'s share of the arena.
  [[nodiscard]] bool fits(std::uint64_t batch, std::size_t size) const noexcept {
    const auto start = aligned(batch == batch_ ? offset_ : 0u);
    return start <= capacity_ && size <= capacity_ - start;
  }

  // Reserves `size` bytes for `batch` and returns their aligned offset, or
  // nothing when the batch's share is full.
  [[nodiscard]] std::optional<std::size_t> allocate(std::uint64_t batch,
                                                    std::size_t size) noexcept {
    if (!fits(batch, size)) return std::nullopt;
    const auto start = aligned(batch == batch_ ? offset_ : 0u);
    batch_ = batch;
    offset_ = start + size;
    return start;
  }

 private:
  [[nodiscard]] static std::size_t aligned(std::size_t offset) noexcept {
    return (offset + kAlignment - 1u) & ~(kAlignment - 1u);
  }

  std::size_t capacity_{};
  std::uint64_t batch_{};
  std::size_t offset_{};
};

}  // namespace xenon::gpu

#pragma once

// Backend-neutral redundancy filters for per-draw work, shared by the D3D12
// and Vulkan backends:
//
//  * BindShadow<T>: what a command list (or rendering instance) currently
//    holds for one piece of bound state. update() says whether a set is needed
//    and remembers it; invalidate() forgets (a root-signature change, a
//    descriptor-heap change, a new command list, a new rendering instance).
//    Values are compared bytewise (trivially copyable, padding-free API
//    structs), so a NaN-valued blend factor equal to the last one is still
//    "unchanged".
//  * BindCounts: set/skipped tallies a recorder keeps locally and flushes to
//    the process-wide perf counters once per run (recording may happen on
//    worker threads, so nothing per draw touches an atomic).
//  * ConstantsSlotCache: decides whether a draw's staged shader constants
//    equal those already written to an earlier slot of the same batch, so the
//    draw can point at that slot instead of copying another 9.7 KB.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <type_traits>

#include "xenon/gpu/perf_stats.hpp"

namespace xenon::gpu {

struct BindCounts {
  std::uint64_t set{};
  std::uint64_t skipped{};
  void flush() noexcept {
    if (set) perf::add(perf::Counter::BindsSet, set);
    if (skipped) perf::add(perf::Counter::BindsSkipped, skipped);
    set = skipped = 0;
  }
};

template <class T>
class BindShadow {
  static_assert(std::is_trivially_copyable_v<T>);

 public:
  // True when `value` differs from what is bound (or nothing is known) and
  // the caller must record the set; the shadow then holds `value`.
  [[nodiscard]] bool update(const T& value, BindCounts& counts) noexcept {
    if (valid_ && std::memcmp(&value_, &value, sizeof(T)) == 0) {
      ++counts.skipped;
      return false;
    }
    std::memcpy(&value_, &value, sizeof(T));
    valid_ = true;
    ++counts.set;
    return true;
  }
  void invalidate() noexcept { valid_ = false; }
  [[nodiscard]] bool known() const noexcept { return valid_; }

 private:
  T value_{};
  bool valid_{};
};

class ConstantsSlotCache {
 public:
  static constexpr std::size_t kMaxBytes = 16384;

  // The slot an earlier draw of this batch wrote identical constants to, when
  // there is one. `slot` is the slot the current draw was assigned: only a
  // lower slot of the same frame and batch counts (slots increase within a
  // batch, so nothing later in it overwrites the earlier one while the GPU may
  // still read it, and a frame's next batch starts again at slot 0).
  [[nodiscard]] std::optional<std::uint32_t> reusable(std::uint32_t frame, std::uint64_t batch,
                                                      std::uint32_t slot,
                                                      std::span<const std::byte> staged) const noexcept {
    if (!valid_ || frame != frame_ || batch != batch_ || slot_ >= slot ||
        staged.size() != size_ || std::memcmp(copy_.data(), staged.data(), size_) != 0)
      return std::nullopt;
    return slot_;
  }
  // Records that `staged` was written to `slot` (a new upload).
  void uploaded(std::uint32_t frame, std::uint64_t batch, std::uint32_t slot,
                std::span<const std::byte> staged) noexcept {
    size_ = (std::min)(staged.size(), copy_.size());
    std::memcpy(copy_.data(), staged.data(), size_);
    frame_ = frame;
    batch_ = batch;
    slot_ = slot;
    valid_ = size_ == staged.size();
  }
  void reset() noexcept { valid_ = false; }

  // Convenience for the layouts: reuse or upload via `write(slot)`.
  // Returns the slot holding the constants and counts the outcome.
  template <class Write>
  std::uint32_t resolve(std::uint32_t frame, std::uint64_t batch, std::uint32_t slot,
                        std::span<const std::byte> staged, Write&& write) {
    if (const auto earlier = reusable(frame, batch, slot, staged)) {
      ++reuses_;
      perf::add(perf::Counter::ConstantReuses, 1u);
      return *earlier;
    }
    write(slot);
    uploaded(frame, batch, slot, staged);
    ++uploads_;
    perf::add(perf::Counter::ConstantUploads, 1u);
    return slot;
  }
  [[nodiscard]] std::uint64_t uploads() const noexcept { return uploads_; }
  [[nodiscard]] std::uint64_t reuses() const noexcept { return reuses_; }

 private:
  std::array<std::byte, kMaxBytes> copy_{};
  std::size_t size_{};
  std::uint32_t frame_{UINT32_MAX};
  std::uint64_t batch_{UINT64_MAX};
  std::uint32_t slot_{};
  bool valid_{};
  std::uint64_t uploads_{};
  std::uint64_t reuses_{};
};

}  // namespace xenon::gpu

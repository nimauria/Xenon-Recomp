#include "xenon/gpu/guest_read_sync.hpp"

namespace xenon::gpu {

std::uint64_t GuestReadSync::sync(memory::AddressSpace& memory, const Classify& classify) {
  auto& coherency = memory.coherency();
  epoch_ = coherency.current_epoch();
  active_ = true;
  ++sync_points_;
  // Every sync point either re-captures a tracked texture the CPU changed or
  // confirms it unchanged, so a capture that survives holds what its memory
  // held at the latest sync point.
  for (auto it = sampled_.begin(); it != sampled_.end();) {
    auto& sampled = it->second;
    const auto tracking = sync_points_ - sampled.last_sync_point > kRecentSyncPoints
                              ? Tracking::Forget
                              : classify(it->first, sampled.base, sampled.size);
    if (tracking == Tracking::Forget) {
      captures_.erase(it->first);
      it = sampled_.erase(it);
      continue;
    }
    sampled.gpu_authored = tracking == Tracking::GpuAuthored;
    if (sampled.gpu_authored) {
      captures_.erase(it->first);
      ++it;
      continue;
    }
    if (coherency.range_changed_since(sampled.base, sampled.size, sampled.accounted_epoch,
                                      epoch_)) {
      auto& capture = captures_[it->first];
      capture.bytes.resize(sampled.size);
      capture.base = sampled.base;
      if (!memory.copy_physical_range(sampled.base, capture.bytes)) {
        // Uncaptured, it must not pass for unchanged: the next draw reads it.
        captures_.erase(it->first);
        it = sampled_.erase(it);
        continue;
      }
      sampled.accounted_epoch = epoch_;
    }
    ++it;
  }
  return epoch_;
}

std::uint64_t GuestReadSync::read_epoch(const memory::AddressSpace& memory) const {
  return active_ ? epoch_ : memory.coherency().current_epoch();
}

void GuestReadSync::refreshed(std::uint64_t key, std::uint32_t base, std::uint32_t size,
                              std::uint64_t epoch) {
  sampled_[key] = Sampled{base, size, sync_points_, epoch, false};
}

void GuestReadSync::reused(std::uint64_t key) noexcept {
  if (const auto it = sampled_.find(key); it != sampled_.end())
    it->second.last_sync_point = sync_points_;
}

std::optional<std::vector<std::byte>> GuestReadSync::take_capture(std::uint64_t key,
                                                                  std::uint32_t base,
                                                                  std::uint32_t size) {
  const auto it = captures_.find(key);
  if (it == captures_.end()) return std::nullopt;
  std::optional<std::vector<std::byte>> bytes;
  if (active_ && it->second.base == base && it->second.bytes.size() == size)
    bytes = std::move(it->second.bytes);
  captures_.erase(it);
  return bytes;
}

bool GuestReadSync::unchanged_at_sync(std::uint64_t key) const {
  if (!active_ || captures_.contains(key)) return false;
  const auto it = sampled_.find(key);
  return it != sampled_.end() && !it->second.gpu_authored;
}

void GuestReadSync::backend_wrote(std::uint32_t base, std::uint32_t size) {
  if (!size) return;
  const auto end = std::uint64_t{base} + size;
  for (auto it = sampled_.begin(); it != sampled_.end();) {
    const auto& sampled = it->second;
    if (sampled.base < end && base < std::uint64_t{sampled.base} + sampled.size) {
      captures_.erase(it->first);
      it = sampled_.erase(it);
    } else {
      ++it;
    }
  }
}

void GuestReadSync::clear() noexcept {
  sampled_.clear();
  captures_.clear();
  active_ = false;
}

}  // namespace xenon::gpu

#include "xenon/gpu/guest_read_sync.hpp"

#include <algorithm>

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
                                      epoch_) &&
        changed_beyond_publications(coherency, sampled.base, sampled.size,
                                    sampled.accounted_epoch)) {
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

bool GuestReadSync::changed_beyond_publications(const memory::GuestMemoryCoherency& coherency,
                                               std::uint32_t base, std::uint32_t size,
                                               std::uint64_t since) const {
  if (published_.empty() || !size) return true;
  const auto end = (std::min)(std::uint64_t{base} + size, std::uint64_t{memory::kPhysicalMemorySize});
  for (auto page = base / memory::kBasePageSize;
       std::uint64_t{page} * memory::kBasePageSize < end; ++page) {
    const auto epoch = coherency.page_epoch(page);
    if (epoch <= since) continue;
    // The page's newest write must be a publication covering it that found
    // the page no newer than `since`: the copy rewrote bytes the texture
    // already has, and nothing else wrote the page since.
    const auto page_begin = std::uint64_t{page} * memory::kBasePageSize;
    const bool only_published = std::any_of(
        published_.rbegin(), published_.rend(), [&](const memory::GpuPublishedWrite& write) {
          return write.epoch == epoch && write.previous_epoch <= since &&
                 write.address < page_begin + memory::kBasePageSize &&
                 page_begin < std::uint64_t{write.address} + write.size;
        });
    if (!only_published) return true;
  }
  return false;
}

void GuestReadSync::gpu_published(std::span<const memory::GpuPublishedWrite> writes) {
  for (const auto& write : writes) published_.push_back(write);
  while (published_.size() > kPublicationsKept) published_.pop_front();
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
  published_.clear();
  active_ = false;
}

bool cached_texture_stale(TextureDirtyTracker& dirty, GuestReadSync& reads, std::uint64_t key,
                          const memory::GuestMemoryCoherency& coherency,
                          std::uint64_t read_epoch) {
  if (!dirty.consume_dirty(key, coherency, read_epoch)) return false;
  // A GPU write untracks the texture (backend_wrote), so this vetoes only a
  // CPU write the latest sync point did not see.
  return !reads.unchanged_at_sync(key);
}

void note_backend_write(TextureDirtyTracker& dirty, GuestReadSync& reads, std::uint32_t base,
                        std::uint32_t size) {
  if (!size) return;
  dirty.mark_dirty(base, size);
  reads.backend_wrote(base, size);
}

}  // namespace xenon::gpu

#include "xenon/gpu/index_cache.hpp"

#include <utility>
#include <vector>

namespace xenon::gpu {

std::size_t IndexCacheKeyHash::operator()(const IndexCacheKey& key) const noexcept {
  std::uint64_t value = key.address;
  value = value * 0x9E3779B97F4A7C15ull ^ key.count;
  value = value * 0x9E3779B97F4A7C15ull ^
          (std::uint64_t(key.format) | std::uint64_t(key.endian) << 8u |
           std::uint64_t(key.primitive) << 16u | std::uint64_t(key.reset_enabled) << 24u);
  value = value * 0x9E3779B97F4A7C15ull ^ key.reset_index;
  return static_cast<std::size_t>(value ^ (value >> 29u));
}

ProcessedIndexCache::ProcessedIndexCache(std::size_t capacity_bytes)
    : capacity_bytes_(capacity_bytes) {}

std::shared_ptr<const ProcessedPrimitiveBatch> ProcessedIndexCache::find(
    const IndexCacheKey& key, std::uint32_t source_bytes,
    const memory::GuestMemoryCoherency& coherency, std::uint64_t now_epoch) {
  const auto found = entries_.find(key);
  if (found == entries_.end()) {
    ++stats_.misses;
    return {};
  }
  auto entry = found->second;
  if (entry->source_bytes != source_bytes ||
      coherency.range_changed_since(key.address, source_bytes, entry->epoch)) {
    ++stats_.stale;
    ++stats_.misses;
    erase(entry);
    return {};
  }
  // Unchanged through now: later writes get newer epochs.
  if (now_epoch > entry->epoch) entry->epoch = now_epoch;
  lru_.splice(lru_.begin(), lru_, entry);
  ++stats_.hits;
  stats_.reused_index_bytes += entry->bytes;
  return entry->batch;
}

void ProcessedIndexCache::insert(const IndexCacheKey& key, std::uint32_t source_bytes,
                                 std::uint64_t read_epoch,
                                 std::shared_ptr<const ProcessedPrimitiveBatch> batch) {
  if (!batch || !batch->valid) return;
  const auto bytes = batch->indices.size() * sizeof(std::uint32_t) + sizeof(Entry);
  stats_.converted_index_bytes += batch->indices.size() * sizeof(std::uint32_t);
  if (const auto found = entries_.find(key); found != entries_.end()) erase(found->second);
  if (bytes > capacity_bytes_) return;
  while (resident_bytes_ + bytes > capacity_bytes_ && !lru_.empty()) {
    ++stats_.evictions;
    erase(std::prev(lru_.end()));
  }
  lru_.push_front(Entry{key, source_bytes, read_epoch, bytes, std::move(batch)});
  entries_.emplace(key, lru_.begin());
  resident_bytes_ += bytes;
}

void ProcessedIndexCache::invalidate(std::uint32_t address, std::uint32_t size) {
  if (!size || lru_.empty()) return;
  const auto begin = std::uint64_t{address};
  const auto end = begin + size;
  for (auto entry = lru_.begin(); entry != lru_.end();) {
    const auto next = std::next(entry);
    const auto first = std::uint64_t{entry->key.address};
    if (first < end && first + entry->source_bytes > begin) {
      ++stats_.gpu_invalidations;
      erase(entry);
    }
    entry = next;
  }
}

void ProcessedIndexCache::clear() {
  stats_.gpu_invalidations += lru_.size();
  entries_.clear();
  lru_.clear();
  resident_bytes_ = 0;
}

void ProcessedIndexCache::erase(Lru::iterator entry) {
  resident_bytes_ -= entry->bytes;
  entries_.erase(entry->key);
  lru_.erase(entry);
}

}  // namespace xenon::gpu

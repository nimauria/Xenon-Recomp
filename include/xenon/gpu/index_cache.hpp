#pragma once

// Processed-index cache: Xenos index buffers converted to host primitives
// once, and reused for as long as the guest bytes under them stay unchanged.
//
// A draw's DMA index buffer is big-endian, may be a strip, fan, quad or
// reset-delimited list, and is read by the GPU every frame. Converting it
// (copy out of guest memory, endian swap, topology expansion) cost every
// indexed draw a pass over its indices and two allocations, though nearly all
// of them - a mesh's indices - never change after loading. An entry is valid
// while no page of its source range has a write epoch newer than the epoch
// read before its bytes were copied (GuestMemoryCoherency page epochs: CPU
// stores, DMA and native-access stores collected by current_epoch()); GPU
// writes (resolves, memory export), which page epochs do not record, reach the
// cache through invalidate(). An entry is therefore exactly the conversion a
// fresh read would produce.
//
// Bounded by the bytes of converted indices it holds; the least recently used
// entries go first. Not thread-safe: one backend's draw consumption owns it.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <unordered_map>

#include "xenon/gpu/primitive_processor.hpp"
#include "xenon/memory/coherency.hpp"

namespace xenon::gpu {

struct IndexCacheKey {
  std::uint32_t address{};
  // Indices read (the draw's count, limited by the buffer's length).
  std::uint32_t count{};
  IndexFormat format{IndexFormat::UInt16};
  Endian endian{Endian::None};
  PrimitiveType primitive{PrimitiveType::TriangleList};
  bool reset_enabled{};
  std::uint32_t reset_index{};

  friend bool operator==(const IndexCacheKey&, const IndexCacheKey&) = default;
};

struct IndexCacheKeyHash {
  std::size_t operator()(const IndexCacheKey& key) const noexcept;
};

struct IndexCacheStats {
  std::uint64_t hits{};
  std::uint64_t misses{};
  // Entries dropped because their source changed (CPU) or the GPU wrote it.
  std::uint64_t stale{};
  std::uint64_t gpu_invalidations{};
  std::uint64_t evictions{};
  std::uint64_t converted_index_bytes{};
  std::uint64_t reused_index_bytes{};
};

class ProcessedIndexCache {
 public:
  static constexpr std::size_t kDefaultCapacityBytes = std::size_t{64} << 20;
  explicit ProcessedIndexCache(std::size_t capacity_bytes = kDefaultCapacityBytes);

  // The cached conversion of `key`, when guest memory [key.address,
  // key.address + source_bytes) has not changed since it was read; null (a
  // miss, and a stale entry dropped) otherwise. `now_epoch` is a
  // GuestMemoryCoherency::current_epoch() taken by the caller for this draw:
  // a hit is known unchanged through it.
  [[nodiscard]] std::shared_ptr<const ProcessedPrimitiveBatch> find(
      const IndexCacheKey& key, std::uint32_t source_bytes,
      const memory::GuestMemoryCoherency& coherency, std::uint64_t now_epoch);

  // Records `batch` as the conversion of `key`'s source bytes as they stood
  // at `read_epoch` (taken before they were copied). A batch larger than the
  // whole capacity is not kept.
  void insert(const IndexCacheKey& key, std::uint32_t source_bytes, std::uint64_t read_epoch,
              std::shared_ptr<const ProcessedPrimitiveBatch> batch);

  // The GPU wrote [address, address + size): every entry reading it is gone.
  void invalidate(std::uint32_t address, std::uint32_t size);
  void clear();

  // Called with every batch the cache drops (stale, invalidated, evicted,
  // replaced or cleared), so state kept for it - its GPU-resident copy - can
  // be released.
  void set_erase_listener(std::function<void(const ProcessedPrimitiveBatch&)> listener) {
    erase_listener_ = std::move(listener);
  }

  [[nodiscard]] std::size_t entries() const noexcept { return entries_.size(); }
  [[nodiscard]] std::size_t resident_bytes() const noexcept { return resident_bytes_; }
  [[nodiscard]] const IndexCacheStats& stats() const noexcept { return stats_; }

 private:
  struct Entry {
    IndexCacheKey key{};
    std::uint32_t source_bytes{};
    std::uint64_t epoch{};
    std::size_t bytes{};
    std::shared_ptr<const ProcessedPrimitiveBatch> batch;
  };
  using Lru = std::list<Entry>;
  void erase(Lru::iterator entry);

  std::size_t capacity_bytes_;
  std::size_t resident_bytes_{};
  Lru lru_;  // most recently used first
  std::unordered_map<IndexCacheKey, Lru::iterator, IndexCacheKeyHash> entries_;
  IndexCacheStats stats_{};
  std::function<void(const ProcessedPrimitiveBatch&)> erase_listener_;
};

}  // namespace xenon::gpu

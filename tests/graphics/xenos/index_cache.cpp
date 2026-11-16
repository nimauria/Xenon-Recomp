// ProcessedIndexCache: a converted index buffer is reused exactly while its
// guest bytes are unchanged - a CPU write (page epochs) or a GPU write
// (invalidate) after it was read makes the next lookup a miss - and the cache
// stays within its byte budget, least recently used first.

#include "xenon/gpu/index_cache.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <memory>
#include <vector>

using namespace xenon::gpu;

namespace {

std::shared_ptr<const ProcessedPrimitiveBatch> batch_of(std::size_t indices) {
  auto batch = std::make_shared<ProcessedPrimitiveBatch>();
  batch->indices.resize(indices, 7u);
  batch->vertex_count = static_cast<std::uint32_t>(indices);
  batch->indexed = true;
  batch->valid = true;
  return batch;
}

IndexCacheKey key_at(std::uint32_t address, std::uint32_t count = 300) {
  return {address, count, IndexFormat::UInt16, Endian::Swap8In16, PrimitiveType::TriangleList,
          false, 0u};
}

}  // namespace

int main() {
  xenon::memory::GuestMemoryCoherency coherency;
  ProcessedIndexCache cache(1u << 20);
  const auto key = key_at(0x10000);
  const std::uint32_t bytes = 600;

  // Empty: a miss.
  auto epoch = coherency.current_epoch();
  assert(!cache.find(key, bytes, coherency, epoch));
  const auto converted = batch_of(300);
  cache.insert(key, bytes, epoch, converted);
  assert(cache.entries() == 1 && cache.resident_bytes() > 0);

  // Unchanged: the same conversion, again and again.
  for (int i = 0; i < 3; ++i) {
    epoch = coherency.current_epoch();
    assert(cache.find(key, bytes, coherency, epoch) == converted);
  }
  assert(cache.stats().hits == 3);

  // A write elsewhere changes nothing.
  coherency.mark_write(0x40000, 64);
  assert(cache.find(key, bytes, coherency, coherency.current_epoch()) == converted);

  // A CPU write over the source: stale, dropped, a miss.
  coherency.mark_write(0x10000 + 100, 2);
  assert(!cache.find(key, bytes, coherency, coherency.current_epoch()));
  assert(cache.entries() == 0 && cache.stats().stale == 1);

  // A write that lands after the bytes were read, but before the entry was
  // recorded, is not hidden: the entry carries the epoch taken before reading.
  const auto before_read = coherency.current_epoch();
  coherency.mark_write(0x10000, 2);  // races the copy
  cache.insert(key, bytes, before_read, converted);
  assert(!cache.find(key, bytes, coherency, coherency.current_epoch()));

  // A different length of the same buffer is not the same conversion.
  cache.insert(key, bytes, coherency.current_epoch(), converted);
  assert(!cache.find(key, bytes - 2, coherency, coherency.current_epoch()));

  // GPU writes reach the cache through invalidate(); page epochs miss them.
  cache.insert(key, bytes, coherency.current_epoch(), converted);
  cache.invalidate(0x10000 + bytes, 16);  // just past it: kept
  assert(cache.find(key, bytes, coherency, coherency.current_epoch()));
  cache.invalidate(0x10000 + bytes - 1, 16);  // its last byte
  assert(!cache.find(key, bytes, coherency, coherency.current_epoch()));
  assert(cache.stats().gpu_invalidations == 1);

  // Other key fields separate entries.
  auto strip = key;
  strip.primitive = PrimitiveType::TriangleStrip;
  cache.insert(key, bytes, coherency.current_epoch(), converted);
  assert(!cache.find(strip, bytes, coherency, coherency.current_epoch()));
  auto reset = key;
  reset.reset_enabled = true;
  reset.reset_index = 0xFFFF;
  assert(!cache.find(reset, bytes, coherency, coherency.current_epoch()));
  assert(cache.find(key, bytes, coherency, coherency.current_epoch()));
  cache.clear();
  assert(cache.entries() == 0 && cache.resident_bytes() == 0);

  // The budget: the least recently used entry is evicted first.
  ProcessedIndexCache small(3 * (64 * 1024 * 4 + 512));
  const auto big = [] { return batch_of(64 * 1024); };
  small.insert(key_at(0x100000), 131072, coherency.current_epoch(), big());
  small.insert(key_at(0x200000), 131072, coherency.current_epoch(), big());
  small.insert(key_at(0x300000), 131072, coherency.current_epoch(), big());
  assert(small.entries() == 3);
  // Touch the oldest, so the second is now least recent.
  assert(small.find(key_at(0x100000), 131072, coherency, coherency.current_epoch()));
  small.insert(key_at(0x400000), 131072, coherency.current_epoch(), big());
  assert(small.entries() == 3 && small.stats().evictions == 1);
  assert(small.find(key_at(0x100000), 131072, coherency, coherency.current_epoch()));
  assert(!small.find(key_at(0x200000), 131072, coherency, coherency.current_epoch()));
  assert(small.resident_bytes() <= 3 * (64 * 1024 * 4 + 512));
  // Bigger than the whole budget: not kept, nothing evicted for it.
  ProcessedIndexCache tiny(1024);
  tiny.insert(key, bytes, coherency.current_epoch(), batch_of(4096));
  assert(tiny.entries() == 0);

  // The erase listener hears every batch the cache drops - stale, GPU
  // invalidated, evicted, replaced and cleared - so its resident copy can go.
  {
    ProcessedIndexCache listened(3 * (1024 * 4 + 512));
    std::vector<std::uint64_t> dropped;
    listened.set_erase_listener(
        [&](const ProcessedPrimitiveBatch& batch) { dropped.push_back(batch.resident_key); });
    const auto keyed = [](std::uint64_t key) {
      auto batch = std::make_shared<ProcessedPrimitiveBatch>();
      batch->indices.resize(1024, 1u);
      batch->valid = true;
      batch->resident_key = key;
      return std::shared_ptr<const ProcessedPrimitiveBatch>(batch);
    };
    listened.insert(key_at(0x10000), 2048, coherency.current_epoch(), keyed(1));
    coherency.mark_write(0x10000, 4);
    assert(!listened.find(key_at(0x10000), 2048, coherency, coherency.current_epoch()));
    assert(dropped == std::vector<std::uint64_t>{1});
    listened.insert(key_at(0x20000), 2048, coherency.current_epoch(), keyed(2));
    listened.invalidate(0x20000, 4);
    listened.insert(key_at(0x30000), 2048, coherency.current_epoch(), keyed(3));
    listened.insert(key_at(0x30000), 2048, coherency.current_epoch(), keyed(4));  // replaces 3
    listened.insert(key_at(0x40000), 2048, coherency.current_epoch(), keyed(5));
    listened.insert(key_at(0x50000), 2048, coherency.current_epoch(), keyed(6));
    listened.insert(key_at(0x60000), 2048, coherency.current_epoch(), keyed(7));  // evicts 4
    listened.clear();
    std::sort(dropped.begin(), dropped.end());
    assert((dropped == std::vector<std::uint64_t>{1, 2, 3, 4, 5, 6, 7}));
  }

  std::cout << "xenon_index_cache_tests: ok\n";
  return 0;
}

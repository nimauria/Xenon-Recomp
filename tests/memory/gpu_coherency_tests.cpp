#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

#include "xenon/memory/address_space.hpp"
#include "xenon/memory/gpu_coherency.hpp"

using namespace xenon::memory;

namespace {

// Regression: pages that generated code (native access) dirtied are journaled
// when a consumer collects them, so the GPU's upload planning keeps its exact
// history and reconciles only those pages. Collection used to publish one
// epoch with no journal entry, sending every plan_upload to the page-level
// path: a scan of all 131072 page epochs, ~50 us per call. With generated
// code storing between nearly every two draws, AC6's 3D scenes ran at 1-3 fps.
void test_native_stores_keep_upload_planning_exact() {
  constexpr GuestAddress kBase = 0x00200000u;
  AddressSpace memory;
  assert(memory.initialize());
  memory.set_native_access(true);
  assert(memory.commit_fixed(kBase, 4u * kBasePageSize, kReadWrite));
  const auto phys = memory.get_physical_address(kBase);
  assert((phys & (kBasePageSize - 1u)) == 0u);

  GuestMemoryGpuCoherency gpu;
  gpu.reset(kPhysicalMemorySize, false);
  // The four pages start valid on the device.
  auto plan = gpu.plan_upload(memory.coherency(), phys, 4u * kBasePageSize);
  for (const auto& range : plan.ranges) gpu.commit_cpu_upload(range.address, range.size);
  assert(gpu.device_range_valid(phys, 4u * kBasePageSize));

  // Stores to pages 0 and 1 (adjacent) and to page 3.
  auto access = memory.access_context();
  access.write32_be(kBase + 0x10u, 1u);
  access.write32_be(kBase + kBasePageSize + 0x20u, 2u);
  access.write8(kBase + 3u * kBasePageSize + 0x30u, 3u);

  const auto before = gpu.synchronized_epoch();
  plan = gpu.plan_upload(memory.coherency(), phys, 4u * kBasePageSize, 0u,
                         GpuRangeUsage::VertexBuffer);
  assert(plan.exact_history);
  // One journaled write per run of adjacent pages.
  std::vector<DirtyPhysicalWrite> writes;
  assert(memory.coherency().collect_exact_dirty_writes(before, plan.through_epoch, writes));
  assert(writes.size() == 2u);
  assert(writes[0].address == phys && writes[0].size == 2u * kBasePageSize);
  assert(writes[1].address == phys + 3u * kBasePageSize && writes[1].size == kBasePageSize);
  // Exactly the stored pages are uploaded; page 2 stays valid.
  assert(plan.ranges.size() == 2u);
  assert(plan.ranges[0].address == phys && plan.ranges[0].size == 2u * kBasePageSize);
  assert(plan.ranges[1].address == phys + 3u * kBasePageSize &&
         plan.ranges[1].size == kBasePageSize);
  assert(gpu.device_range_valid(phys + 2u * kBasePageSize, kBasePageSize));
  std::cout << "  [PASS] native stores keep GPU upload planning on its exact path\n";
}

// Regression (AC6 cutscenes at 2-3 fps): a draw plans its uploads as of
// its read epoch - the latest GPU/CPU sync point - so CPU writes are folded
// in through that epoch only, once for all the draws until the next one.
// Writes after it are not planned yet; the next sync point uploads them.
void test_upload_planning_folds_in_writes_through_the_read_epoch() {
  GuestMemoryCoherency stores;
  GuestMemoryGpuCoherency gpu;
  gpu.reset(0x10000u, false);
  auto plan = gpu.plan_upload(stores, 0x1000u, 0x2000u);
  for (const auto& range : plan.ranges) gpu.commit_cpu_upload(range.address, range.size);
  assert(gpu.device_range_valid(0x1000u, 0x2000u));

  stores.mark_write(0x1100u, 8u);
  const auto sync_point = stores.current_epoch();
  stores.mark_write(0x2200u, 8u);  // after the sync point

  plan = gpu.plan_upload(stores, 0x1000u, 0x2000u, 0u, GpuRangeUsage::VertexBuffer, sync_point);
  assert(plan.through_epoch == sync_point && gpu.synchronized_epoch() == sync_point);
  assert(plan.ranges.size() == 1u && plan.ranges[0].address == 0x1100u &&
         plan.ranges[0].size == 8u);
  for (const auto& range : plan.ranges) gpu.commit_cpu_upload(range.address, range.size);
  // The same read epoch folds in nothing new.
  plan = gpu.plan_upload(stores, 0x1000u, 0x2000u, 0u, GpuRangeUsage::VertexBuffer, sync_point);
  assert(plan.ranges.empty() && gpu.synchronized_epoch() == sync_point);
  // The next sync point exposes the later write.
  const auto next = stores.current_epoch();
  plan = gpu.plan_upload(stores, 0x1000u, 0x2000u, 0u, GpuRangeUsage::VertexBuffer, next);
  assert(gpu.synchronized_epoch() == next);
  assert(plan.ranges.size() == 1u && plan.ranges[0].address == 0x2200u &&
         plan.ranges[0].size == 8u);
  std::cout << "  [PASS] upload planning folds in writes through the read epoch\n";
}

// Regression (AC6 cutscenes at ~3 fps): every indexed draw plans a readback
// of its index buffer. Where the GPU owns none of those bytes the plan is
// empty without folding in pending CPU writes - they stay pending, exact, for
// the next upload - while a request the GPU did write still reconciles CPU
// writes first and reads back only the bytes the GPU still owns.
void test_readback_planning_skips_ranges_the_gpu_never_wrote() {
  GuestMemoryCoherency stores;
  GuestMemoryGpuCoherency gpu;
  gpu.reset(0x10000u, false);
  auto upload = gpu.plan_upload(stores, 0x1000u, 0x3000u);
  for (const auto& range : upload.ranges) gpu.commit_cpu_upload(range.address, range.size);

  stores.mark_write(0x1200u, 8u);
  const auto clean = gpu.plan_readback(stores, 0x1000u, 0x1000u);
  assert(clean.ranges.empty());
  assert(clean.through_epoch == 0u);  // nothing was folded in
  assert(gpu.synchronized_epoch() == 0u);
  upload = gpu.plan_upload(stores, 0x1000u, 0x1000u);
  assert(upload.ranges.size() == 1u && upload.ranges[0].address == 0x1200u &&
         upload.ranges[0].size == 8u);
  for (const auto& range : upload.ranges) gpu.commit_cpu_upload(range.address, range.size);

  gpu.mark_gpu_write(0x3000u, 16u);
  stores.mark_write(0x3004u, 4u);
  const auto owned = gpu.plan_readback(stores, 0x3000u, 16u);
  assert(owned.through_epoch != 0u);
  assert(owned.ranges.size() == 2u);
  assert(owned.ranges[0].address == 0x3000u && owned.ranges[0].size == 4u);
  assert(owned.ranges[1].address == 0x3008u && owned.ranges[1].size == 8u);

  // A CPU write before a GPU write (a resolve) is folded in when the GPU write
  // is recorded, so it cannot come in later and cancel the GPU's ownership.
  stores.mark_write(0x5000u, 16u);
  (void)gpu.plan_readback(stores, 0x5000u, 16u);  // not GPU-owned: folds in nothing
  gpu.mark_gpu_write(stores, 0x5000u, 16u);
  const auto resolved = gpu.plan_readback(stores, 0x5000u, 16u);
  assert(resolved.ranges.size() == 1u && resolved.ranges[0].address == 0x5000u &&
         resolved.ranges[0].size == 16u);
  std::cout << "  [PASS] readback planning skips ranges the GPU never wrote\n";
}

}  // namespace

int main() {
  test_native_stores_keep_upload_planning_exact();
  test_upload_planning_folds_in_writes_through_the_read_epoch();
  test_readback_planning_skips_ranges_the_gpu_never_wrote();
  GuestMemoryCoherency publications;
  GuestMemoryGpuCoherency tracker;
  tracker.reset(0x10000u, false);

  // Phase 15 lazy initialization: an untouched but device-invalid requested
  // range is initialized exactly, without making the full physical aperture
  // dirty. Requests and touched physical pages are tracked/coalesced.
  GuestMemoryGpuCoherency lazy;
  lazy.reset(0x10000u, false);
  auto lazy_plan = lazy.plan_upload(
      publications, 0x1800u, 0x20u, 0u, GpuRangeUsage::VertexBuffer);
  assert(lazy_plan.usage == GpuRangeUsage::VertexBuffer);
  assert(lazy_plan.ranges.size() == 1u);
  assert(lazy_plan.ranges[0].address == 0x1800u &&
         lazy_plan.ranges[0].size == 0x20u);
  auto requested = lazy.requested_ranges();
  assert(requested.size() == 1u && requested[0].address == 0x1800u &&
         requested[0].size == 0x20u);
  auto touched = lazy.touched_page_ranges();
  assert(touched.size() == 1u && touched[0].address == 0x1000u &&
         touched[0].size == kBasePageSize);
  lazy.commit_cpu_upload(0x1800u, 0x20u);
  assert(lazy.device_range_valid(0x1800u, 0x20u));
  lazy_plan = lazy.plan_upload(
      publications, 0x1800u, 0x20u, 0u, GpuRangeUsage::VertexBuffer);
  assert(lazy_plan.ranges.empty());

  // Adjacent byte-precise CPU writes coalesce into one requested upload range.
  publications.mark_write(0x1808u, 4u);
  publications.mark_write(0x180Cu, 4u);
  lazy_plan = lazy.plan_upload(
      publications, 0x1800u, 0x20u, 0u, GpuRangeUsage::VertexBuffer);
  assert(lazy_plan.ranges.size() == 1u);
  assert(lazy_plan.ranges[0].address == 0x1808u &&
         lazy_plan.ranges[0].size == 8u);

  // A frame of guest vertex data arrives as thousands of small stores in any
  // order. Ingesting them coalesced must give exactly the ranges ingesting
  // them one by one gives: two separate runs here, with a gap between.
  {
    GuestMemoryCoherency stores;
    GuestMemoryGpuCoherency batched;
    batched.reset(0x10000u, false);
    for (std::uint32_t i = 0; i < 2048u; ++i) {
      const std::uint32_t slot = (i * 7919u) % 2048u;  // scrambled order
      stores.mark_write(0x4000u + slot * 4u, 4u);
    }
    stores.mark_write(0x4000u, 8u);           // overlaps the first run
    stores.mark_write(0xA000u, 16u);          // separate run
    stores.mark_write(0xA00Cu, 8u);           // overlaps its end
    const auto plan = batched.plan_upload(stores, 0x4000u, 0x7000u, 0u,
                                          GpuRangeUsage::VertexBuffer);
    const auto dirty = batched.cpu_dirty_ranges();
    assert(dirty.size() == 2u);
    assert(dirty[0].address == 0x4000u && dirty[0].size == 2048u * 4u);
    assert(dirty[1].address == 0xA000u && dirty[1].size == 0x14u);
    // Nothing is device-valid yet, so the whole request uploads once.
    assert(plan.ranges.size() == 1u && plan.ranges[0].address == 0x4000u &&
           plan.ranges[0].size == 0x7000u);
    assert(!batched.device_range_valid(0x4000u, 4u));

    // Mixed ordering domains are applied in publication order: the later
    // cache-inhibited store over the same bytes decides their domain.
    GuestMemoryCoherency mixed_stores;
    GuestMemoryGpuCoherency mixed;
    mixed.reset(0x10000u, false);
    mixed_stores.mark_write(0x5000u, 8u);
    mixed_stores.mark_write(0x5000u, 8u,
                            xenon::cpu::MemoryOrderingDomain::CacheInhibited);
    const auto mixed_plan = mixed.plan_upload(mixed_stores, 0x5000u, 8u, 0u,
                                              GpuRangeUsage::VertexBuffer);
    assert(mixed_plan.source_domain ==
           xenon::cpu::MemoryOrderingDomain::CacheInhibited);
    assert(mixed.cpu_dirty_ranges().size() == 1u);
  }

  // The one-domain batch path must leave every set exactly as applying the
  // writes one at a time does, over sets already fragmented by uploads and
  // by GPU writes of different generations. A single write of another
  // domain forces the per-write path for the reference.
  {
    const auto prepare = [](GuestMemoryGpuCoherency& tracker,
                            GuestMemoryCoherency& stores) {
      tracker.reset(0x40000u, false);
      for (std::uint32_t i = 0; i < 64u; ++i) {
        const auto address = 0x1000u + i * 0x700u;
        stores.mark_write(address, 0x80u + (i % 5u) * 0x40u);
        (void)tracker.plan_upload(stores, address, 0x300u, 0u, GpuRangeUsage::VertexBuffer);
        tracker.commit_cpu_upload(address, 0x300u);
        if (i % 3u == 0u) tracker.mark_gpu_write(address + 0x100u, 0x180u);
      }
    };
    GuestMemoryCoherency batch_stores;
    GuestMemoryCoherency reference_stores;
    GuestMemoryGpuCoherency batched;
    GuestMemoryGpuCoherency reference;
    prepare(batched, batch_stores);
    prepare(reference, reference_stores);
    std::uint32_t state = 12345u;
    for (std::uint32_t i = 0; i < 3000u; ++i) {
      state = state * 1103515245u + 12345u;
      const auto address = 0x800u + (state >> 8) % 0x30000u;
      const auto size = 1u + (state >> 4) % 0x90u;
      batch_stores.mark_write(address, size);
      reference_stores.mark_write(address, size);
    }
    reference_stores.mark_write(0x3F000u, 4u,
                                xenon::cpu::MemoryOrderingDomain::CacheInhibited);
    (void)batched.plan_upload(batch_stores, 0x800u, 4u, 0u, GpuRangeUsage::VertexBuffer);
    (void)reference.plan_upload(reference_stores, 0x800u, 4u, 0u,
                                GpuRangeUsage::VertexBuffer);
    const auto below = [](std::vector<GpuCoherencyRange> ranges) {
      std::erase_if(ranges, [](const auto& range) { return range.address >= 0x3F000u; });
      return ranges;
    };
    const auto same = [](const std::vector<GpuCoherencyRange>& a,
                         const std::vector<GpuCoherencyRange>& b) {
      return a.size() == b.size() &&
             std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) {
               return x.address == y.address && x.size == y.size;
             });
    };
    assert(same(below(batched.cpu_dirty_ranges()), below(reference.cpu_dirty_ranges())));
    assert(same(below(batched.gpu_dirty_ranges()), below(reference.gpu_dirty_ranges())));
    for (std::uint32_t address = 0x800u; address < 0x3F000u; address += 0x10u)
      assert(batched.device_range_valid(address, 0x10u) ==
             reference.device_range_valid(address, 0x10u));
  }

  // Mixed ordering domains (AC6 interleaves write-combined vertex stores with
  // cached ones) are batched too. Every set and every byte's visibility
  // domain must match applying the same stores one at a time in publication
  // order, where a later store decides the domain of the bytes it covers.
  {
    using xenon::cpu::MemoryOrderingDomain;
    GuestMemoryCoherency batch_stores;
    GuestMemoryCoherency reference_stores;
    GuestMemoryGpuCoherency batched;
    GuestMemoryGpuCoherency reference;
    for (auto* tracker : {&batched, &reference}) {
      tracker->reset(0x20000u, false);
      for (std::uint32_t i = 0; i < 32u; ++i) {
        tracker->mark_gpu_write(0x400u + i * 0x900u, 0x200u);
      }
    }
    std::uint32_t state = 777u;
    for (std::uint32_t i = 0; i < 4000u; ++i) {
      state = state * 1103515245u + 12345u;
      const auto address = 0x100u + (state >> 8) % 0x1F000u;
      const auto size = 1u + (state >> 3) % 0x60u;
      const auto domain = static_cast<MemoryOrderingDomain>((state >> 20) % 4u);
      batch_stores.mark_write(address, size, domain);
      reference.mark_cpu_write(address, size, domain);
    }
    (void)batched.plan_upload(batch_stores, 0x100u, 4u, 0u, GpuRangeUsage::VertexBuffer);
    (void)reference.plan_upload(reference_stores, 0x100u, 4u, 0u, GpuRangeUsage::VertexBuffer);
    const auto same = [](const std::vector<GpuCoherencyRange>& a,
                         const std::vector<GpuCoherencyRange>& b) {
      return a.size() == b.size() &&
             std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) {
               return x.address == y.address && x.size == y.size;
             });
    };
    assert(same(batched.cpu_dirty_ranges(), reference.cpu_dirty_ranges()));
    assert(same(batched.gpu_dirty_ranges(), reference.gpu_dirty_ranges()));
    for (std::uint32_t address = 0x100u; address < 0x20000u; address += 4u) {
      assert(batched.device_range_valid(address, 4u) ==
             reference.device_range_valid(address, 4u));
      const auto batch_plan =
          batched.plan_upload(batch_stores, address, 4u, 0u, GpuRangeUsage::VertexBuffer);
      const auto reference_plan =
          reference.plan_upload(reference_stores, address, 4u, 0u, GpuRangeUsage::VertexBuffer);
      assert(batch_plan.source_domain == reference_plan.source_domain);
    }
  }

  // More stores than the journal holds fall back to dirty pages, applied in
  // one pass: every page written becomes CPU-dirty, nothing else does.
  {
    GuestMemoryCoherency stores;
    GuestMemoryGpuCoherency overflowed;
    overflowed.reset(0x200000u, false);
    (void)overflowed.plan_upload(stores, 0u, 0x200000u, 0u, GpuRangeUsage::Generic);
    overflowed.commit_cpu_upload(0u, 0x200000u);
    for (std::uint32_t i = 0; i < GuestMemoryCoherency::kWriteJournalCapacity + 4096u; ++i)
      stores.mark_write(0x10000u + (i % 64u) * 2u * kBasePageSize + (i % 997u), 4u);
    (void)overflowed.plan_upload(stores, 0u, 4u, 0u, GpuRangeUsage::Generic);
    const auto dirty = overflowed.cpu_dirty_ranges();
    assert(dirty.size() == 64u);
    for (std::uint32_t page = 0; page < 64u; ++page) {
      assert(dirty[page].address == 0x10000u + page * 2u * kBasePageSize);
      assert(dirty[page].size == kBasePageSize);
      assert(!overflowed.device_range_valid(dirty[page].address, 4u));
      assert(overflowed.device_range_valid(dirty[page].address + kBasePageSize, 4u));
    }
  }

  // Adjacent dirty physical pages are also coalesced by the canonical memory
  // tracker when exact journal history is unavailable or page ranges are used.
  GuestMemoryCoherency page_dirty;
  page_dirty.mark_write(0x2000u, 4u);
  page_dirty.mark_write(0x3000u, 4u);
  std::vector<DirtyPhysicalRange> dirty_pages;
  page_dirty.collect_dirty_ranges(0u, page_dirty.current_epoch(), 0u,
                                  dirty_pages);
  assert(dirty_pages.size() == 1u);
  assert(dirty_pages[0].address == 0x2000u &&
         dirty_pages[0].size == 2u * kBasePageSize);

  // Every current GPU consumer class can make an exact range request. The
  // unrestricted class is the explicit full-range fallback for workloads with
  // addresses that cannot be bounded ahead of execution.
  const std::array usages{
      GpuRangeUsage::Texture,      GpuRangeUsage::VertexBuffer,
      GpuRangeUsage::IndexBuffer,  GpuRangeUsage::Shader,
      GpuRangeUsage::CommandData,  GpuRangeUsage::MemoryExport,
      GpuRangeUsage::RenderReadback};
  GuestMemoryGpuCoherency classified;
  classified.reset(0x10000u, false);
  std::uint32_t classified_address = 0x4000u;
  for (const auto usage : usages) {
    const auto classified_plan = classified.plan_upload(
        publications, classified_address, 4u, 0u, usage);
    assert(classified_plan.usage == usage);
    assert(classified_plan.ranges.size() == 1u);
    assert(classified_plan.ranges[0].address == classified_address);
    classified.commit_cpu_upload(classified_address, 4u);
    classified_address += 0x100u;
  }
  GuestMemoryGpuCoherency unrestricted;
  unrestricted.reset(0x10000u, false);
  const auto full_plan = unrestricted.plan_upload(
      publications, 0u, 0x10000u, 0u, GpuRangeUsage::Unrestricted);
  assert(full_plan.usage == GpuRangeUsage::Unrestricted);
  assert(full_plan.ranges.size() == 1u && full_plan.ranges[0].address == 0u &&
         full_plan.ranges[0].size == 0x10000u);

  // CPU dirt newer than GPU ownership is reconciled before readback planning.
  tracker.mark_gpu_write(0x1000u, 8u);
  publications.mark_write(0x1002u, 2u);
  auto plan = tracker.plan_readback(publications, 0x1000u, 8u);
  assert(plan.ranges.size() == 2u);
  assert(plan.ranges[0].address == 0x1000u && plan.ranges[0].size == 2u);
  assert(plan.ranges[1].address == 0x1004u && plan.ranges[1].size == 4u);

  // A newer non-overlapping GPU generation doesn't invalidate an older plan.
  tracker.mark_gpu_write(0x2000u, 4u);
  auto independent = tracker.plan_readback(publications, 0x2000u, 4u);
  assert(independent.ranges.size() == 1u);
  tracker.mark_gpu_write(0x3000u, 4u);
  auto independent_safe =
      tracker.prepare_gpu_download(publications, independent.ranges[0]);
  assert(independent_safe.size() == 1u);

  // A newer overlapping generation splits the older plan and survives commit.
  tracker.mark_gpu_write(0x4000u, 8u);
  auto stale = tracker.plan_readback(publications, 0x4000u, 8u);
  tracker.mark_gpu_write(0x4002u, 2u);
  const auto safe = tracker.prepare_gpu_download(publications, stale.ranges[0]);
  assert(safe.size() == 2u);
  for (const auto& range : safe) {
    const auto epoch = publications.mark_write(range.address, range.size);
    assert(tracker.commit_gpu_download(publications, range, range.address,
                                       range.size, epoch));
  }
  assert(tracker.has_gpu_dirty(0x4002u, 2u));
  assert(!tracker.has_gpu_dirty(0x4000u, 2u));
  assert(!tracker.has_gpu_dirty(0x4004u, 4u));

  // The physical readback window is the race boundary between GPU staging and
  // CPU RAM. A CPU writer entering while the window is held must wait, then
  // deterministically become the later writer after the GPU bytes are applied.
  AddressSpace memory(GuestTranslationMode::Compact);
  assert(memory.initialize());
  constexpr GuestAddress kWord = 0x00400000u;
  assert(memory.commit_fixed(kWord, kBasePageSize, kReadWrite));
  const auto physical = memory.get_physical_address(kWord);
  assert(physical != 0xFFFFFFFFu);

  std::atomic<bool> entered{false};
  std::atomic<bool> completed{false};
  std::thread writer;
  {
    auto window = memory.physical_write_window();
    assert(window);
    writer = std::thread([&] {
      entered.store(true, std::memory_order_release);
      memory.write32_be(kWord, 0xAABBCCDDu);
      completed.store(true, std::memory_order_release);
    });
    while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
    for (std::uint32_t i = 0; i < 2000u; ++i) std::this_thread::yield();
    assert(!completed.load(std::memory_order_acquire));

    const std::array gpu_bytes{std::byte{0x11}, std::byte{0x22},
                               std::byte{0x33}, std::byte{0x44}};
    std::uint64_t epoch = 0u;
    assert(window.write(physical, gpu_bytes, &epoch));
    assert(epoch != 0u);
    assert(memory.read32_be(kWord) == 0x11223344u);
  }
  writer.join();
  assert(completed.load(std::memory_order_acquire));
  assert(memory.read32_be(kWord) == 0xAABBCCDDu);

  // Shared-host-visible backends receive visibility work rather than copies.
  GuestMemoryGpuCoherency uma;
  uma.reset(0x10000u, false, GpuMemoryTopology::SharedHostVisible);
  publications.mark_write(0x5000u, 4u);
  const auto upload = uma.plan_upload(publications, 0x5000u, 4u);
  assert(upload.action == GpuSynchronizationAction::VisibilityOnly);
  uma.mark_gpu_write(0x6000u, 4u);
  const auto readback = uma.plan_readback(publications, 0x6000u, 4u);
  assert(readback.action == GpuSynchronizationAction::VisibilityOnly);

  std::cout << "xenon_gpu_coherency_tests: ok\n";
  return 0;
}

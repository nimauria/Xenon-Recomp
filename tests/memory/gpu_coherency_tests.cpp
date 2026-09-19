#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>

#include "xenon/memory/address_space.hpp"
#include "xenon/memory/gpu_coherency.hpp"

using namespace xenon::memory;

int main() {
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

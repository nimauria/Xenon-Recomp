// Per-draw redundancy filters (include/xenon/gpu/bind_shadow.hpp) and the
// texture dirty tracker's page-bucket index.

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xenon/gpu/bind_shadow.hpp"
#include "xenon/gpu/perf_stats.hpp"
#include "xenon/gpu/texture.hpp"
#include "xenon/memory/coherency.hpp"

namespace gpu = xenon::gpu;
namespace perf = gpu::perf;

namespace {

struct Viewport {
  float x{}, y{}, w{}, h{};
};

void test_bind_shadow_skips_unchanged_values() {
  gpu::BindCounts counts;
  gpu::BindShadow<Viewport> viewport;
  const Viewport a{0, 0, 1280, 720};
  const Viewport b{0, 0, 640, 360};
  assert(viewport.update(a, counts));    // nothing known: must set
  assert(!viewport.update(a, counts));   // unchanged: skipped
  assert(!viewport.update(a, counts));
  assert(viewport.update(b, counts));    // changed
  assert(!viewport.update(b, counts));
  assert(viewport.update(a, counts));    // back again is a change
  assert(counts.set == 3u && counts.skipped == 3u);
  // Invalidation (root signature / heap change, new command list) forgets.
  viewport.invalidate();
  assert(!viewport.known());
  assert(viewport.update(a, counts));
  assert(counts.set == 4u);
  // Bytewise comparison: the same NaN bit pattern is "unchanged".
  gpu::BindShadow<std::array<float, 4>> blend;
  const std::array<float, 4> nan_factor{std::numeric_limits<float>::quiet_NaN(), 0, 0, 0};
  assert(blend.update(nan_factor, counts));
  assert(!blend.update(nan_factor, counts));
}

void test_bind_counts_flush_to_perf() {
  (void)perf::take_line(1.0);
  gpu::BindCounts counts;
  counts.set = 7;
  counts.skipped = 93;
  counts.flush();
  assert(counts.set == 0 && counts.skipped == 0);
  assert(perf::peek(perf::Counter::BindsSet) == 7u);
  assert(perf::peek(perf::Counter::BindsSkipped) == 93u);
  const auto line = perf::take_line(1.0);
  assert(line.find("binds set 7 skip 93") != std::string::npos);
}

// A simulated constants buffer: slot s lives at s * kSize. The cache decides
// which slot a draw points at; every draw must read the constants it staged.
struct SimulatedBuffer {
  static constexpr std::size_t kSize = 256;
  std::vector<std::byte> bytes = std::vector<std::byte>(kSize * 64);
  std::size_t writes{};
  void write(std::uint32_t slot, std::span<const std::byte> staged) {
    std::memcpy(bytes.data() + slot * kSize, staged.data(), staged.size());
    ++writes;
  }
  [[nodiscard]] std::byte first_byte(std::uint32_t slot) const { return bytes[slot * kSize]; }
};

void test_identical_draws_upload_constants_once() {
  gpu::ConstantsSlotCache cache;
  SimulatedBuffer buffer;
  std::array<std::byte, SimulatedBuffer::kSize> staged{};
  staged[0] = std::byte{0x11};
  const auto uploads_before = perf::lifetime(perf::Counter::ConstantUploads);
  const auto reuses_before = perf::lifetime(perf::Counter::ConstantReuses);
  std::vector<std::uint32_t> slots;
  for (std::uint32_t draw = 0; draw < 50; ++draw)
    slots.push_back(cache.resolve(1, 7, draw, staged,
                                  [&](std::uint32_t slot) { buffer.write(slot, staged); }));
  assert(buffer.writes == 1u);
  assert(cache.uploads() == 1u && cache.reuses() == 49u);
  for (const auto slot : slots) assert(slot == 0u);
  assert(perf::lifetime(perf::Counter::ConstantUploads) == uploads_before + 1u);
  assert(perf::lifetime(perf::Counter::ConstantReuses) == reuses_before + 49u);
}

void test_a_constant_change_is_seen_by_later_draws_only() {
  gpu::ConstantsSlotCache cache;
  SimulatedBuffer buffer;
  std::array<std::byte, SimulatedBuffer::kSize> staged{};
  const auto draw = [&](std::uint32_t index, std::byte value) {
    staged[0] = value;
    return cache.resolve(0, 3, index, staged,
                         [&](std::uint32_t slot) { buffer.write(slot, staged); });
  };
  const auto first = draw(0, std::byte{1});
  const auto second = draw(1, std::byte{1});
  const auto third = draw(2, std::byte{2});   // the change
  const auto fourth = draw(3, std::byte{2});
  const auto back = draw(4, std::byte{1});    // an earlier value again: new upload
  assert(first == second && third == fourth);
  assert(third != first && back != first && back != third);
  assert(buffer.writes == 3u);
  // The draws before the change still see the old value, the later ones the new.
  assert(buffer.first_byte(first) == std::byte{1});
  assert(buffer.first_byte(second) == std::byte{1});
  assert(buffer.first_byte(third) == std::byte{2});
  assert(buffer.first_byte(fourth) == std::byte{2});
  assert(buffer.first_byte(back) == std::byte{1});
  // A change in the last byte (the draw-state tail) counts too.
  staged[SimulatedBuffer::kSize - 1] = std::byte{9};
  const auto tail = draw(5, std::byte{1});
  assert(tail != back);
}

void test_slot_reuse_rules() {
  gpu::ConstantsSlotCache cache;
  SimulatedBuffer buffer;
  std::array<std::byte, SimulatedBuffer::kSize> staged{};
  const auto write = [&](std::uint32_t slot) { buffer.write(slot, staged); };
  assert(cache.resolve(1, 10, 0, staged, write) == 0u);
  // The same slot cannot refer to itself, another frame has its own buffer,
  // and a new batch rewrites the frame's slots from 0.
  assert(!cache.reusable(1, 10, 0, staged));
  assert(!cache.reusable(2, 10, 1, staged));
  assert(!cache.reusable(1, 11, 1, staged));
  assert(cache.reusable(1, 10, 1, staged) == 0u);
  assert(cache.resolve(1, 11, 0, staged, write) == 0u);  // new batch: uploads again
  assert(buffer.writes == 2u);
  cache.reset();
  assert(!cache.reusable(1, 11, 1, staged));
}


gpu::TextureLayout layout_of(std::initializer_list<std::pair<std::uint32_t, std::uint64_t>> ranges) {
  gpu::TextureLayout layout;
  layout.valid = true;
  for (const auto& [address, size] : ranges) {
    gpu::TextureSubresourceLayout sub;
    sub.guest_address = address;
    sub.guest_size_bytes = size;
    layout.subresources.push_back(sub);
  }
  return layout;
}

void test_texture_tracker_marks_only_overlapping_textures() {
  gpu::TextureDirtyTracker tracker;
  // Three textures far apart, one spanning several buckets with a mip chain
  // elsewhere, and one adjacent to the first.
  tracker.track_clean(1, layout_of({{0x100000, 0x4000}}), 0);
  tracker.track_clean(2, layout_of({{0x900000, 0x400000}, {0x1F000000, 0x1000}}), 0);
  tracker.track_clean(3, layout_of({{0x104000, 0x4000}}), 0);
  tracker.track_clean(4, layout_of({{0x5000000, 0x10000}}), 0);
  assert(tracker.tracked_count() == 4u);
  for (std::uint64_t key = 1; key <= 4; ++key) assert(!tracker.is_dirty(key));

  // An unrelated write leaves every texture clean (and examines none).
  tracker.mark_dirty(0x3000000, 0x100);
  for (std::uint64_t key = 1; key <= 4; ++key) assert(!tracker.is_dirty(key));
  assert(tracker.candidates_for(0x3000000, 0x100) == 0u);
  assert(tracker.textures_marked() == 0u);

  // Touching the byte just before / after a texture does not dirty it.
  tracker.mark_dirty(0x100000 - 4, 4);
  assert(!tracker.is_dirty(1));
  tracker.mark_dirty(0x104000 - 4, 8);  // last 4 of texture 1, first 4 of 3
  assert(tracker.is_dirty(1) && tracker.is_dirty(3) && !tracker.is_dirty(2) && !tracker.is_dirty(4));
  assert(tracker.textures_marked() == 2u);
  // Only the textures sharing a bucket with the write were looked at.
  assert(tracker.candidates_for(0x104000 - 4, 8) <= 2u);

  // A write into the middle of the large texture dirties just it.
  assert(tracker.consume_dirty(1) && tracker.consume_dirty(3));
  tracker.mark_dirty(0xA00000, 4);
  assert(tracker.is_dirty(2) && !tracker.is_dirty(1) && !tracker.is_dirty(3));
  assert(tracker.consume_dirty(2));
  // Its second range, far away, still dirties it.
  tracker.mark_dirty(0x1F000800, 4);
  assert(tracker.is_dirty(2));
  // One write spanning everything dirties exactly the overlapped textures.
  assert(tracker.consume_dirty(2));
  tracker.mark_dirty(0x0, 0x1000000);
  assert(tracker.is_dirty(1) && tracker.is_dirty(2) && tracker.is_dirty(3) && !tracker.is_dirty(4));
  assert(tracker.mark_calls() == 6u);
}

void test_texture_tracker_removal_unindexes() {
  gpu::TextureDirtyTracker tracker;
  tracker.track_clean(10, layout_of({{0x200000, 0x20000}}), 0);
  tracker.track_clean(11, layout_of({{0x200000, 0x20000}}), 0);
  assert(tracker.candidates_for(0x210000, 4) == 2u);
  tracker.erase(10);
  assert(tracker.tracked_count() == 1u);
  assert(tracker.candidates_for(0x210000, 4) == 1u);
  tracker.mark_dirty(0x210000, 4);
  assert(!tracker.is_dirty(10) && tracker.is_dirty(11));
  // Re-tracking under the same key moves it: the old range no longer hits it.
  assert(tracker.consume_dirty(11));
  tracker.track_clean(11, layout_of({{0x800000, 0x1000}}), 0);
  assert(tracker.candidates_for(0x210000, 4) == 0u);
  tracker.mark_dirty(0x210000, 4);
  assert(!tracker.is_dirty(11));
  tracker.mark_dirty(0x800000, 4);
  assert(tracker.is_dirty(11));
  tracker.clear();
  assert(tracker.tracked_count() == 0u && tracker.candidates_for(0x800000, 4) == 0u);
  tracker.mark_dirty(0x800000, 4);  // nothing tracked: harmless
  // Ranges past physical memory (never indexed below a bucket) still work.
  tracker.track_clean(12, layout_of({{0xFFFFF000u, 0x2000}}), 0);
  tracker.mark_dirty(0xFFFFFF00u, 4);
  assert(tracker.is_dirty(12));
}

void test_texture_tracker_consume_skips_walks_when_nothing_was_written() {
  xenon::memory::GuestMemoryCoherency coherency;
  gpu::TextureDirtyTracker tracker;
  const auto layout = layout_of({{0x300000, 0x40000}});
  coherency.mark_write(0x2000, 4);  // some unrelated write
  auto epoch = coherency.current_epoch();
  tracker.track_clean(20, layout, epoch);
  // Nothing newer than the texture's consumed epoch exists: no page walk.
  assert(!tracker.consume_dirty(20, coherency, epoch));
  assert(!tracker.consume_dirty(20, coherency, epoch));
  assert(tracker.consume_walks_skipped() == 2u && tracker.consume_walks() == 0u);
  // An unrelated write makes the walk necessary, finds nothing, and the
  // texture stays clean.
  coherency.mark_write(0x2000, 4);
  epoch = coherency.current_epoch();
  assert(!tracker.consume_dirty(20, coherency, epoch));
  assert(tracker.consume_walks() == 1u);
  // A write to the texture is found; once consumed, clean again and skipped.
  coherency.mark_write(0x320000, 4);
  epoch = coherency.current_epoch();
  assert(tracker.consume_dirty(20, coherency, epoch));
  assert(!tracker.consume_dirty(20, coherency, epoch));
  assert(tracker.consume_walks_skipped() == 3u);
  // An explicit GPU-write mark is honoured without any walk.
  tracker.mark_dirty(0x300000, 4);
  assert(tracker.consume_dirty(20, coherency, epoch));
}
}  // namespace

int main() {
  test_bind_shadow_skips_unchanged_values();
  test_bind_counts_flush_to_perf();
  test_identical_draws_upload_constants_once();
  test_a_constant_change_is_seen_by_later_draws_only();
  test_slot_reuse_rules();
  test_texture_tracker_marks_only_overlapping_textures();
  test_texture_tracker_removal_unindexes();
  test_texture_tracker_consume_skips_walks_when_nothing_was_written();
  std::cout << "dedup tests passed\n";
  return 0;
}

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
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xenon/gpu/bind_shadow.hpp"
#include "xenon/gpu/ir.hpp"
#include "xenon/gpu/perf_stats.hpp"
#include "xenon/gpu/resource_ir.hpp"
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

// The tracked staging path: a draw is a stage() of the register file into the
// staging buffer, then resolve() against the slot cache.
using FullBuffer = std::array<std::byte, gpu::kShaderConstantBufferBytes>;
struct TrackedDraws {
  gpu::ResourceStateTracker tracker;
  gpu::ConstantsSlotCache cache;
  FullBuffer staging{};
  std::vector<std::byte> bytes = std::vector<std::byte>(gpu::kShaderConstantBufferBytes * 64);
  std::size_t writes{};
  std::uint32_t slot{};
  std::vector<std::uint32_t> draw_slots;
  void write(std::uint32_t index, std::uint32_t value) {
    tracker.apply(xenon::gpu::ir::RegisterWrite{index, value});
  }
  // Records one draw; returns the slot it points at.
  std::uint32_t draw() {
    const auto staged = cache.stage(tracker, staging);
    assert(!staged.empty());
    const auto result = cache.resolve_staged(1, 5, slot, staging, [&](std::uint32_t s) {
      std::memcpy(bytes.data() + std::size_t(s) * gpu::kShaderConstantBufferBytes, staging.data(),
                  staging.size());
      ++writes;
    });
    ++slot;
    draw_slots.push_back(result);
    return result;
  }
  [[nodiscard]] FullBuffer expected() const {
    FullBuffer full{};
    const bool ok = tracker.write_constant_buffer(full);
    assert(ok);
    (void)ok;
    return full;
  }
  [[nodiscard]] bool slot_holds(std::uint32_t s, const FullBuffer& full) const {
    return std::memcmp(bytes.data() + std::size_t(s) * gpu::kShaderConstantBufferBytes, full.data(),
                       full.size()) == 0;
  }
};

void test_identical_draws_skip_the_constant_fill_and_compare() {
  TrackedDraws draws;
  draws.write(0x4000, 0x3F800000u);
  draws.write(0x4900, 5);
  const auto fills_before = perf::lifetime(perf::Counter::ConstantFillsSkipped);
  const auto blocks_before = perf::lifetime(perf::Counter::ConstantBlocksFilled);
  for (int i = 0; i < 50; ++i) draws.draw();
  // The first draw fills everything; the 49 after it find no constant register
  // moved: no block rewritten, nothing but the draw-state tail compared.
  assert(draws.cache.fills_skipped() == 49u);
  assert(perf::lifetime(perf::Counter::ConstantFillsSkipped) == fills_before + 49u);
  const auto blocks = perf::lifetime(perf::Counter::ConstantBlocksFilled) - blocks_before;
  assert(blocks == 32u + 1u);  // 32 float blocks + the misc section, first draw only
  assert(draws.writes == 1u);
  for (const auto slot : draws.draw_slots) assert(slot == 0u);
  assert(draws.slot_holds(0u, draws.expected()));
  // Unrelated registers (render state) change only the always-compared tail.
  draws.write(0x2202, 0x12);
  const auto before = draws.writes;
  draws.draw();
  assert(draws.writes == before + 1u);  // the draw-state words differ: new slot
  assert(draws.slot_holds(draws.draw_slots.back(), draws.expected()));
}

void test_one_register_change_is_seen_by_the_next_draw_only() {
  TrackedDraws draws;
  draws.write(0x4000 + 17 * 4, 0x11111111u);
  const auto first = draws.draw();
  const auto second = draws.draw();
  assert(first == second);
  const auto first_full = draws.expected();
  // One float constant register changes: only its 256-byte block is rewritten.
  const auto blocks_before = perf::lifetime(perf::Counter::ConstantBlocksFilled);
  draws.write(0x4000 + 17 * 4 + 2, 0x22222222u);
  const auto third = draws.draw();
  assert(perf::lifetime(perf::Counter::ConstantBlocksFilled) == blocks_before + 1u);
  assert(third != first);
  const auto fourth = draws.draw();
  assert(fourth == third);
  // The earlier draws' slots still hold what they saw; the later ones the new value.
  assert(draws.slot_holds(first, first_full));
  assert(draws.slot_holds(third, draws.expected()));
  // Writing the value it already holds is no change at all.
  draws.write(0x4000 + 17 * 4 + 2, 0x22222222u);
  assert(draws.draw() == third);
  // A fetch constant, a loop constant and a bool constant each reach the buffer.
  draws.write(0x4800 + 6 * 3 + 1, 0x00000003u);
  const auto after_fetch = draws.draw();
  assert(after_fetch != third && draws.slot_holds(after_fetch, draws.expected()));
  draws.write(0x4908 + 4, 0x0000FF03u);
  const auto after_loop = draws.draw();
  assert(after_loop != after_fetch && draws.slot_holds(after_loop, draws.expected()));
  draws.write(0x4901, 0xFFFFFFFFu);
  const auto after_bool = draws.draw();
  assert(after_bool != after_loop && draws.slot_holds(after_bool, draws.expected()));
}

void test_incremental_staging_equals_a_full_rebuild() {
  // Random register writes between draws: the incrementally maintained staging
  // buffer must always equal write_constant_buffer() of the same registers, a
  // reset tracker included.
  TrackedDraws draws;
  std::mt19937 rng(12345);
  for (int round = 0; round < 400; ++round) {
    const auto writes = rng() % 4u;
    for (unsigned i = 0; i < writes; ++i) {
      std::uint32_t index = 0;
      switch (rng() % 5u) {
        case 0: index = 0x4000 + rng() % 0x800; break;
        case 1: index = 0x4800 + rng() % 0xC0; break;
        case 2: index = 0x4900 + rng() % 8; break;
        case 3: index = 0x4908 + rng() % 32; break;
        default: index = 0x2000 + rng() % 0x400; break;
      }
      draws.write(index, static_cast<std::uint32_t>(rng()));
    }
    if (round == 200) {
      draws.tracker.reset();
      draws.write(0x4001, 7);
    }
    if (draws.slot >= 60u) {
      draws.slot = 0;
      draws.cache.reset();
    }
    const auto slot = draws.draw();
    const auto full = draws.expected();
    assert(std::memcmp(draws.staging.data(), full.data(), full.size()) == 0);
    assert(draws.slot_holds(slot, full));
  }
}

void test_staging_changed_behind_the_cache_is_refilled() {
  TrackedDraws draws;
  draws.write(0x4000, 1);
  draws.draw();
  draws.cache.mark_all_changed();  // someone scribbled over the staging buffer
  draws.staging[0] = std::byte{0x77};
  const auto slot = draws.draw();
  assert(draws.slot_holds(slot, draws.expected()));
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
  // Answered from the write journal: one epoch to look at, no page walk.
  assert(tracker.consume_walks() == 0u && tracker.consume_journal_checks() == 1u);
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

void test_texture_consume_uses_the_journal_with_exact_results() {
  xenon::memory::GuestMemoryCoherency coherency;
  gpu::TextureDirtyTracker tracker;
  // Three big textures (64 pages each), one small. All clean at the same epoch.
  coherency.mark_write(0x1000, 4);
  auto epoch = coherency.current_epoch();
  tracker.track_clean(1, layout_of({{0x400000, 0x40000}}), epoch);
  tracker.track_clean(2, layout_of({{0x800000, 0x40000}, {0x900000, 0x1000}}), epoch);
  tracker.track_clean(3, layout_of({{0xC00000, 0x40000}}), epoch);
  tracker.track_clean(4, layout_of({{0xD00000, 0x1000}}), epoch);
  // Unrelated writes leave every texture clean, with one epoch examined per
  // write rather than a walk over the textures' pages.
  for (std::uint32_t i = 0; i < 5; ++i) coherency.mark_write(0x2000000 + i * 0x1000, 16);
  epoch = coherency.current_epoch();
  for (std::uint64_t key = 1; key <= 3; ++key) assert(!tracker.consume_dirty(key, coherency, epoch));
  assert(tracker.consume_walks() == 0u && tracker.consume_journal_checks() == 3u);
  // Texture 4 is one page: a walk of one page is cheaper than five epochs.
  assert(!tracker.consume_dirty(4, coherency, epoch));
  assert(tracker.consume_walks() == 1u);
  // A write overlapping just texture 2's second range dirties exactly texture 2;
  // a write to the same page as texture 4 but outside its bytes still dirties
  // it (page granularity, as the page walk it replaces).
  coherency.mark_write(0x900100, 4);
  coherency.mark_write(0xD00F00, 4);
  epoch = coherency.current_epoch();
  assert(!tracker.consume_dirty(1, coherency, epoch));
  assert(tracker.consume_dirty(2, coherency, epoch));
  assert(!tracker.consume_dirty(3, coherency, epoch));
  assert(tracker.consume_dirty(4, coherency, epoch));
  // Consumed: clean again, no writes since.
  for (std::uint64_t key = 1; key <= 4; ++key) assert(!tracker.consume_dirty(key, coherency, epoch));
  // A write at the page just before / after a texture does not dirty it.
  coherency.mark_write(0x400000 - 4, 4);
  coherency.mark_write(0x440000, 4);
  epoch = coherency.current_epoch();
  assert(!tracker.consume_dirty(1, coherency, epoch));
  // A write past the consumer's through_epoch counts as dirty now (like the
  // page epoch walk) and, the consumer only advancing to through_epoch, again
  // on the next consume.
  coherency.mark_write(0x400000, 4);
  assert(tracker.consume_dirty(1, coherency, epoch));
  // More epochs than the journal holds fall back to the page walk, same answer.
  for (std::uint32_t i = 0; i < xenon::memory::GuestMemoryCoherency::kWriteJournalCapacity + 8u; ++i)
    coherency.mark_write(0x3000000, 4);
  coherency.mark_write(0xC02000, 4);
  epoch = coherency.current_epoch();
  const auto walks_before = tracker.consume_walks();
  assert(tracker.consume_dirty(3, coherency, epoch));
  assert(tracker.consume_walks() == walks_before + 1u);
  assert(tracker.consume_dirty(1, coherency, epoch));  // the 0x400000 write, seen again
  assert(!tracker.consume_dirty(1, coherency, epoch));
}
}  // namespace

int main() {
  test_bind_shadow_skips_unchanged_values();
  test_bind_counts_flush_to_perf();
  test_identical_draws_upload_constants_once();
  test_a_constant_change_is_seen_by_later_draws_only();
  test_slot_reuse_rules();
  test_identical_draws_skip_the_constant_fill_and_compare();
  test_one_register_change_is_seen_by_the_next_draw_only();
  test_incremental_staging_equals_a_full_rebuild();
  test_staging_changed_behind_the_cache_is_refilled();
  test_texture_tracker_marks_only_overlapping_textures();
  test_texture_tracker_removal_unindexes();
  test_texture_tracker_consume_skips_walks_when_nothing_was_written();
  test_texture_consume_uses_the_journal_with_exact_results();
  std::cout << "dedup tests passed\n";
  return 0;
}

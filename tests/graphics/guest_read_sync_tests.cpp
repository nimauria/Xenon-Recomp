// Texture reads as of GPU/CPU sync points (xenon/gpu/guest_read_sync.hpp).

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

#include "xenon/gpu/guest_read_sync.hpp"
#include "xenon/memory/address_space.hpp"

using xenon::gpu::GuestReadSync;
using xenon::memory::AddressSpace;

namespace {

constexpr std::uint64_t kKey = 0xA11CEu;
constexpr std::uint32_t kBase = 0x00400000u;
constexpr std::uint32_t kSize = 3u * 4096u;

void fill(AddressSpace& memory, std::uint32_t address, std::uint32_t size, std::byte value) {
  std::vector<std::byte> bytes(size, value);
  assert(memory.write_physical(address, bytes));
}

const GuestReadSync::Classify kTrackAll = [](std::uint64_t, std::uint32_t, std::uint32_t) {
  return GuestReadSync::Tracking::Track;
};

// Regression (Ace Combat 6 intro movie: the top of the picture a frame ahead,
// in the wrong colours). The interrupt before a movie draw wakes the decoder,
// which refills the planes that draw samples; the draw ran milliseconds later
// and read the planes part-rewritten. What the draw decodes is the plane as it
// stood at the sync point, however much the CPU has rewritten since.
void test_draw_reads_the_texture_as_of_the_sync_point() {
  AddressSpace memory;
  assert(memory.initialize());
  GuestReadSync sync;
  fill(memory, kBase, kSize, std::byte{0x11});
  sync.refreshed(kKey, kBase, kSize, memory.coherency().current_epoch());

  // The next frame is decoded into the plane, then the sync point.
  fill(memory, kBase, kSize, std::byte{0x22});
  sync.sync(memory, kTrackAll);
  // After it, the decoder starts on the frame after that.
  fill(memory, kBase, 4096u, std::byte{0x33});

  assert(!sync.unchanged_at_sync(kKey));
  const auto capture = sync.take_capture(kKey, kBase, kSize);
  assert(capture.has_value() && capture->size() == kSize);
  for (const auto byte : *capture) assert(byte == std::byte{0x22});
  // A copy is decoded once.
  assert(!sync.take_capture(kKey, kBase, kSize).has_value());
}

// A texture unchanged at the sync point keeps its cached image when the CPU
// writes it afterwards: the hardware sampled it before that write.
void test_write_after_the_sync_point_is_not_visible_yet() {
  AddressSpace memory;
  assert(memory.initialize());
  GuestReadSync sync;
  fill(memory, kBase, kSize, std::byte{0x11});
  sync.refreshed(kKey, kBase, kSize, memory.coherency().current_epoch());
  sync.sync(memory, kTrackAll);
  fill(memory, kBase, 4096u, std::byte{0x44});
  assert(sync.unchanged_at_sync(kKey));
  assert(!sync.take_capture(kKey, kBase, kSize).has_value());

  // The next sync point exposes the write.
  sync.sync(memory, kTrackAll);
  assert(!sync.unchanged_at_sync(kKey));
  const auto capture = sync.take_capture(kKey, kBase, kSize);
  assert(capture.has_value() && (*capture)[0] == std::byte{0x44} &&
         (*capture)[4096] == std::byte{0x11});
}

// Every sync point keeps a capture current: one taken earlier and still
// matching memory serves a later sync point, and a later change replaces it.
void test_capture_follows_each_sync_point() {
  AddressSpace memory;
  assert(memory.initialize());
  GuestReadSync sync;
  fill(memory, kBase, kSize, std::byte{0x11});
  sync.refreshed(kKey, kBase, kSize, memory.coherency().current_epoch());
  fill(memory, kBase, kSize, std::byte{0x22});
  sync.sync(memory, kTrackAll);
  sync.sync(memory, kTrackAll);  // unchanged since: the capture stands
  fill(memory, kBase, kSize, std::byte{0x55});
  sync.sync(memory, kTrackAll);
  const auto capture = sync.take_capture(kKey, kBase, kSize);
  assert(capture.has_value() && (*capture)[kSize - 1u] == std::byte{0x55});
}

// What the GPU itself writes (a resolve, a memory export) is ordered with the
// commands: a later draw reads it, never a capture from before it.
void test_gpu_writes_are_visible_to_later_draws() {
  AddressSpace memory;
  assert(memory.initialize());
  GuestReadSync sync;
  sync.refreshed(kKey, kBase, kSize, memory.coherency().current_epoch());
  fill(memory, kBase, kSize, std::byte{0x22});
  sync.sync(memory, kTrackAll);
  sync.backend_wrote(kBase + kSize - 4u, 16u);
  assert(!sync.unchanged_at_sync(kKey));
  assert(!sync.take_capture(kKey, kBase, kSize).has_value());

  // A write elsewhere leaves the texture alone.
  constexpr std::uint64_t kOther = 0xB0Bu;
  sync.refreshed(kOther, kBase, kSize, memory.coherency().current_epoch());
  sync.sync(memory, kTrackAll);
  sync.backend_wrote(kBase + kSize, 4096u);
  assert(sync.unchanged_at_sync(kOther));
}

xenon::gpu::TextureLayout one_range_layout(std::uint32_t base, std::uint32_t size) {
  xenon::gpu::TextureLayout layout{};
  layout.subresources.emplace_back();
  layout.subresources.back().guest_address = base;
  layout.subresources.back().guest_size_bytes = size;
  return layout;
}

// After one sync point, Ace Combat 6 resolves each frame's post-process chain
// into a texture and its composite draw samples it. The resolve's write is
// newer than the epoch the composite reads at; the composite must still decode
// it, never sample the image cached when the texture was last decoded.
void test_draw_after_a_resolve_decodes_it_before_the_next_sync_point() {
  AddressSpace memory;
  assert(memory.initialize());
  GuestReadSync reads;
  xenon::gpu::TextureDirtyTracker dirty;
  const auto layout = one_range_layout(kBase, kSize);

  // An earlier frame decoded the texture.
  fill(memory, kBase, kSize, std::byte{0x11});
  const auto decoded_at = memory.coherency().current_epoch();
  dirty.track_clean(kKey, layout, decoded_at);
  reads.refreshed(kKey, kBase, kSize, decoded_at);

  // This frame: the sync point, then the resolve, then the composite.
  const auto sync_epoch = reads.sync(memory, kTrackAll);
  fill(memory, kBase, kSize, std::byte{0x22});
  xenon::gpu::note_backend_write(dirty, reads, kBase, kSize);
  const auto read_epoch = reads.read_epoch(memory);
  assert(read_epoch == sync_epoch);
  assert(xenon::gpu::cached_texture_stale(dirty, reads, kKey, memory.coherency(), read_epoch));

  // The composite decoded it; a second draw sampling it is not stale again.
  dirty.track_clean(kKey, layout, read_epoch);
  reads.refreshed(kKey, kBase, kSize, read_epoch);
  assert(!xenon::gpu::cached_texture_stale(dirty, reads, kKey, memory.coherency(), read_epoch));

  // A GPU write elsewhere leaves it cached.
  xenon::gpu::note_backend_write(dirty, reads, kBase + kSize, 4096u);
  assert(!xenon::gpu::cached_texture_stale(dirty, reads, kKey, memory.coherency(), read_epoch));
  // A write of no bytes is no write.
  xenon::gpu::note_backend_write(dirty, reads, kBase, 0u);
  assert(!xenon::gpu::cached_texture_stale(dirty, reads, kKey, memory.coherency(), read_epoch));
}

// The CPU, unlike the GPU, is not ordered with the draws: its write after the
// sync point stays invisible until the next one exposes it.
void test_cpu_write_after_the_sync_point_waits_for_the_next() {
  AddressSpace memory;
  assert(memory.initialize());
  GuestReadSync reads;
  xenon::gpu::TextureDirtyTracker dirty;
  const auto layout = one_range_layout(kBase, kSize);
  fill(memory, kBase, kSize, std::byte{0x11});
  const auto decoded_at = memory.coherency().current_epoch();
  dirty.track_clean(kKey, layout, decoded_at);
  reads.refreshed(kKey, kBase, kSize, decoded_at);

  const auto sync_epoch = reads.sync(memory, kTrackAll);
  fill(memory, kBase, 4096u, std::byte{0x33});
  assert(!xenon::gpu::cached_texture_stale(dirty, reads, kKey, memory.coherency(), sync_epoch));

  const auto next_sync = reads.sync(memory, kTrackAll);
  assert(xenon::gpu::cached_texture_stale(dirty, reads, kKey, memory.coherency(), next_sync));
}

// GPU-authored textures live in the mirror, not guest memory: never captured,
// never assumed unchanged. Textures no longer cached, or not sampled for
// kRecentSyncPoints sync points, stop being tracked.
void test_tracking_follows_the_texture_cache() {
  AddressSpace memory;
  assert(memory.initialize());
  GuestReadSync sync;
  sync.refreshed(kKey, kBase, kSize, memory.coherency().current_epoch());
  fill(memory, kBase, kSize, std::byte{0x22});
  sync.sync(memory, [](std::uint64_t, std::uint32_t, std::uint32_t) {
    return GuestReadSync::Tracking::GpuAuthored;
  });
  assert(!sync.unchanged_at_sync(kKey));
  assert(!sync.take_capture(kKey, kBase, kSize).has_value());

  sync.sync(memory, [](std::uint64_t, std::uint32_t, std::uint32_t) {
    return GuestReadSync::Tracking::Forget;
  });
  assert(!sync.unchanged_at_sync(kKey));

  sync.refreshed(kKey, kBase, kSize, memory.coherency().current_epoch());
  for (std::uint64_t i = 0; i < GuestReadSync::kRecentSyncPoints; ++i) sync.sync(memory, kTrackAll);
  assert(sync.unchanged_at_sync(kKey));
  sync.sync(memory, kTrackAll);
  assert(!sync.unchanged_at_sync(kKey));
  // Sampling keeps it tracked.
  sync.refreshed(kKey, kBase, kSize, memory.coherency().current_epoch());
  for (std::uint64_t i = 0; i < 3u * GuestReadSync::kRecentSyncPoints; ++i) {
    sync.sync(memory, kTrackAll);
    sync.reused(kKey);
  }
  assert(sync.unchanged_at_sync(kKey));
}

// Outside a ring drain (captures, tools) reads see memory as it is now.
void test_outside_a_drain_reads_see_current_memory() {
  AddressSpace memory;
  assert(memory.initialize());
  GuestReadSync sync;
  sync.refreshed(kKey, kBase, kSize, memory.coherency().current_epoch());
  fill(memory, kBase, kSize, std::byte{0x22});
  const auto epoch = sync.sync(memory, kTrackAll);
  assert(sync.active() && sync.read_epoch(memory) == epoch);
  sync.end();
  assert(!sync.active());
  fill(memory, kBase, kSize, std::byte{0x66});
  assert(sync.read_epoch(memory) > epoch);
  assert(!sync.unchanged_at_sync(kKey));
  assert(!sync.take_capture(kKey, kBase, kSize).has_value());
}

// A resolve's readback lands in guest memory after the texture was loaded
// from the mirror, which already held the resolved bytes. That write is not a
// CPU change: copying the texture at the next sync point (and reloading it)
// cost a CPU copy of every resolved texture a frame. A CPU write before or
// after the readback still is one.
void test_resolve_readback_is_not_a_cpu_change() {
  AddressSpace memory;
  assert(memory.initialize());
  GuestReadSync sync;
  const auto publish = [&](std::byte value) {
    const auto previous = memory.coherency().newest_page_epoch(kBase, kSize);
    fill(memory, kBase, kSize, value);
    const std::array writes{xenon::memory::GpuPublishedWrite{
        memory.coherency().current_epoch(), previous, kBase, kSize}};
    sync.gpu_published(writes);
  };
  fill(memory, kBase, kSize, std::byte{0x11});
  sync.refreshed(kKey, kBase, kSize, memory.coherency().current_epoch());
  publish(std::byte{0x55});
  sync.sync(memory, kTrackAll);
  assert(sync.unchanged_at_sync(kKey));
  assert(!sync.take_capture(kKey, kBase, kSize).has_value());

  // A CPU write after the readback is a change.
  fill(memory, kBase, 4096u, std::byte{0x66});
  sync.sync(memory, kTrackAll);
  assert(!sync.unchanged_at_sync(kKey));
  const auto after = sync.take_capture(kKey, kBase, kSize);
  assert(after.has_value() && (*after)[0] == std::byte{0x66} && (*after)[4096] == std::byte{0x55});

  // So is one before it: the readback does not hide it.
  constexpr std::uint64_t kOther = kKey + 1u;
  sync.refreshed(kOther, kBase, kSize, memory.coherency().current_epoch());
  fill(memory, kBase, 4096u, std::byte{0x77});
  publish(std::byte{0x88});
  sync.sync(memory, kTrackAll);
  assert(!sync.unchanged_at_sync(kOther));
  assert(sync.take_capture(kOther, kBase, kSize).has_value());
}

}  // namespace

int main() {
  test_draw_reads_the_texture_as_of_the_sync_point();
  test_write_after_the_sync_point_is_not_visible_yet();
  test_capture_follows_each_sync_point();
  test_gpu_writes_are_visible_to_later_draws();
  test_draw_after_a_resolve_decodes_it_before_the_next_sync_point();
  test_cpu_write_after_the_sync_point_waits_for_the_next();
  test_tracking_follows_the_texture_cache();
  test_outside_a_drain_reads_see_current_memory();
  test_resolve_readback_is_not_a_cpu_change();
  std::cout << "xenon_guest_read_sync_tests: ok\n";
  return 0;
}

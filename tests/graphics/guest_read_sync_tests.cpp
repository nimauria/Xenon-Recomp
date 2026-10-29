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

}  // namespace

int main() {
  test_draw_reads_the_texture_as_of_the_sync_point();
  test_write_after_the_sync_point_is_not_visible_yet();
  test_capture_follows_each_sync_point();
  test_gpu_writes_are_visible_to_later_draws();
  test_tracking_follows_the_texture_cache();
  test_outside_a_drain_reads_see_current_memory();
  std::cout << "xenon_guest_read_sync_tests: ok\n";
  return 0;
}

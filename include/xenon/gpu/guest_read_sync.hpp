#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

#include "xenon/memory/address_space.hpp"

namespace xenon::gpu {

// Guest-memory reads as of GPU/CPU sync points (gpu::Backend::
// sync_guest_reads()). Hardware samples a texture microseconds after the
// command processor passes the wait or interrupt before the draw; a host
// backend decodes it milliseconds later, by which time CPU code that sync
// point released may be rewriting it (AC6's movie decoder refills the planes
// the next draw samples). At every sync point each recently sampled texture
// the CPU changed is copied as it stands then, and a draw decodes that copy;
// one found unchanged was written, if at all, after the sync point, and keeps
// its cached image until a later sync point exposes the write.
class GuestReadSync {
 public:
  // A texture unsampled for this many sync points is no longer tracked.
  static constexpr std::uint64_t kRecentSyncPoints = 64;

  enum class Tracking : std::uint8_t {
    Track,         // captured here if the CPU changed it
    GpuAuthored,   // current on the GPU, not in guest memory: not captured
    Forget,        // no longer cached: stop tracking it
  };
  using Classify = std::function<Tracking(std::uint64_t key, std::uint32_t base,
                                          std::uint32_t size)>;

  // A sync point. Returns its coherency epoch.
  std::uint64_t sync(memory::AddressSpace& memory, const Classify& classify);
  // The drain ended: reads see memory as it is until the next sync point.
  void end() noexcept { active_ = false; }
  [[nodiscard]] bool active() const noexcept { return active_; }
  // The epoch draws read guest memory at: the latest sync point while one is
  // in force, otherwise now.
  [[nodiscard]] std::uint64_t read_epoch(const memory::AddressSpace& memory) const;

  // A draw uploaded texture `key` from [base, base + size) with its memory as
  // of `epoch` (or later).
  void refreshed(std::uint64_t key, std::uint32_t base, std::uint32_t size,
                 std::uint64_t epoch);
  // A draw sampled `key` from its cached image.
  void reused(std::uint64_t key) noexcept;
  // The copy the latest sync point holds of [base, base + size) for `key`;
  // moved out, so each copy is decoded once.
  [[nodiscard]] std::optional<std::vector<std::byte>> take_capture(std::uint64_t key,
                                                                   std::uint32_t base,
                                                                   std::uint32_t size);
  // The latest sync point found tracked texture `key` unchanged since it was
  // uploaded, so a CPU write the page epochs report came after it.
  [[nodiscard]] bool unchanged_at_sync(std::uint64_t key) const;
  // The GPU itself wrote [base, base + size) (a resolve, a memory export):
  // ordered with the commands, and visible to every later draw. Textures it
  // overlaps are no longer tracked, so the next draw reads them anew.
  void backend_wrote(std::uint32_t base, std::uint32_t size);

  void clear() noexcept;
  [[nodiscard]] std::uint64_t sync_points() const noexcept { return sync_points_; }

 private:
  struct Sampled {
    std::uint32_t base{};
    std::uint32_t size{};
    std::uint64_t last_sync_point{};
    // Changes through this epoch are in the uploaded image or the capture.
    std::uint64_t accounted_epoch{};
    bool gpu_authored{false};
  };
  struct Capture {
    std::vector<std::byte> bytes{};
    std::uint32_t base{};
  };
  std::unordered_map<std::uint64_t, Sampled> sampled_{};
  std::unordered_map<std::uint64_t, Capture> captures_{};
  bool active_{false};
  std::uint64_t epoch_{};
  std::uint64_t sync_points_{};
};

}  // namespace xenon::gpu

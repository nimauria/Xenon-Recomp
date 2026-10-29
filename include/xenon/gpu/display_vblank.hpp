#pragma once

#include <cstdint>
#include <memory>

namespace xenon::gpu {

// The guest's vertical blank is NTSC/HDMI 60 Hz. A frame the title shows for
// two vblanks stays up for exactly two host refreshes only if guest vblanks
// fall on host refreshes; a free-running 60 Hz timer drifts against the real
// display and frames alternate between one and three refreshes (visible judder).

inline constexpr double kGuestVblankHz = 60.0;

// How many host refreshes make one guest vblank: the whole multiple of the
// guest rate the host refresh rate is (60 Hz -> 1, 120 Hz -> 2, 240 Hz -> 4),
// or 0 when it is none (75 Hz, 144 Hz, 165 Hz), which cannot be locked and keeps
// timer-paced vblanks. Within a hertz of the multiple counts: displays run at
// 59.94 Hz, 119.88 Hz and the like.
[[nodiscard]] std::uint32_t host_refreshes_per_guest_vblank(double host_refresh_hz) noexcept;

// Waits for the vertical blank of the display a native window is on (the
// primary display without one). Backend-independent: the same display clock
// serves D3D12 and Vulkan presentation.
class HostDisplayVblank {
 public:
  HostDisplayVblank();
  ~HostDisplayVblank();
  HostDisplayVblank(const HostDisplayVblank&) = delete;
  HostDisplayVblank& operator=(const HostDisplayVblank&) = delete;

  // Resolves the display `native_window` is on (an HWND on Windows; null for
  // the primary display) and its refresh rate. Returns available().
  bool select(void* native_window);
  [[nodiscard]] bool available() const noexcept;
  // The selected display's refresh rate in hertz; 0 when unknown.
  [[nodiscard]] double refresh_hz() const noexcept;
  // Blocks until the selected display's next vertical blank. False when no
  // display is selected or the wait failed.
  bool wait();

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace xenon::gpu

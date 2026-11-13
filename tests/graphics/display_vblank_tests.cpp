// Host display vblank source the guest's vertical blank locks to
// (xenon/gpu/display_vblank.hpp).

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <vector>

#include "xenon/gpu/display_vblank.hpp"

using xenon::gpu::host_refreshes_per_guest_vblank;

namespace {

// Regression (Ace Combat 6 intro movie judder): guest vblanks came from a free
// 60 Hz timer that drifted against the display, so a frame held for two
// vblanks was shown for one or three refreshes. Guest vblanks now fall on
// every Nth host refresh where the rate is a whole multiple of 60 Hz; other
// rates cannot be locked and stay timer-paced.
void test_refresh_multiples() {
  assert(host_refreshes_per_guest_vblank(60.0) == 1u);
  assert(host_refreshes_per_guest_vblank(59.94) == 1u);
  assert(host_refreshes_per_guest_vblank(59.0) == 1u);  // 59.94 Hz as Windows reports it
  assert(host_refreshes_per_guest_vblank(61.0) == 1u);
  assert(host_refreshes_per_guest_vblank(120.0) == 2u);
  assert(host_refreshes_per_guest_vblank(119.88) == 2u);
  assert(host_refreshes_per_guest_vblank(119.0) == 2u);
  assert(host_refreshes_per_guest_vblank(180.0) == 3u);
  assert(host_refreshes_per_guest_vblank(240.0) == 4u);
  assert(host_refreshes_per_guest_vblank(480.0) == 8u);
  // Not a whole multiple: no lock.
  assert(host_refreshes_per_guest_vblank(50.0) == 0u);
  assert(host_refreshes_per_guest_vblank(75.0) == 0u);
  assert(host_refreshes_per_guest_vblank(90.0) == 0u);
  assert(host_refreshes_per_guest_vblank(100.0) == 0u);
  assert(host_refreshes_per_guest_vblank(144.0) == 0u);
  assert(host_refreshes_per_guest_vblank(165.0) == 0u);
  // Unknown or nonsense rates.
  assert(host_refreshes_per_guest_vblank(0.0) == 0u);
  assert(host_refreshes_per_guest_vblank(-60.0) == 0u);
  assert(host_refreshes_per_guest_vblank(20.0) == 0u);
  assert(host_refreshes_per_guest_vblank(std::nan("")) == 0u);
  assert(host_refreshes_per_guest_vblank(INFINITY) == 0u);
}

// Regression (Windows CI: the runner's virtual display reports 64 Hz and its
// vblank waits take 24.6 ms): a display whose waits outlast its reported
// refreshes is not one guest vblanks can lock to.
void test_waits_longer_than_the_refresh_are_rejected() {
  using std::chrono::microseconds;
  using xenon::gpu::display_wait_overran;
  // A real 60 Hz display: one refresh (16.7 ms), or a late arrival waiting a
  // little longer, is fine; two refreshes for a 120 Hz lock are fine.
  assert(!display_wait_overran(60.0, 1u, microseconds(16667)));
  assert(!display_wait_overran(60.0, 1u, microseconds(24000)));
  assert(!display_wait_overran(120.0, 2u, microseconds(16667)));
  // A wait returning at once is the GPU pump's early check, not this one.
  assert(!display_wait_overran(60.0, 1u, microseconds(10)));
  // CI's virtual display, and a 60 Hz display waiting like a 40 Hz one.
  assert(display_wait_overran(64.0, 1u, microseconds(24637)));
  assert(display_wait_overran(60.0, 1u, microseconds(26000)));
  assert(display_wait_overran(120.0, 2u, microseconds(21000)));
  // No usable refresh rate is never lockable.
  assert(display_wait_overran(0.0, 1u, microseconds(16667)));
  assert(display_wait_overran(std::nan(""), 1u, microseconds(16667)));
}

// On a host with a display the source waits for its refreshes. Whatever the
// display does, guest vblanks lock only to one whose waits follow its
// reported rate: a wait one refresh long is accepted, one that outlasts the
// refresh (a virtual display such as CI's) is rejected by
// display_wait_overran(), and one that returns at once by the GPU pump's
// early check.
void test_waits_follow_the_display() {
  xenon::gpu::HostDisplayVblank display;
  if (!display.select(nullptr)) {
#if defined(_WIN32)
    std::cout << "  no display output on this host; display vblank wait not exercised\n";
#endif
    assert(!display.available() && !display.wait());
    return;
  }
  assert(display.available());
  const double hz = display.refresh_hz();
  assert(hz > 1.0);
  using Clock = std::chrono::steady_clock;
  assert(display.wait());
  std::vector<double> periods_ms;
  auto last = Clock::now();
  for (int i = 0; i < 30; ++i) {
    assert(display.wait());
    const auto now = Clock::now();
    periods_ms.push_back(std::chrono::duration<double, std::milli>(now - last).count());
    last = now;
  }
  std::sort(periods_ms.begin(), periods_ms.end());
  const double median = periods_ms[periods_ms.size() / 2];
  const double expected = 1000.0 / hz;
  std::cout << "  display " << hz << " Hz, median wait " << median << " ms\n";
  const bool follows_refresh = std::abs(median - expected) < expected * 0.15;
  const bool overran = xenon::gpu::display_wait_overran(
      hz, 1u, std::chrono::nanoseconds(static_cast<std::int64_t>(median * 1e6)));
  const bool early = median < expected * 0.5;
  if (!follows_refresh)
    std::cout << "  this display does not wait at its reported rate; guest vblanks stay "
              << (overran ? "timer-paced (waits overrun)" : early ? "timer-paced (waits return early)"
                                                                   : "locked (within a refresh)")
              << "\n";
  // A display that is neither accurate, overrunning nor early would be
  // locked to while off its rate by up to half a refresh.
  assert(follows_refresh || overran || early);
}

}  // namespace

int main() {
  test_refresh_multiples();
  test_waits_longer_than_the_refresh_are_rejected();
  test_waits_follow_the_display();
  std::cout << "xenon_display_vblank_tests: ok\n";
  return 0;
}

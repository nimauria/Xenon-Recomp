#include <cassert>
#include <iostream>
#include <sstream>
#include <string>

#include "xenon/core/guest_profiler.hpp"

using xenon::core::GuestProfiler;

int main() {
  GuestProfiler profiler;
  // Second 0: the main thread mostly runs one loop, sometimes waits on a read.
  for (int i = 0; i < 6; ++i) profiler.record(10u + i, 4u, "MainThread", 0x822CD840u, {});
  for (int i = 0; i < 3; ++i) profiler.record(20u + i, 4u, "MainThread", 0, "xboxkrnl.exe!NtReadFile");
  profiler.record(30u, 4u, "MainThread", 0x821CCC24u, {});
  // Second 1 is its own bucket, and so is another thread.
  profiler.record(1500u, 4u, "MainThread", 0x821CCC24u, {});
  profiler.record(1500u, 7u, "", 0x823A0000u, {});
  assert(profiler.samples() == 12u);

  std::ostringstream out;
  profiler.write(out, 2);
  const auto text = out.str();
  std::cout << text;
  // Ranked by share and cut to the requested top entries (the single 821CCC24
  // sample of second 0 is dropped); each second and thread is its own bucket.
  assert(text.find("== 0s t4 (MainThread): 10 samples\n"
                   "        6  60.0%  822CD840\n"
                   "        3  30.0%  xboxkrnl.exe!NtReadFile\n"
                   "== 1s t4 (MainThread): 1 samples\n"
                   "        1 100.0%  821CCC24\n"
                   "== 1s t7: 1 samples\n") != std::string::npos);
  std::cout << "guest profiler tests passed\n";
  return 0;
}

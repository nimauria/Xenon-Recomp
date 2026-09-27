// Part 15 of the AC6 Runtime Readiness / Platform Fidelity pass: boot phase
// checkpoints. Tests xenon::core::BootCheckpointTracker in isolation,
// independent of XenonSession - see tests/core/session_tests.cpp for the
// end-to-end wiring (XexLoaded/EntryStarted/FirstGuestThread reached during
// a real load_game()/start()).

#include "xenon/core/boot_checkpoints.hpp"

#include <cassert>
#include <iostream>

using namespace xenon::core;

namespace {

void test_reach_is_idempotent_and_reports_first_time() {
  BootCheckpointTracker tracker;
  assert(!tracker.reached(BootCheckpoint::XexLoaded));
  assert(tracker.reach(BootCheckpoint::XexLoaded) &&
         "first reach() must return true");
  assert(tracker.reached(BootCheckpoint::XexLoaded));
  assert(!tracker.reach(BootCheckpoint::XexLoaded) &&
         "reaching the same checkpoint again must return false");
}

void test_reached_in_order_reflects_actual_order_not_declaration_order() {
  BootCheckpointTracker tracker;
  // Deliberately reached out of declaration order.
  assert(tracker.reach(BootCheckpoint::FirstGuestThread));
  assert(tracker.reach(BootCheckpoint::XexLoaded));
  assert(tracker.reach(BootCheckpoint::EntryStarted));

  const auto order = tracker.reached_in_order();
  assert(order.size() == 3);
  assert(order[0] == BootCheckpoint::FirstGuestThread);
  assert(order[1] == BootCheckpoint::XexLoaded);
  assert(order[2] == BootCheckpoint::EntryStarted);
}

void test_reset_clears_everything() {
  BootCheckpointTracker tracker;
  assert(tracker.reach(BootCheckpoint::XexLoaded));
  tracker.reset();
  assert(!tracker.reached(BootCheckpoint::XexLoaded));
  assert(tracker.reached_in_order().empty());
  // Must be reachable again after reset (a session re-initialized for a
  // second Play must not appear stuck at the previous run's progress).
  assert(tracker.reach(BootCheckpoint::XexLoaded));
}

void test_to_string_is_stable_for_every_checkpoint() {
  assert(to_string(BootCheckpoint::XexLoaded) == "XEX_LOADED");
  assert(to_string(BootCheckpoint::EntryStarted) == "ENTRY_STARTED");
  assert(to_string(BootCheckpoint::FirstGuestThread) == "FIRST_GUEST_THREAD");
  assert(to_string(BootCheckpoint::FirstFileOpen) == "FIRST_FILE_OPEN");
  assert(to_string(BootCheckpoint::FirstInputPoll) == "FIRST_INPUT_POLL");
  assert(to_string(BootCheckpoint::FirstAudioClient) == "FIRST_AUDIO_CLIENT");
  assert(to_string(BootCheckpoint::FirstGpuSubmission) == "FIRST_GPU_SUBMISSION");
  assert(to_string(BootCheckpoint::FirstShader) == "FIRST_SHADER");
  assert(to_string(BootCheckpoint::FirstResolve) == "FIRST_RESOLVE");
  assert(to_string(BootCheckpoint::FirstPresent) == "FIRST_PRESENT");
  assert(to_string(BootCheckpoint::FirstVblank) == "FIRST_VBLANK");
  assert(to_string(BootCheckpoint::ProfileReady) == "PROFILE_READY");
  assert(to_string(BootCheckpoint::SaveEnumeration) == "SAVE_ENUMERATION");
}

}  // namespace

int main() {
  std::cout << "Testing BootCheckpointTracker...\n";

  test_reach_is_idempotent_and_reports_first_time();
  test_reached_in_order_reflects_actual_order_not_declaration_order();
  test_reset_clears_everything();
  test_to_string_is_stable_for_every_checkpoint();

  std::cout << "All boot checkpoint tests passed!\n";
  return 0;
}

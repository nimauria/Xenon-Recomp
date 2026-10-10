// Bounded diagnostic event ring (xenon/logging/diagnostic_events.hpp).
//
// The ring is process-wide, so each test starts from clear().

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "xenon/logging/diagnostic_events.hpp"

namespace events = xenon::logging::events;
using events::Event;
using events::EventKind;

namespace {

constexpr const char* kSource = "diagnostic_events_tests";

Event make_event(std::uint32_t thread, std::uint32_t value) {
  Event event{};
  event.guest_thread_id = thread;
  event.kind = EventKind::Signal;
  // Derived from the other fields, so a record assembled from two different
  // events' words fails the check below.
  event.object_id = (std::uint64_t{thread} << 32) | value;
  event.guest_address = 0x80000000u ^ value;
  event.value = value;
  event.source = kSource;
  return event;
}

bool consistent(const Event& event) {
  return event.kind == EventKind::Signal && event.source == kSource &&
         event.object_id == ((std::uint64_t{event.guest_thread_id} << 32) | event.value) &&
         event.guest_address == (0x80000000u ^ event.value);
}

void test_disabled_recording_keeps_nothing() {
  events::set_enabled(false);
  events::clear();
  events::record(make_event(1, 1));
  assert(events::recorded() == 0);
  assert(events::snapshot().empty());
}

void test_events_round_trip_in_order() {
  events::set_enabled(true);
  events::clear();
  Event wait{};
  wait.guest_thread_id = 7;
  wait.kind = EventKind::WaitBegin;
  wait.object_id = 42;
  wait.guest_address = 0x82001234u;
  wait.value = 0xFFFFFFFFu;
  wait.source = "KeWaitForSingleObject";
  events::record(wait);
  wait.kind = EventKind::WaitEnd;
  wait.value = 0x102u;
  events::record(wait);

  const auto snapshot = events::snapshot();
  assert(snapshot.size() == 2 && events::recorded() == 2);
  assert(snapshot[0].sequence + 1 == snapshot[1].sequence);
  assert(snapshot[0].host_time_ns <= snapshot[1].host_time_ns);
  assert(snapshot[0].kind == EventKind::WaitBegin && snapshot[1].kind == EventKind::WaitEnd);
  assert(snapshot[0].guest_thread_id == 7 && snapshot[0].object_id == 42);
  assert(snapshot[0].guest_address == 0x82001234u && snapshot[0].value == 0xFFFFFFFFu);
  assert(snapshot[1].value == 0x102u);
  assert(std::strcmp(snapshot[0].source, "KeWaitForSingleObject") == 0);
}

void test_storage_is_bounded_and_keeps_the_newest() {
  events::set_enabled(true);
  events::clear();
  constexpr std::uint32_t kOverflow = 100;
  const auto total = static_cast<std::uint32_t>(events::kCapacity) + kOverflow;
  for (std::uint32_t i = 0; i < total; ++i) events::record(make_event(3, i));

  const auto snapshot = events::snapshot();
  assert(snapshot.size() == events::kCapacity);
  assert(events::recorded() == total && "overwritten events still count as recorded");
  assert(snapshot.front().value == kOverflow && snapshot.back().value == total - 1);
  for (std::size_t i = 1; i < snapshot.size(); ++i)
    assert(snapshot[i].sequence == snapshot[i - 1].sequence + 1);
}

void test_clear_forgets_earlier_events() {
  events::set_enabled(true);
  events::clear();
  events::record(make_event(4, 1));
  events::clear();
  assert(events::snapshot().empty() && events::recorded() == 0);
  events::record(make_event(4, 2));
  const auto snapshot = events::snapshot();
  assert(snapshot.size() == 1 && snapshot[0].value == 2);
}

// Writers race each other and a reader. Every snapshot must contain only
// whole events, and each writer's events must stay in that writer's order.
void test_concurrent_writers_never_produce_torn_events() {
  events::set_enabled(true);
  events::clear();
  constexpr std::uint32_t kWriters = 4;
  constexpr std::uint32_t kPerWriter = 400000;
  std::atomic<bool> writing{true};
  std::atomic<std::uint64_t> snapshots_checked{0};

  std::thread reader([&] {
    while (writing.load()) {
      for (const auto& event : events::snapshot()) assert(consistent(event));
      ++snapshots_checked;
    }
  });
  std::vector<std::thread> writers;
  for (std::uint32_t w = 0; w < kWriters; ++w) {
    writers.emplace_back([w] {
      for (std::uint32_t i = 0; i < kPerWriter; ++i) events::record(make_event(w + 1, i));
    });
  }
  for (auto& writer : writers) writer.join();
  writing = false;
  reader.join();

  assert(events::recorded() == std::uint64_t{kWriters} * kPerWriter);
  const auto snapshot = events::snapshot();
  assert(snapshot.size() == events::kCapacity);
  std::map<std::uint32_t, std::uint32_t> last_value;
  for (const auto& event : snapshot) {
    assert(consistent(event));
    const auto found = last_value.find(event.guest_thread_id);
    if (found != last_value.end()) assert(event.value > found->second);
    last_value[event.guest_thread_id] = event.value;
  }
  assert(snapshots_checked.load() > 0);
}

Event at(std::uint64_t sequence, std::uint32_t thread, EventKind kind, std::uint64_t object,
         std::uint32_t value, const char* source) {
  Event event{};
  event.sequence = sequence;
  event.host_time_ns = static_cast<std::int64_t>(sequence) * 1'000'000;  // 1 ms apart
  event.guest_thread_id = thread;
  event.kind = kind;
  event.object_id = object;
  event.guest_address = 0x82000000u + static_cast<std::uint32_t>(object);
  event.value = value;
  event.source = source;
  return event;
}

void test_blocked_waits_name_the_waiter_and_the_last_signaller() {
  const std::vector<Event> trace = {
      // t2 signals object 10; t3 waits on it and returns: that wait is closed.
      at(0, 2, EventKind::Signal, 10, 0, "KeSetEvent"),
      at(1, 3, EventKind::WaitBegin, 10, 0xFFFFFFFFu, "KeWaitForSingleObject"),
      at(2, 3, EventKind::WaitEnd, 10, 0, "KeWaitForSingleObject"),
      // t1 then waits on object 10 forever: still open, last signal before it.
      at(3, 1, EventKind::WaitBegin, 10, 0xFFFFFFFFu, "KeWaitForSingleObject"),
      // t4 waits on objects 20 and 21 together; 21 is signalled afterwards.
      at(4, 4, EventKind::WaitBegin, 20, 500, "KeWaitForMultipleObjects"),
      at(5, 4, EventKind::WaitBegin, 21, 500, "KeWaitForMultipleObjects"),
      at(6, 5, EventKind::Signal, 21, 0, "KeSetEvent"),
  };
  const auto waits = events::blocked_waits(trace);
  assert(waits.size() == 3);

  assert(waits[0].guest_thread_id == 1 && waits[0].object_id == 10);
  assert(waits[0].timeout_ms == 0xFFFFFFFFu && waits[0].waiting_since_ns == 3'000'000);
  assert(waits[0].last_signal && waits[0].last_signal->guest_thread_id == 2);
  assert(!waits[0].signalled_after_wait_began);

  assert(waits[1].guest_thread_id == 4 && waits[1].object_id == 20 && !waits[1].last_signal);
  assert(waits[2].guest_thread_id == 4 && waits[2].object_id == 21);
  assert(waits[2].last_signal && waits[2].last_signal->guest_thread_id == 5);
  assert(waits[2].signalled_after_wait_began && "a signal after the wait began is flagged");

  const auto text = events::format_blocked_waits(waits, 10'000'000);
  assert(text.find("t1 waits in KeWaitForSingleObject on object 10 (guest 0x8200000A) for 7 ms, "
                   "no timeout; last signalled by t2 in KeSetEvent 10 ms ago, before the wait "
                   "began") != std::string::npos);
  assert(text.find("t4 waits in KeWaitForMultipleObjects on object 20") != std::string::npos);
  assert(text.find("no signal of it recorded") != std::string::npos);
  assert(text.find("after the wait began") != std::string::npos);
  assert(text.find("t3 ") == std::string::npos && "a completed wait is not reported");
}

}  // namespace

int main() {
  std::cout << "Testing diagnostic event ring...\n";
  test_disabled_recording_keeps_nothing();
  test_events_round_trip_in_order();
  test_storage_is_bounded_and_keeps_the_newest();
  test_clear_forgets_earlier_events();
  test_concurrent_writers_never_produce_torn_events();
  test_blocked_waits_name_the_waiter_and_the_last_signaller();
  events::set_enabled(false);
  std::cout << "All diagnostic event ring tests passed!\n";
  return 0;
}

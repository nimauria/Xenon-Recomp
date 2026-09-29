#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <thread>
#include <vector>

#include "xenon/core/guest_memory_watch.hpp"

using xenon::core::GuestMemoryWatch;
using xenon::core::WatchObserver;
using xenon::core::WatchPhase;

namespace {

struct FakeMemory {
  std::map<std::uint32_t, std::uint32_t> words{};
  GuestMemoryWatch::Reader reader() {
    return [this](std::uint32_t address) -> std::optional<std::uint32_t> {
      const auto it = words.find(address);
      if (it == words.end()) return std::nullopt;
      return it->second;
    };
  }
};

WatchObserver observer(WatchPhase phase, std::uint32_t thread, std::uint32_t cia = 0u) {
  WatchObserver result{};
  result.phase = phase;
  result.thread_id = thread;
  result.cia = cia;
  result.nia = cia + 4u;
  result.lr = 0x82000000u;
  return result;
}

void test_inactive_watch_is_free_and_silent() {
  GuestMemoryWatch watch;
  FakeMemory memory;
  memory.words[0x829DDEBCu] = 1u;
  assert(!watch.active());
  assert(watch.sample(memory.reader(), observer(WatchPhase::Poll, 0u)).empty());
  assert(watch.history().empty());
}

void test_initial_value_then_changes_only() {
  GuestMemoryWatch watch({0x829DDEBCu}, 16u);
  assert(watch.active());
  FakeMemory memory;
  memory.words[0x829DDEBCu] = 0u;

  auto added = watch.sample(memory.reader(), observer(WatchPhase::Poll, 0u));
  assert(added.size() == 1u);
  assert(added[0].initial);
  assert(*added[0].new_value == 0u);
  assert(watch.total_changes() == 0u);

  // Unchanged: nothing recorded.
  assert(watch.sample(memory.reader(), observer(WatchPhase::DispatchBoundary, 3u)).empty());

  memory.words[0x829DDEBCu] = 1u;
  added = watch.sample(memory.reader(), observer(WatchPhase::AfterKernelCall, 12u, 0x821CD7F0u));
  assert(added.size() == 1u);
  assert(!added[0].initial);
  assert(*added[0].old_value == 0u && *added[0].new_value == 1u);
  assert(added[0].observer.thread_id == 12u);
  assert(added[0].observer.phase == WatchPhase::AfterKernelCall);
  assert(added[0].observer.cia == 0x821CD7F0u);
  assert(watch.total_changes() == 1u);
  assert(watch.last_value(0x829DDEBCu) == std::optional<std::uint32_t>(1u));

  const auto text = GuestMemoryWatch::format(added[0]);
  assert(text.find("0x829DDEBC") != std::string::npos);
  assert(text.find("0x00000000 -> 0x00000001") != std::string::npos);
  assert(text.find("t12") != std::string::npos);
  assert(text.find("after-kernel-call") != std::string::npos);
}

void test_unmapped_has_no_baseline_and_lost_mapping_is_a_change() {
  GuestMemoryWatch watch({0x1000u}, 8u);
  FakeMemory memory;
  assert(watch.sample(memory.reader(), observer(WatchPhase::Poll, 0u)).empty());

  memory.words[0x1000u] = 7u;
  auto added = watch.sample(memory.reader(), observer(WatchPhase::Poll, 0u));
  assert(added.size() == 1u && added[0].initial);

  memory.words.clear();
  added = watch.sample(memory.reader(), observer(WatchPhase::Poll, 0u));
  assert(added.size() == 1u);
  assert(!added[0].new_value.has_value());
  assert(*added[0].old_value == 7u);
}

void test_ring_keeps_newest_and_counts_all() {
  GuestMemoryWatch watch({0x2000u}, 3u);
  FakeMemory memory;
  for (std::uint32_t value = 0; value < 8u; ++value) {
    memory.words[0x2000u] = value;
    (void)watch.sample(memory.reader(), observer(WatchPhase::Poll, 0u));
  }
  // 8 observations = 1 initial + 7 changes; only the newest 3 are retained.
  assert(watch.total_changes() == 7u);
  const auto history = watch.history();
  assert(history.size() == 3u);
  assert(*history[0].new_value == 5u);
  assert(*history[1].new_value == 6u);
  assert(*history[2].new_value == 7u);
  assert(history[0].sequence < history[1].sequence && history[1].sequence < history[2].sequence);
}

void test_multiple_addresses_dedupe_and_reconfigure() {
  GuestMemoryWatch watch({0x3004u, 0x3000u, 0x3004u}, 16u);
  assert((watch.addresses() == std::vector<std::uint32_t>{0x3000u, 0x3004u}));
  FakeMemory memory;
  memory.words[0x3000u] = 1u;
  memory.words[0x3004u] = 2u;
  assert(watch.sample(memory.reader(), observer(WatchPhase::Poll, 0u)).size() == 2u);
  memory.words[0x3004u] = 9u;
  const auto added = watch.sample(memory.reader(), observer(WatchPhase::Poll, 0u));
  assert(added.size() == 1u && added[0].address == 0x3004u);

  watch.configure({}, 4u);
  assert(!watch.active());
  assert(watch.history().empty());
  assert(watch.total_changes() == 0u);
}

// The store happens on one thread and is observed by a different sampler: the
// record must carry the sampler's identity, exactly once.
void test_concurrent_writer_observed_once() {
  GuestMemoryWatch watch({0x4000u}, 64u);
  std::atomic<std::uint32_t> word{0u};
  const GuestMemoryWatch::Reader reader = [&](std::uint32_t) -> std::optional<std::uint32_t> {
    return word.load();
  };
  (void)watch.sample(reader, observer(WatchPhase::Poll, 0u));

  std::thread writer([&] { word.store(0xCAFEu); });
  writer.join();

  std::size_t seen = 0u;
  std::thread a([&] { seen += watch.sample(reader, observer(WatchPhase::Poll, 0u)).size(); });
  a.join();
  seen += watch.sample(reader, observer(WatchPhase::DispatchBoundary, 5u)).size();
  assert(seen == 1u);
  assert(watch.total_changes() == 1u);
}

}  // namespace

int main() {
  test_inactive_watch_is_free_and_silent();
  test_initial_value_then_changes_only();
  test_unmapped_has_no_baseline_and_lost_mapping_is_a_change();
  test_ring_keeps_newest_and_counts_all();
  test_multiple_addresses_dedupe_and_reconfigure();
  test_concurrent_writer_observed_once();
  std::cout << "guest memory watch tests passed" << std::endl;
  return 0;
}

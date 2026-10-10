#include "xenon/logging/diagnostic_events.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string_view>
#include <thread>

namespace xenon::logging::events {
namespace {

static_assert((kCapacity & (kCapacity - 1)) == 0, "kCapacity must be a power of two");

// One ring slot. Every field is atomic, so concurrent readers and writers
// never race. `version` is 2 * sequence + 1 while the event with that
// sequence is being written and 2 * sequence + 2 once it is complete, so a
// reader accepts a slot only when it holds exactly the event it asked for.
struct Slot {
  std::atomic<std::uint64_t> version{0};
  std::array<std::atomic<std::uint64_t>, 5> words{};
};

bool enabled_from_environment() {
  const char* value = std::getenv("XENON_DIAG_EVENTS");
  return value != nullptr && *value != '\0' && std::string_view(value) != "0";
}

std::atomic<bool> g_enabled{enabled_from_environment()};
std::atomic<std::uint64_t> g_next_sequence{0};
std::atomic<std::uint64_t> g_base_sequence{0};
std::array<Slot, kCapacity> g_ring{};

std::uint64_t pack(std::uint32_t low, std::uint32_t high) {
  return std::uint64_t{low} | (std::uint64_t{high} << 32);
}

}  // namespace

bool enabled() noexcept { return g_enabled.load(std::memory_order_relaxed); }

void set_enabled(bool enabled) noexcept { g_enabled.store(enabled, std::memory_order_relaxed); }

void record(Event event) noexcept {
  if (!enabled()) return;
  const auto sequence = g_next_sequence.fetch_add(1, std::memory_order_relaxed);
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto host_time_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();

  auto& slot = g_ring[sequence & (kCapacity - 1)];
  const std::uint64_t writing = 2 * sequence + 1;
  auto current = slot.version.load(std::memory_order_relaxed);
  while (true) {
    // A writer kCapacity events ahead already owns this slot: this event is
    // overwritten before it was ever visible.
    if (current >= writing) return;
    // The event this slot held one lap earlier is still being written.
    if ((current & 1u) != 0) {
      std::this_thread::yield();
      current = slot.version.load(std::memory_order_relaxed);
      continue;
    }
    if (slot.version.compare_exchange_weak(current, writing, std::memory_order_acquire,
                                           std::memory_order_relaxed))
      break;
  }
  // Release stores: a reader that sees any of these words also sees the odd
  // version written above, and so rejects the slot until it is complete.
  slot.words[0].store(static_cast<std::uint64_t>(host_time_ns), std::memory_order_release);
  slot.words[1].store(pack(event.guest_thread_id, static_cast<std::uint32_t>(event.kind)),
                      std::memory_order_release);
  slot.words[2].store(event.object_id, std::memory_order_release);
  slot.words[3].store(pack(event.guest_address, event.value), std::memory_order_release);
  slot.words[4].store(reinterpret_cast<std::uintptr_t>(event.source), std::memory_order_release);
  slot.version.store(writing + 1, std::memory_order_release);
}

std::vector<Event> snapshot() {
  const auto end = g_next_sequence.load(std::memory_order_acquire);
  auto begin = g_base_sequence.load(std::memory_order_acquire);
  if (end > kCapacity && end - kCapacity > begin) begin = end - kCapacity;

  std::vector<Event> events;
  events.reserve(static_cast<std::size_t>(end > begin ? end - begin : 0));
  for (auto sequence = begin; sequence < end; ++sequence) {
    const auto& slot = g_ring[sequence & (kCapacity - 1)];
    const std::uint64_t complete = 2 * sequence + 2;
    if (slot.version.load(std::memory_order_acquire) != complete) continue;
    std::array<std::uint64_t, 5> words{};
    // Acquire loads keep the version re-check below after every word read.
    for (std::size_t i = 0; i < words.size(); ++i)
      words[i] = slot.words[i].load(std::memory_order_acquire);
    if (slot.version.load(std::memory_order_relaxed) != complete) continue;

    Event event{};
    event.sequence = sequence;
    event.host_time_ns = static_cast<std::int64_t>(words[0]);
    event.guest_thread_id = static_cast<std::uint32_t>(words[1]);
    event.kind = static_cast<EventKind>(static_cast<std::uint16_t>(words[1] >> 32));
    event.object_id = words[2];
    event.guest_address = static_cast<std::uint32_t>(words[3]);
    event.value = static_cast<std::uint32_t>(words[3] >> 32);
    event.source = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(words[4]));
    events.push_back(event);
  }
  return events;
}

std::uint64_t recorded() noexcept {
  // Base first: clear() only ever moves it up to a value the end has
  // already reached, so end read afterwards is never below it.
  const auto base = g_base_sequence.load(std::memory_order_acquire);
  return g_next_sequence.load(std::memory_order_acquire) - base;
}

void clear() noexcept {
  g_base_sequence.store(g_next_sequence.load(std::memory_order_acquire),
                        std::memory_order_release);
}

}  // namespace xenon::logging::events

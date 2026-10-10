#include "xenon/logging/diagnostic_events.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
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

std::vector<BlockedWait> blocked_waits(std::span<const Event> events) {
  std::map<std::uint32_t, std::vector<const Event*>> open_waits;
  std::map<std::uint32_t, EventKind> last_kind;
  std::map<std::uint64_t, const Event*> last_signal;
  for (const auto& event : events) {
    switch (event.kind) {
      case EventKind::WaitBegin: {
        // A WaitBegin that does not continue a multi-object wait's run of
        // WaitBegins starts a new wait.
        const auto previous = last_kind.find(event.guest_thread_id);
        auto& open = open_waits[event.guest_thread_id];
        if (previous == last_kind.end() || previous->second != EventKind::WaitBegin) open.clear();
        open.push_back(&event);
        break;
      }
      case EventKind::WaitEnd:
        open_waits[event.guest_thread_id].clear();
        break;
      case EventKind::Signal:
        if (event.object_id != 0) last_signal[event.object_id] = &event;
        break;
    }
    last_kind[event.guest_thread_id] = event.kind;
  }

  std::vector<BlockedWait> waits;
  for (const auto& [thread, open] : open_waits) {
    for (const auto* begin : open) {
      BlockedWait wait{};
      wait.guest_thread_id = thread;
      wait.object_id = begin->object_id;
      wait.guest_address = begin->guest_address;
      wait.source = begin->source;
      wait.waiting_since_ns = begin->host_time_ns;
      wait.timeout_ms = begin->value;
      if (const auto found = last_signal.find(begin->object_id); found != last_signal.end()) {
        wait.last_signal = *found->second;
        wait.signalled_after_wait_began = found->second->sequence > begin->sequence;
      }
      waits.push_back(wait);
    }
  }
  return waits;
}

std::string format_blocked_waits(std::span<const BlockedWait> waits, std::int64_t now_ns) {
  std::string text;
  char line[320];
  for (const auto& wait : waits) {
    const auto waited_ms = (now_ns - wait.waiting_since_ns) / 1'000'000;
    std::snprintf(line, sizeof(line), "t%u waits in %s on object %llu (guest 0x%08X) for %lld ms",
                  wait.guest_thread_id, wait.source ? wait.source : "?",
                  static_cast<unsigned long long>(wait.object_id), wait.guest_address,
                  static_cast<long long>(waited_ms));
    text += line;
    if (wait.timeout_ms == 0xFFFFFFFFu) {
      text += ", no timeout";
    } else {
      std::snprintf(line, sizeof(line), ", timeout %u ms", wait.timeout_ms);
      text += line;
    }
    if (!wait.last_signal) {
      text += "; no signal of it recorded";
    } else {
      std::snprintf(line, sizeof(line), "; last signalled by t%u in %s %lld ms %s",
                    wait.last_signal->guest_thread_id,
                    wait.last_signal->source ? wait.last_signal->source : "?",
                    static_cast<long long>(
                        (now_ns - wait.last_signal->host_time_ns) / 1'000'000),
                    wait.signalled_after_wait_began ? "ago, after the wait began"
                                                    : "ago, before the wait began");
      text += line;
    }
    text += '\n';
  }
  return text;
}

}  // namespace xenon::logging::events

#include "xenon/kernel/wait_profile.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>

#include "xenon/kernel/object.hpp"
#include "xenon/kernel/wait.hpp"

namespace xenon::kernel {
namespace {

std::atomic<bool> g_enabled{false};

struct Key {
  std::uint32_t thread_id;
  std::uint64_t object_id;
  std::uint32_t signaler_thread_id;
  std::uint32_t signaler_caller;
  friend bool operator<(const Key& a, const Key& b) {
    return std::tie(a.thread_id, a.object_id, a.signaler_thread_id, a.signaler_caller) <
           std::tie(b.thread_id, b.object_id, b.signaler_thread_id, b.signaler_caller);
  }
};

std::mutex g_mutex;
std::map<Key, WaitProfileEntry> g_entries;

struct SpinCounters {
  std::atomic<std::uint64_t> registered{0};
  std::atomic<std::uint64_t> spins{0};
  std::atomic<std::uint64_t> spin_hits{0};
  std::atomic<std::uint64_t> blocks{0};
  std::atomic<std::uint64_t> spin_ns{0};
};
SpinCounters g_spin;

const char* type_name(ObjectType type) {
  switch (type) {
    case ObjectType::Event: return "event";
    case ObjectType::Thread: return "thread";
    case ObjectType::Semaphore: return "semaphore";
    case ObjectType::Mutant: return "mutant";
    case ObjectType::Timer: return "timer";
    default: return "object";
  }
}

double ms(std::uint64_t ns) { return static_cast<double>(ns) / 1e6; }

}  // namespace

bool wait_profile_enabled() noexcept { return g_enabled.load(std::memory_order_relaxed); }

void set_wait_profile_enabled(bool enabled) noexcept {
  g_enabled.store(enabled, std::memory_order_relaxed);
}

void reset_wait_profile() {
  {
    std::scoped_lock lock(g_mutex);
    g_entries.clear();
  }
  g_spin.registered.store(0, std::memory_order_relaxed);
  g_spin.spins.store(0, std::memory_order_relaxed);
  g_spin.spin_hits.store(0, std::memory_order_relaxed);
  g_spin.blocks.store(0, std::memory_order_relaxed);
  g_spin.spin_ns.store(0, std::memory_order_relaxed);
}

WaitSpinCounters wait_spin_counters() noexcept {
  WaitSpinCounters counters;
  counters.registered = g_spin.registered.load(std::memory_order_relaxed);
  counters.spins = g_spin.spins.load(std::memory_order_relaxed);
  counters.spin_hits = g_spin.spin_hits.load(std::memory_order_relaxed);
  counters.blocks = g_spin.blocks.load(std::memory_order_relaxed);
  counters.spin_ns = g_spin.spin_ns.load(std::memory_order_relaxed);
  return counters;
}

void note_registered_wait(bool spun, bool spin_hit, bool blocked, std::uint64_t spin_ns) noexcept {
  g_spin.registered.fetch_add(1, std::memory_order_relaxed);
  if (spun) {
    g_spin.spins.fetch_add(1, std::memory_order_relaxed);
    g_spin.spin_ns.fetch_add(spin_ns, std::memory_order_relaxed);
  }
  if (spin_hit) g_spin.spin_hits.fetch_add(1, std::memory_order_relaxed);
  if (blocked) g_spin.blocks.fetch_add(1, std::memory_order_relaxed);
}

void note_blocking_wait(std::uint32_t thread_id, const KernelObject& object, WaitResult result,
                        std::uint64_t blocked_ns, bool spin_hit) {
  if (!wait_profile_enabled()) return;
  const auto signal = object.last_signal();
  const Key key{thread_id, object.object_id(), signal.guest_thread_id, signal.guest_caller};
  const bool took = result == WaitResult::Success || result == WaitResult::Abandoned;
  std::scoped_lock lock(g_mutex);
  auto& entry = g_entries[key];
  if (!entry.waits) {
    entry.thread_id = thread_id;
    entry.object_id = object.object_id();
    entry.object_type = object.type();
    entry.signaler_thread_id = signal.guest_thread_id;
    entry.signaler_caller = signal.guest_caller;
  }
  entry.signal_source = signal.source;
  ++entry.waits;
  if (!took) ++entry.timeouts;
  if (spin_hit) ++entry.spin_hits;
  entry.total_ns += blocked_ns;
  entry.max_ns = (std::max)(entry.max_ns, blocked_ns);
}

std::vector<WaitProfileEntry> wait_profile_entries() {
  std::vector<WaitProfileEntry> entries;
  {
    std::scoped_lock lock(g_mutex);
    entries.reserve(g_entries.size());
    for (const auto& [key, entry] : g_entries) entries.push_back(entry);
  }
  std::sort(entries.begin(), entries.end(),
            [](const auto& a, const auto& b) { return a.total_ns > b.total_ns; });
  return entries;
}

std::string wait_profile_report(std::size_t top_per_thread) {
  const auto entries = wait_profile_entries();
  std::map<std::uint32_t, std::uint64_t> per_thread;
  for (const auto& entry : entries) per_thread[entry.thread_id] += entry.total_ns;
  std::vector<std::pair<std::uint32_t, std::uint64_t>> threads(per_thread.begin(),
                                                                per_thread.end());
  std::sort(threads.begin(), threads.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  std::string out =
      "wait profile: blocking guest waits by waiting thread, object and the signal that ended "
      "them\n  (released by = the object's last signal when the wait ended: guest thread, "
      "caller, export; timeouts = waits that ended without the object; spin hits = waits "
      "granted while spinning, before they blocked)\n";
  char line[320];
  const auto spin = wait_spin_counters();
  std::snprintf(line, sizeof(line),
                "\nspin: %llu registered waits, %llu spun, %llu spin hits, %llu blocked, "
                "%.3f ms spinning\n",
                static_cast<unsigned long long>(spin.registered),
                static_cast<unsigned long long>(spin.spins),
                static_cast<unsigned long long>(spin.spin_hits),
                static_cast<unsigned long long>(spin.blocks), ms(spin.spin_ns));
  out += line;
  for (const auto& [thread, total] : threads) {
    std::snprintf(line, sizeof(line), "\nthread %u: %.1f ms blocked\n", thread, ms(total));
    out += line;
    out += "      total ms     waits  timeouts spinhits   avg ms   max ms  object            "
           "released by\n";
    std::size_t shown = 0;
    for (const auto& entry : entries) {
      if (entry.thread_id != thread) continue;
      if (shown++ == top_per_thread) break;
      char released[96];
      if (entry.signaler_thread_id || entry.signaler_caller)
        std::snprintf(released, sizeof(released), "T%u from 0x%08X via %s",
                      entry.signaler_thread_id, entry.signaler_caller,
                      entry.signal_source ? entry.signal_source : "?");
      else
        std::snprintf(released, sizeof(released), "(never signalled by the guest)");
      std::snprintf(line, sizeof(line),
                    "  %12.1f %9llu %9llu %8llu %8.2f %8.2f  %-9s #%-6llu %s\n",
                    ms(entry.total_ns), static_cast<unsigned long long>(entry.waits),
                    static_cast<unsigned long long>(entry.timeouts),
                    static_cast<unsigned long long>(entry.spin_hits),
                    ms(entry.total_ns) / static_cast<double>(entry.waits), ms(entry.max_ns),
                    type_name(entry.object_type),
                    static_cast<unsigned long long>(entry.object_id), released);
      out += line;
    }
  }
  return out;
}

}  // namespace xenon::kernel

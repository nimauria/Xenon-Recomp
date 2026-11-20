#include "xenon/core/in_flight_calls.hpp"

#include <thread>

namespace xenon::core {
namespace {

std::uint64_t next_instance_id() noexcept {
  static std::atomic<std::uint64_t> counter{0};
  return counter.fetch_add(1u, std::memory_order_relaxed) + 1u;
}

}  // namespace

// Written only by its owner thread. `seq` is odd while the owner rewrites the
// slot, so a reader that sees it odd, or sees it change across its copy, has
// a torn copy and discards it.
struct InFlightCalls::Slot {
  explicit Slot(std::thread::id writer) : owner(writer) {}

  const std::thread::id owner;
  std::atomic<std::uint64_t> seq{0};
  bool active{};
  Call call{};
};

InFlightCalls::InFlightCalls() : instance_id_(next_instance_id()) {}

InFlightCalls::~InFlightCalls() = default;

InFlightCalls::Slot& InFlightCalls::slot_for_this_thread() {
  // Instance ids are never reused, so an entry for a destroyed table is never
  // matched again.
  struct CacheEntry {
    std::uint64_t instance{};
    Slot* slot{};
  };
  thread_local std::array<CacheEntry, 4> cache{};
  thread_local std::size_t next_victim = 0;

  for (const auto& entry : cache) {
    if (entry.instance == instance_id_) return *entry.slot;
  }

  const auto self = std::this_thread::get_id();
  Slot* slot = nullptr;
  {
    std::scoped_lock lock(slots_mutex_);
    // A host thread id can be reused once its thread has exited; the slot
    // still has exactly one live writer.
    for (const auto& candidate : slots_) {
      if (candidate->owner == self) {
        slot = candidate.get();
        break;
      }
    }
    if (slot == nullptr) {
      slots_.push_back(std::make_unique<Slot>(self));
      slot = slots_.back().get();
    }
  }
  cache[next_victim] = {instance_id_, slot};
  next_victim = (next_victim + 1u) % cache.size();
  return *slot;
}

void InFlightCalls::publish(Slot& slot, const std::optional<Call>& call) noexcept {
  const auto seq = slot.seq.load(std::memory_order_relaxed);
  slot.seq.store(seq + 1u, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  slot.active = call.has_value();
  if (call) slot.call = *call;
  slot.seq.store(seq + 2u, std::memory_order_release);
}

std::optional<InFlightCalls::Call> InFlightCalls::enter(const Call& call) {
  auto& slot = slot_for_this_thread();
  // Only this thread writes the slot, so it reads its own fields directly.
  std::optional<Call> outer;
  if (slot.active) outer = slot.call;
  publish(slot, call);
  return outer;
}

void InFlightCalls::leave(const std::optional<Call>& outer) {
  publish(slot_for_this_thread(), outer);
}

std::map<std::uint32_t, InFlightCalls::Call> InFlightCalls::snapshot() const {
  std::map<std::uint32_t, Call> result;
  std::scoped_lock lock(slots_mutex_);
  for (const auto& slot : slots_) {
    // A slot being rewritten settles in a few instructions; one that keeps
    // changing is a thread making call after call, and is skipped.
    for (int attempt = 0; attempt < 8; ++attempt) {
      const auto before = slot->seq.load(std::memory_order_acquire);
      if (before & 1u) continue;
      const bool active = slot->active;
      const Call call = slot->call;
      std::atomic_thread_fence(std::memory_order_acquire);
      if (slot->seq.load(std::memory_order_relaxed) != before) continue;
      if (active) {
        auto [it, inserted] = result.try_emplace(call.thread_id, call);
        if (!inserted && call.since > it->second.since) it->second = call;
      }
      break;
    }
  }
  return result;
}

InFlightCalls::Scope::Scope(InFlightCalls& calls, const Call& call)
    : calls_(&calls), outer_(calls.enter(call)) {}

InFlightCalls::Scope::~Scope() { calls_->leave(outer_); }

}  // namespace xenon::core

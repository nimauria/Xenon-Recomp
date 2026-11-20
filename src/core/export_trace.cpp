#include "xenon/core/export_trace.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <tuple>

namespace xenon::core {
namespace {

template <std::size_t N>
std::string_view array_string_view(const std::array<char, N>& value) noexcept {
  const auto end = std::find(value.begin(), value.end(), '\0');
  return {value.data(), static_cast<std::size_t>(end - value.begin())};
}

std::uint64_t next_instance_id() noexcept {
  static std::atomic<std::uint64_t> counter{0};
  return counter.fetch_add(1u, std::memory_order_relaxed) + 1u;
}

// Steady-clock nanoseconds, the unit callers time their calls in.
std::int64_t now_ticks() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

std::string_view ExportTraceRecord::library_view() const noexcept {
  return array_string_view(library);
}

std::string_view ExportTraceRecord::name_view() const noexcept {
  return array_string_view(name);
}

// One writer's ring. Record number n (0-based, counted by `written`) lives in
// slot n % capacity. A slot's `seq` is odd while it is being written and
// 2 * (n + 1) once record n is complete, so a reader can tell a finished
// record from one being overwritten or one already replaced by a later lap.
struct ExportTrace::Lane {
  struct Slot {
    std::atomic<std::uint64_t> seq{0};
    std::int64_t timestamp{};
    ExportTraceRecord record{};
  };

  Lane(std::size_t slot_count, std::thread::id writer)
      : slots(std::make_unique<Slot[]>(slot_count)),
        capacity(slot_count),
        owner(writer) {}

  std::unique_ptr<Slot[]> slots;
  const std::size_t capacity;
  const std::thread::id owner;
  std::atomic<std::uint64_t> written{0};
  // Records numbered below this were cleared.
  std::atomic<std::uint64_t> base{0};
};

ExportTrace::ExportTrace(std::size_t capacity)
    : instance_id_(next_instance_id()),
      capacity_(std::max<std::size_t>(capacity, 1u)),
      lane_capacity_(std::min(capacity_, kMaxLaneCapacity)) {}

ExportTrace::~ExportTrace() = default;

void ExportTrace::set_enabled(bool enabled) noexcept {
  enabled_.store(enabled, std::memory_order_release);
}

bool ExportTrace::enabled() const noexcept {
  return enabled_.load(std::memory_order_acquire);
}

std::size_t ExportTrace::capacity() const noexcept { return capacity_; }

template <std::size_t N>
void ExportTrace::copy_text(std::array<char, N>& destination,
                            std::string_view source) noexcept {
  destination.fill('\0');
  const auto count = std::min(source.size(), N - 1u);
  std::copy_n(source.data(), count, destination.data());
}

ExportTrace::Lane& ExportTrace::lane_for_this_thread() {
  // A few traces can be live at once (tests run several sessions); each
  // thread remembers its lane in the most recent ones. Instance ids are never
  // reused, so an entry for a destroyed trace is never matched again.
  struct CacheEntry {
    std::uint64_t instance{};
    Lane* lane{};
  };
  thread_local std::array<CacheEntry, 4> cache{};
  thread_local std::size_t next_victim = 0;

  for (const auto& entry : cache) {
    if (entry.instance == instance_id_) return *entry.lane;
  }

  const auto self = std::this_thread::get_id();
  Lane* lane = nullptr;
  {
    std::scoped_lock lock(lanes_mutex_);
    for (const auto& candidate : lanes_) {
      if (candidate->owner == self) {
        lane = candidate.get();
        break;
      }
    }
    if (lane == nullptr) {
      if (lanes_.size() >= kMaxLanes) {
        if (!shared_lane_) {
          shared_lane_ = std::make_unique<Lane>(lane_capacity_, std::thread::id{});
        }
        return *shared_lane_;  // not cached: every write takes the lock
      }
      lanes_.push_back(std::make_unique<Lane>(lane_capacity_, self));
      lane = lanes_.back().get();
    }
  }
  cache[next_victim] = {instance_id_, lane};
  next_victim = (next_victim + 1u) % cache.size();
  return *lane;
}

void ExportTrace::record(std::uint32_t thread_id, std::string_view library,
                         std::string_view name, std::uint32_t ordinal,
                         cpu::GuestAddress call_address, std::uint64_t lr,
                         std::uint64_t ctr,
                         std::span<const std::uint64_t, 8> arguments,
                         std::uint64_t result_r3, bool handler_found,
                         bool handled, bool success, std::uint64_t start_ns,
                         std::uint64_t duration_ns) {
  if (!enabled()) return;

  auto& lane = lane_for_this_thread();
  const auto write = [&] {
    const auto number = lane.written.load(std::memory_order_relaxed);
    auto& slot = lane.slots[number % lane.capacity];
    slot.seq.store(2u * number + 1u, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    // A timed call ends at start + duration; only an untimed one reads the
    // clock here.
    slot.timestamp = duration_ns != 0u || start_ns != 0u
                         ? static_cast<std::int64_t>(start_ns + duration_ns)
                         : now_ticks();
    auto& record = slot.record;
    record.sequence = 0u;  // assigned when a snapshot orders the lanes
    record.thread_id = thread_id;
    copy_text(record.library, library);
    copy_text(record.name, name);
    record.ordinal = ordinal;
    record.call_address = call_address;
    record.lr = lr;
    record.ctr = ctr;
    std::copy(arguments.begin(), arguments.end(), record.arguments.begin());
    record.result_r3 = result_r3;
    record.handler_found = handler_found;
    record.handled = handled;
    record.success = success;
    record.start_ns = start_ns;
    record.duration_ns = duration_ns;

    slot.seq.store(2u * (number + 1u), std::memory_order_release);
    lane.written.store(number + 1u, std::memory_order_release);
  };

  if (&lane == shared_lane_.get()) {
    std::scoped_lock lock(lanes_mutex_);
    write();
  } else {
    write();
  }
}

std::vector<ExportTraceRecord> ExportTrace::snapshot() const {
  struct Entry {
    std::int64_t timestamp;
    std::size_t lane;
    std::uint64_t number;
    ExportTraceRecord record;
  };
  std::vector<Entry> entries;

  std::scoped_lock lock(lanes_mutex_);
  const auto collect = [&](const Lane& lane, std::size_t lane_index) {
    const auto written = lane.written.load(std::memory_order_acquire);
    const auto oldest_kept =
        written > lane.capacity ? written - lane.capacity : std::uint64_t{0};
    const auto first = std::max(oldest_kept, lane.base.load(std::memory_order_acquire));
    for (auto number = first; number < written; ++number) {
      const auto& slot = lane.slots[number % lane.capacity];
      const auto expected = 2u * (number + 1u);
      if (slot.seq.load(std::memory_order_acquire) != expected) continue;
      Entry entry{slot.timestamp, lane_index, number, slot.record};
      std::atomic_thread_fence(std::memory_order_acquire);
      // Overwritten while it was copied: the writer has lapped this slot.
      if (slot.seq.load(std::memory_order_relaxed) != expected) continue;
      entries.push_back(entry);
    }
  };
  for (std::size_t i = 0; i < lanes_.size(); ++i) collect(*lanes_[i], i);
  if (shared_lane_) collect(*shared_lane_, lanes_.size());

  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    return std::tie(a.timestamp, a.lane, a.number) <
           std::tie(b.timestamp, b.lane, b.number);
  });
  const auto keep = std::min(entries.size(), capacity_);
  const auto skip = entries.size() - keep;

  std::vector<ExportTraceRecord> result;
  result.reserve(keep);
  for (std::size_t i = skip; i < entries.size(); ++i) {
    result.push_back(entries[i].record);
    result.back().sequence = result.size();
  }
  return result;
}

std::vector<ExportTraceRecord> ExportTrace::recent_global(
    std::size_t limit) const {
  auto result = snapshot();
  if (result.size() > limit) {
    result.erase(result.begin(), result.end() - static_cast<std::ptrdiff_t>(limit));
  }
  return result;
}

std::vector<ExportTraceRecord> ExportTrace::recent_for_thread(
    std::uint32_t thread_id, std::size_t limit) const {
  const auto all = snapshot();
  std::vector<ExportTraceRecord> result;
  result.reserve(std::min(limit, all.size()));
  for (auto it = all.rbegin(); it != all.rend() && result.size() < limit; ++it) {
    if (it->thread_id == thread_id) result.push_back(*it);
  }
  std::reverse(result.begin(), result.end());
  return result;
}

void ExportTrace::clear() {
  std::scoped_lock lock(lanes_mutex_);
  for (const auto& lane : lanes_) {
    lane->base.store(lane->written.load(std::memory_order_acquire),
                     std::memory_order_release);
  }
  if (shared_lane_) {
    shared_lane_->base.store(shared_lane_->written.load(std::memory_order_acquire),
                             std::memory_order_release);
  }
}

}  // namespace xenon::core

#include "xenon/core/guest_memory_watch.hpp"

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <sstream>

namespace xenon::core {

std::string_view watch_phase_name(WatchPhase phase) noexcept {
  switch (phase) {
    case WatchPhase::Poll: return "poll";
    case WatchPhase::BeforeKernelCall: return "before-kernel-call";
    case WatchPhase::AfterKernelCall: return "after-kernel-call";
    case WatchPhase::DispatchBoundary: return "dispatch-boundary";
  }
  return "unknown";
}

namespace {

std::optional<std::uint32_t> parse_word(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  int base = 10;
  if (text.size() > 2u && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    base = 16;
    text.remove_prefix(2);
  }
  if (text.empty()) return std::nullopt;
  std::uint32_t value{};
  const auto* end = text.data() + text.size();
  const auto [ptr, error] = std::from_chars(text.data(), end, value, base);
  if (error != std::errc{} || ptr != end) return std::nullopt;
  return value;
}

void append_hex(std::ostringstream& out, std::uint32_t value) {
  out << "0x" << std::hex << std::uppercase << value << std::dec;
}

// The word a target names: the plain word, or the end of its pointer chain.
std::optional<std::uint32_t> resolve(const WatchTarget& target,
                                     const GuestMemoryWatch::Reader& reader) {
  auto value = reader(target.address);
  for (const auto offset : target.offsets) {
    if (!value || *value == 0u) return std::nullopt;
    value = reader(*value + offset);
  }
  return value;
}

}  // namespace

std::optional<WatchTarget> parse_watch_target(std::string_view text) {
  WatchTarget target;
  bool first = true;
  while (true) {
    const auto arrow = text.find("->");
    const auto part = parse_word(text.substr(0, arrow));
    if (!part) return std::nullopt;
    if (first) {
      target.address = *part;
      first = false;
    } else {
      target.offsets.push_back(*part);
    }
    if (arrow == std::string_view::npos) return target;
    text.remove_prefix(arrow + 2u);
  }
}

std::string format_watch_target(const WatchTarget& target) {
  std::ostringstream out;
  append_hex(out, target.address);
  for (const auto offset : target.offsets) {
    out << "->";
    append_hex(out, offset);
  }
  return out.str();
}

GuestMemoryWatch::GuestMemoryWatch(std::vector<WatchTarget> targets, std::size_t capacity) {
  configure(std::move(targets), capacity);
}

void GuestMemoryWatch::configure(std::vector<WatchTarget> targets, std::size_t capacity) {
  std::sort(targets.begin(), targets.end());
  targets.erase(std::unique(targets.begin(), targets.end()), targets.end());

  std::scoped_lock lock(mutex_);
  slots_.clear();
  slots_.reserve(targets.size());
  for (auto& target : targets) slots_.push_back(Slot{std::move(target), false, std::nullopt});
  capacity_ = std::max<std::size_t>(capacity, 1u);
  ring_.clear();
  ring_.reserve(std::min<std::size_t>(capacity_, 256u));
  next_index_ = 0u;
  size_ = 0u;
  next_sequence_ = 1u;
  total_changes_ = 0u;
  epoch_ = std::chrono::steady_clock::now();
  active_.store(!slots_.empty(), std::memory_order_relaxed);
}

std::vector<WatchTarget> GuestMemoryWatch::targets() const {
  std::scoped_lock lock(mutex_);
  std::vector<WatchTarget> result;
  result.reserve(slots_.size());
  for (const auto& slot : slots_) result.push_back(slot.target);
  return result;
}

std::vector<WatchRecord> GuestMemoryWatch::sample(const Reader& reader,
                                                  const WatchObserver& observer) {
  std::vector<WatchRecord> added;
  if (!active()) return added;

  // Read outside the lock: guest memory reads may fault-check and are the slow part.
  std::vector<WatchTarget> targets;
  {
    std::scoped_lock lock(mutex_);
    targets.reserve(slots_.size());
    for (const auto& slot : slots_) targets.push_back(slot.target);
  }
  std::vector<std::optional<std::uint32_t>> values;
  values.reserve(targets.size());
  for (const auto& target : targets) values.push_back(resolve(target, reader));

  const auto differs = [&](std::size_t i) {
    const auto& slot = slots_[i];
    if (slot.target != targets[i]) return false;
    if (slot.seen && slot.value == values[i]) return false;
    return slot.seen || values[i].has_value();
  };

  // Build the (possibly lock-taking) description only when something changed, and
  // before taking our own lock so lock order never depends on the callback.
  std::string extra_note;
  if (observer.describe) {
    bool changed = false;
    {
      std::scoped_lock peek(mutex_);
      for (std::size_t i = 0; i < targets.size() && i < slots_.size() && !changed; ++i) {
        changed = differs(i);
      }
    }
    if (changed) extra_note = observer.describe();
  }

  std::scoped_lock lock(mutex_);
  const auto now = std::chrono::steady_clock::now();
  const auto micros =
      std::chrono::duration_cast<std::chrono::microseconds>(now - epoch_).count();
  for (std::size_t i = 0; i < targets.size() && i < slots_.size(); ++i) {
    auto& slot = slots_[i];
    if (slot.target != targets[i]) continue;  // reconfigured while reading
    const auto& value = values[i];
    if (slot.seen && slot.value == value) continue;
    // An address that is not yet readable has no baseline: the first readable value
    // is reported as the initial observation rather than a change from "unmapped".
    if (!slot.seen && !value) continue;

    WatchRecord record{};
    record.sequence = next_sequence_++;
    record.microseconds = micros;
    record.address = slot.target.address;
    record.offsets = slot.target.offsets;
    record.old_value = slot.value;
    record.new_value = value;
    record.initial = !slot.seen;
    record.observer = observer;
    record.observer.describe = nullptr;
    if (!extra_note.empty()) {
      if (!record.observer.note.empty()) record.observer.note += ' ';
      record.observer.note += extra_note;
    }
    slot.seen = true;
    slot.value = value;
    if (!record.initial) ++total_changes_;

    if (ring_.size() < capacity_) {
      ring_.push_back(record);
    } else {
      ring_[next_index_] = record;
    }
    next_index_ = (next_index_ + 1u) % capacity_;
    size_ = ring_.size();
    added.push_back(std::move(record));
  }
  return added;
}

std::vector<WatchRecord> GuestMemoryWatch::history() const {
  std::scoped_lock lock(mutex_);
  std::vector<WatchRecord> result;
  result.reserve(size_);
  if (size_ < capacity_) {
    result.assign(ring_.begin(), ring_.end());
  } else {
    for (std::size_t i = 0; i < size_; ++i) result.push_back(ring_[(next_index_ + i) % capacity_]);
  }
  return result;
}

std::uint64_t GuestMemoryWatch::total_changes() const {
  std::scoped_lock lock(mutex_);
  return total_changes_;
}

std::optional<std::uint32_t> GuestMemoryWatch::last_value(const WatchTarget& target) const {
  std::scoped_lock lock(mutex_);
  for (const auto& slot : slots_) {
    if (slot.target == target) return slot.value;
  }
  return std::nullopt;
}

std::string GuestMemoryWatch::format(const WatchRecord& record) {
  std::ostringstream out;
  const auto value_text = [&](const std::optional<std::uint32_t>& value) {
    if (!value) {
      out << "<unreadable>";
    } else {
      out << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << *value
          << std::dec << std::setfill(' ');
    }
  };
  out << "watch #" << record.sequence << " +" << record.microseconds << "us "
      << format_watch_target(WatchTarget{record.address, record.offsets}) << ' ';
  if (record.initial) {
    out << "initial ";
    value_text(record.new_value);
  } else {
    value_text(record.old_value);
    out << " -> ";
    value_text(record.new_value);
  }
  out << " observed by ";
  if (record.observer.thread_id != 0u) {
    out << 't' << record.observer.thread_id;
  } else {
    out << "host";
  }
  out << " phase=" << watch_phase_name(record.observer.phase) << " cia=0x" << std::hex
      << std::uppercase << record.observer.cia << " nia=0x" << record.observer.nia << " lr=0x"
      << record.observer.lr << std::dec;
  if (!record.observer.note.empty()) out << ' ' << record.observer.note;
  return out.str();
}

}  // namespace xenon::core

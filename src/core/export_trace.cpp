#include "xenon/core/export_trace.hpp"

#include <algorithm>

namespace xenon::core {
namespace {

template <std::size_t N>
std::string_view array_string_view(const std::array<char, N>& value) noexcept {
  const auto end = std::find(value.begin(), value.end(), '\0');
  return {value.data(), static_cast<std::size_t>(end - value.begin())};
}

}  // namespace

std::string_view ExportTraceRecord::library_view() const noexcept {
  return array_string_view(library);
}

std::string_view ExportTraceRecord::name_view() const noexcept {
  return array_string_view(name);
}

ExportTrace::ExportTrace(std::size_t capacity)
    : records_(std::max<std::size_t>(capacity, 1u)) {}

void ExportTrace::set_enabled(bool enabled) noexcept {
  enabled_.store(enabled, std::memory_order_release);
}

bool ExportTrace::enabled() const noexcept {
  return enabled_.load(std::memory_order_acquire);
}

std::size_t ExportTrace::capacity() const noexcept { return records_.size(); }

template <std::size_t N>
void ExportTrace::copy_text(std::array<char, N>& destination,
                            std::string_view source) noexcept {
  destination.fill('\0');
  const auto count = std::min(source.size(), N - 1u);
  std::copy_n(source.data(), count, destination.data());
}

void ExportTrace::record(std::uint32_t thread_id, std::string_view library,
                         std::string_view name, std::uint32_t ordinal,
                         cpu::GuestAddress call_address, std::uint64_t lr,
                         std::uint64_t ctr,
                         std::span<const std::uint64_t, 8> arguments,
                         std::uint64_t result_r3, bool handler_found,
                         bool handled, bool success) {
  if (!enabled()) return;

  std::scoped_lock lock(mutex_);
  auto& record = records_[next_index_];
  record = {};
  record.sequence = next_sequence_++;
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
  next_index_ = (next_index_ + 1u) % records_.size();
  size_ = std::min(size_ + 1u, records_.size());
}

std::vector<ExportTraceRecord> ExportTrace::snapshot_locked() const {
  std::vector<ExportTraceRecord> result;
  result.reserve(size_);
  const auto oldest = (next_index_ + records_.size() - size_) % records_.size();
  for (std::size_t i = 0; i < size_; ++i) {
    result.push_back(records_[(oldest + i) % records_.size()]);
  }
  return result;
}

std::vector<ExportTraceRecord> ExportTrace::recent_global(
    std::size_t limit) const {
  std::scoped_lock lock(mutex_);
  auto result = snapshot_locked();
  if (result.size() > limit) {
    result.erase(result.begin(), result.end() - static_cast<std::ptrdiff_t>(limit));
  }
  return result;
}

std::vector<ExportTraceRecord> ExportTrace::recent_for_thread(
    std::uint32_t thread_id, std::size_t limit) const {
  std::scoped_lock lock(mutex_);
  const auto all = snapshot_locked();
  std::vector<ExportTraceRecord> result;
  result.reserve(std::min(limit, all.size()));
  for (auto it = all.rbegin(); it != all.rend() && result.size() < limit; ++it) {
    if (it->thread_id == thread_id) result.push_back(*it);
  }
  std::reverse(result.begin(), result.end());
  return result;
}

void ExportTrace::clear() {
  std::scoped_lock lock(mutex_);
  next_sequence_ = 1u;
  next_index_ = 0u;
  size_ = 0u;
  std::fill(records_.begin(), records_.end(), ExportTraceRecord{});
}

}  // namespace xenon::core

#include "memory/guest/address_space_internal.hpp"

namespace xenon::memory {

PhysicalWriteWindow::~PhysicalWriteWindow() noexcept { release(); }

PhysicalWriteWindow::PhysicalWriteWindow(PhysicalWriteWindow&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)) {}

PhysicalWriteWindow& PhysicalWriteWindow::operator=(
    PhysicalWriteWindow&& other) noexcept {
  if (this == &other) return *this;
  release();
  owner_ = std::exchange(other.owner_, nullptr);
  return *this;
}

bool PhysicalWriteWindow::write(
    std::uint32_t physical_address, std::span<const std::byte> source,
    std::uint64_t* published_epoch) noexcept {
  if (published_epoch) *published_epoch = 0u;
  if (!owner_) return false;
  return owner_->write_physical_exclusive(physical_address, source,
                                          published_epoch);
}

void PhysicalWriteWindow::release() noexcept {
  if (!owner_) return;
  auto* owner = std::exchange(owner_, nullptr);
  owner->release_physical_write_window();
}

PhysicalWriteSpan::~PhysicalWriteSpan() noexcept { (void)complete(); }

PhysicalWriteSpan::PhysicalWriteSpan(PhysicalWriteSpan&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      physical_address_(other.physical_address_),
      bytes_(other.bytes_),
      reservation_participant_(other.reservation_participant_) {
  other.bytes_ = {};
}

PhysicalWriteSpan& PhysicalWriteSpan::operator=(PhysicalWriteSpan&& other) noexcept {
  if (this == &other) return *this;
  (void)complete();
  owner_ = std::exchange(other.owner_, nullptr);
  physical_address_ = other.physical_address_;
  bytes_ = other.bytes_;
  reservation_participant_ = other.reservation_participant_;
  other.bytes_ = {};
  other.reservation_participant_ = false;
  return *this;
}

bool PhysicalWriteSpan::write(
    std::uint32_t offset, std::span<const std::byte> source) noexcept {
  if (!owner_ || offset > bytes_.size() ||
      source.size() > bytes_.size() - offset) {
    return false;
  }
  if (!source.empty()) {
    xenon::cpu::detail::atomic_copy_to_guest(bytes_.data() + offset, source);
  }
  return true;
}

bool PhysicalWriteSpan::fill(std::uint32_t offset, std::uint32_t size,
                             std::byte value) noexcept {
  if (!owner_ || offset > bytes_.size() || size > bytes_.size() - offset) {
    return false;
  }
  if (size) {
    xenon::cpu::detail::atomic_fill_guest(
        bytes_.data() + offset, size, std::to_integer<std::uint8_t>(value));
  }
  return true;
}

std::uint64_t PhysicalWriteSpan::commit() noexcept { return complete(); }

std::uint64_t PhysicalWriteSpan::complete() noexcept {
  if (!owner_) return 0u;
  auto* owner = std::exchange(owner_, nullptr);
  const auto address = physical_address_;
  const auto size = static_cast<std::uint32_t>(bytes_.size());
  bytes_ = {};
  const auto epoch = size ? owner->complete_physical_write(
                                address, size, reservation_participant_,
                                xenon::cpu::MemoryOrderingDomain::Device)
                          : 0u;
  reservation_participant_ = false;
  return epoch;
}

PhysicalWriteSpan AddressSpace::physical_write_span(
    std::uint32_t physical_address, std::uint32_t size) noexcept {
  if (!initialized_ || !size || physical_address >= kPhysicalMemorySize ||
      std::uint64_t{physical_address} + size > kPhysicalMemorySize) {
    return {};
  }
  const bool reservation_participant =
      begin_physical_write(physical_address, size);
  return PhysicalWriteSpan(
      this, physical_address,
      std::span<std::byte>(physical_->data() + physical_address, size),
      reservation_participant);
}

PhysicalWriteWindow AddressSpace::physical_write_window() noexcept {
  if (!initialized_) return {};

  std::uint32_t expected = 0u;
  while (!coherency_commit_gate_.compare_exchange_weak(
      expected, 1u, std::memory_order_acq_rel, std::memory_order_acquire)) {
    expected = 0u;
    std::this_thread::yield();
  }

  // Writers that entered before the gate was raised are allowed to finish.
  // New ordinary writers observe the gate in MemoryAccessContext::enter_write.
  while (coherency_.active_writers_data()->load(std::memory_order_acquire) !=
         0u) {
    std::this_thread::yield();
  }

  // Store-conditionals use a separate short commit gate and don't otherwise
  // participate in the normal writer entry protocol. Acquire it after normal
  // writers have drained so the final readback ownership point is stable.
  reservation_commit_gate_->acquire();
  while (coherency_.active_writers_data()->load(std::memory_order_acquire) !=
             0u ||
         active_reservation_ops_.load(std::memory_order_acquire) != 0u) {
    std::this_thread::yield();
  }
  return PhysicalWriteWindow(this);
}

bool AddressSpace::write_physical_exclusive(
    std::uint32_t physical_address, std::span<const std::byte> source,
    std::uint64_t* published_epoch) noexcept {
  if (published_epoch) *published_epoch = 0u;
  if (!initialized_ || source.empty() ||
      physical_address >= kPhysicalMemorySize ||
      std::uint64_t{physical_address} + source.size() > kPhysicalMemorySize ||
      coherency_commit_gate_.load(std::memory_order_acquire) == 0u ||
      !reservation_commit_gate_->held()) {
    return source.empty() && initialized_;
  }

  auto view = make_fast_memory_view();
  // This window owns both gates, so it may become the sole active writer
  // without going through the normal gate-entry loop.
  view.active_coherency_writers->fetch_add(1u, std::memory_order_acq_rel);
  xenon::cpu::reservation_monitor_detail::invalidate_range(
      view, physical_address, static_cast<std::uint32_t>(source.size()));
  xenon::cpu::detail::atomic_copy_to_guest(
      physical_->data() + physical_address, source);
  const auto epoch = xenon::cpu::reservation_monitor_detail::finish_write(
      view, physical_address, static_cast<std::uint32_t>(source.size()), false,
      xenon::cpu::MemoryOrderingDomain::Device);
  if (published_epoch) *published_epoch = epoch;
  return true;
}

void AddressSpace::release_physical_write_window() noexcept {
  reservation_commit_gate_->release();
  coherency_commit_gate_.store(0u, std::memory_order_release);
}

}  // namespace xenon::memory

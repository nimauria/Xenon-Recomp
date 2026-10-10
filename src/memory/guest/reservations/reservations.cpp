#include "memory/guest/address_space_internal.hpp"

namespace xenon::memory {

void AddressSpace::begin_reservation_operation() noexcept {
  for (;;) {
    while (reservation_commit_gate_->held()) {
      std::this_thread::yield();
    }
    active_reservation_ops_.fetch_add(1u, std::memory_order_acq_rel);
    if (!reservation_commit_gate_->held()) return;
    active_reservation_ops_.fetch_sub(1u, std::memory_order_release);
  }
}

void AddressSpace::end_reservation_operation() noexcept {
  active_reservation_ops_.fetch_sub(1u, std::memory_order_release);
}

std::uint64_t AddressSpace::claim_reservation(
    std::uint32_t physical_address, std::uint32_t width) noexcept {
  if (physical_address >= kPhysicalMemorySize ||
      (width != 4u && width != 8u)) {
    return 0u;
  }

  const auto granule = physical_address / kReservationGranuleSize;
  const auto word = granule >> 6u;
  const auto bit = std::uint64_t{1} << (granule & 63u);
  reservation_seen_bitmap_[word].fetch_or(bit, std::memory_order_release);

  auto generation = reservation_next_generation_.fetch_add(
      1u, std::memory_order_acq_rel);
  if (generation == 0u) {
    generation = reservation_next_generation_.fetch_add(
        1u, std::memory_order_acq_rel);
    if (generation == 0u) generation = 1u;
  }

  const auto descriptor =
      xenon::cpu::reservation_monitor_detail::encode_slot(
          physical_address, width, generation,
          xenon::cpu::reservation_monitor_detail::SlotStatus::Active);
  for (std::uint32_t slot_index = 0; slot_index < kReservationSlotCount;
       ++slot_index) {
    auto expected = std::uint64_t{0};
    if (reservation_slots_[slot_index].compare_exchange_strong(
            expected, descriptor, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      return make_reservation_token(slot_index, generation);
    }
  }

  // Xenon exposes six hardware threads, so production code should never need a
  // seventh simultaneously live reservation. Failing to claim a slot models a
  // conservatively lost reservation rather than stealing another thread's.
  return 0u;
}

void AddressSpace::cancel_reservation(std::uint64_t token) noexcept {
  std::uint32_t slot_index = 0;
  std::uint32_t generation = 0;
  if (!decode_reservation_token(token, slot_index, generation) ||
      slot_index >= kReservationSlotCount) {
    return;
  }
  auto& slot = reservation_slots_[slot_index];
  auto descriptor = slot.load(std::memory_order_acquire);
  for (;;) {
    if (descriptor == 0u ||
        xenon::cpu::reservation_monitor_detail::slot_generation(descriptor) !=
            generation ||
        xenon::cpu::reservation_monitor_detail::slot_status(descriptor) !=
            xenon::cpu::reservation_monitor_detail::SlotStatus::Active) {
      return;
    }
    if (slot.compare_exchange_weak(descriptor, 0u,
                                   std::memory_order_acq_rel,
                                   std::memory_order_acquire)) {
      return;
    }
  }
}

void AddressSpace::invalidate_reservations(
    std::uint32_t physical_address, std::uint32_t width) noexcept {
  auto view = make_fast_memory_view();
  xenon::cpu::reservation_monitor_detail::invalidate_range(
      view, physical_address, width);
}

bool AddressSpace::claim_store_conditional(
    std::uint32_t physical_address, std::uint32_t width, std::uint64_t token,
    std::uint32_t& slot_index,
    std::uint64_t& committing_descriptor) noexcept {
  std::uint32_t generation = 0;
  if (!decode_reservation_token(token, slot_index, generation) ||
      slot_index >= kReservationSlotCount) {
    return false;
  }

  auto& slot = reservation_slots_[slot_index];
  auto descriptor = slot.load(std::memory_order_acquire);
  if (descriptor == 0u ||
      xenon::cpu::reservation_monitor_detail::slot_status(descriptor) !=
          xenon::cpu::reservation_monitor_detail::SlotStatus::Active ||
      xenon::cpu::reservation_monitor_detail::slot_generation(descriptor) !=
          generation ||
      xenon::cpu::reservation_monitor_detail::slot_physical_address(
          descriptor) != physical_address ||
      xenon::cpu::reservation_monitor_detail::slot_width(descriptor) != width) {
    cancel_reservation(token);
    return false;
  }

  committing_descriptor =
      xenon::cpu::reservation_monitor_detail::encode_slot(
          physical_address, width, generation,
          xenon::cpu::reservation_monitor_detail::SlotStatus::Committing);
  return slot.compare_exchange_strong(descriptor, committing_descriptor,
                                      std::memory_order_acq_rel,
                                      std::memory_order_acquire);
}

std::uint64_t AddressSpace::reserve32(GuestAddress address,
                                      std::uint32_t& value) {
  validate_guest_range(address, sizeof(value), AccessKind::Read);
  auto access = access_context();
  xenon::cpu::MemoryAccessContext::PhysicalResolution fast_resolved{};
  std::byte* ptr = nullptr;
  std::uint32_t physical_address = 0u;
  if (access.resolve_physical_ram(address, sizeof(value), false, alignof(std::uint32_t),
                                  fast_resolved)) {
    ptr = fast_resolved.ptr;
    physical_address = fast_resolved.physical_address;
  } else {
    std::lock_guard lock(mutex_);
    auto resolved = resolve_contiguous(address, sizeof(value), AccessKind::Read);
    if (!resolved || !resolved->physical) {
      fault(address, sizeof(value), AccessKind::Read, FaultReason::Unmapped,
            "reservation has no physical RAM backing");
    }
    ptr = resolved->ptr;
    physical_address = resolved->physical_address;
  }

  begin_reservation_operation();
  const auto epoch_before =
      coherency_.write_epoch_data()->load(std::memory_order_acquire);
  auto token = claim_reservation(physical_address, sizeof(value));
  value = atomic_load_guest_be<std::uint32_t>(ptr);
  const auto epoch_after =
      coherency_.write_epoch_data()->load(std::memory_order_acquire);
  const auto writers =
      coherency_.active_writers_data()->load(std::memory_order_acquire);
  if (token && (epoch_before != epoch_after || writers != 0u)) {
    cancel_reservation(token);
    token = 0u;
  }
  end_reservation_operation();
  return token;
}

std::uint64_t AddressSpace::reserve64(GuestAddress address,
                                      std::uint64_t& value) {
  validate_guest_range(address, sizeof(value), AccessKind::Read);
  auto access = access_context();
  xenon::cpu::MemoryAccessContext::PhysicalResolution fast_resolved{};
  std::byte* ptr = nullptr;
  std::uint32_t physical_address = 0u;
  if (access.resolve_physical_ram(address, sizeof(value), false, alignof(std::uint64_t),
                                  fast_resolved)) {
    ptr = fast_resolved.ptr;
    physical_address = fast_resolved.physical_address;
  } else {
    std::lock_guard lock(mutex_);
    auto resolved = resolve_contiguous(address, sizeof(value), AccessKind::Read);
    if (!resolved || !resolved->physical) {
      fault(address, sizeof(value), AccessKind::Read, FaultReason::Unmapped,
            "reservation has no physical RAM backing");
    }
    ptr = resolved->ptr;
    physical_address = resolved->physical_address;
  }

  begin_reservation_operation();
  const auto epoch_before =
      coherency_.write_epoch_data()->load(std::memory_order_acquire);
  auto token = claim_reservation(physical_address, sizeof(value));
  value = atomic_load_guest_be<std::uint64_t>(ptr);
  const auto epoch_after =
      coherency_.write_epoch_data()->load(std::memory_order_acquire);
  const auto writers =
      coherency_.active_writers_data()->load(std::memory_order_acquire);
  if (token && (epoch_before != epoch_after || writers != 0u)) {
    cancel_reservation(token);
    token = 0u;
  }
  end_reservation_operation();
  return token;
}

bool AddressSpace::store_conditional32(GuestAddress address,
                                       std::uint64_t token,
                                       std::uint32_t value) {
  validate_guest_range(address, sizeof(value), AccessKind::Write);
  auto access = access_context();
  xenon::cpu::MemoryAccessContext::PhysicalResolution fast_resolved{};
  std::byte* ptr = nullptr;
  std::uint32_t physical_address = 0u;
  if (access.resolve_physical_ram(address, sizeof(value), true, alignof(std::uint32_t),
                                  fast_resolved)) {
    ptr = fast_resolved.ptr;
    physical_address = fast_resolved.physical_address;
  } else {
    std::lock_guard lock(mutex_);
    auto resolved = resolve_contiguous(address, sizeof(value), AccessKind::Write);
    if (!resolved || !resolved->physical || !token) {
      cancel_reservation(token);
      return false;
    }
    ptr = resolved->ptr;
    physical_address = resolved->physical_address;
  }
  if (!token) return false;

  reservation_commit_gate_->acquire();
  while (coherency_.active_writers_data()->load(std::memory_order_acquire) !=
             0u ||
         active_reservation_ops_.load(std::memory_order_acquire) != 0u) {
    std::this_thread::yield();
  }

  std::uint32_t slot_index = 0u;
  std::uint64_t committing = 0u;
  const auto claimed = claim_store_conditional(
      physical_address, sizeof(value), token, slot_index, committing);
  if (!claimed) {
    reservation_commit_gate_->release();
    return false;
  }

  invalidate_reservations(physical_address, sizeof(value));
  atomic_store_guest_be<std::uint32_t>(ptr, value);
  coherency_.mark_write(physical_address, sizeof(value));
  reservation_slots_[slot_index].store(0u, std::memory_order_release);
  reservation_commit_gate_->release();
  return true;
}

bool AddressSpace::store_conditional64(GuestAddress address,
                                       std::uint64_t token,
                                       std::uint64_t value) {
  validate_guest_range(address, sizeof(value), AccessKind::Write);
  auto access = access_context();
  xenon::cpu::MemoryAccessContext::PhysicalResolution fast_resolved{};
  std::byte* ptr = nullptr;
  std::uint32_t physical_address = 0u;
  if (access.resolve_physical_ram(address, sizeof(value), true, alignof(std::uint64_t),
                                  fast_resolved)) {
    ptr = fast_resolved.ptr;
    physical_address = fast_resolved.physical_address;
  } else {
    std::lock_guard lock(mutex_);
    auto resolved = resolve_contiguous(address, sizeof(value), AccessKind::Write);
    if (!resolved || !resolved->physical || !token) {
      cancel_reservation(token);
      return false;
    }
    ptr = resolved->ptr;
    physical_address = resolved->physical_address;
  }
  if (!token) return false;

  reservation_commit_gate_->acquire();
  while (coherency_.active_writers_data()->load(std::memory_order_acquire) !=
             0u ||
         active_reservation_ops_.load(std::memory_order_acquire) != 0u) {
    std::this_thread::yield();
  }

  std::uint32_t slot_index = 0u;
  std::uint64_t committing = 0u;
  const auto claimed = claim_store_conditional(
      physical_address, sizeof(value), token, slot_index, committing);
  if (!claimed) {
    reservation_commit_gate_->release();
    return false;
  }

  invalidate_reservations(physical_address, sizeof(value));
  atomic_store_guest_be<std::uint64_t>(ptr, value);
  coherency_.mark_write(physical_address, sizeof(value));
  reservation_slots_[slot_index].store(0u, std::memory_order_release);
  reservation_commit_gate_->release();
  return true;
}

std::size_t AddressSpace::reservation_monitor_storage_bytes() const noexcept {
  return reservation_slots_.size() * sizeof(reservation_slots_[0]) +
         reservation_seen_bitmap_.size() * sizeof(reservation_seen_bitmap_[0]) +
         sizeof(reservation_next_generation_) +
         sizeof(*reservation_commit_gate_) + sizeof(active_reservation_ops_);
}

}  // namespace xenon::memory

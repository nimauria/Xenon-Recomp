#include "memory/guest/address_space_internal.hpp"

namespace xenon::memory {

xenon::cpu::FastMemoryView AddressSpace::make_fast_memory_view() noexcept {
  xenon::cpu::FastMemoryView view{};
  if (initialized_ && physical_ && physical_->data()) {
    view.physical_base = physical_->data();
    if (guest_aperture_ && guest_aperture_->active()) {
      view.guest_aperture_base = guest_aperture_->base();
    }
    view.page_table = hot_pages_.data();
    view.page_count = kPageCount;
    view.page_shift = kPageShift;
    view.physical_size = kPhysicalMemorySize;
    view.reservation_slots = reservation_slots_.data();
    view.reservation_seen_bitmap = reservation_seen_bitmap_.data();
    view.reservation_slot_count = kReservationSlotCount;
    view.reservation_seen_word_count = kReservationBitmapWordCount;
    view.reservation_granule_size = kReservationGranuleSize;
    view.reservation_granule_count = kReservationGranuleCount;
    view.reservation_commit_gate = reservation_commit_gate_.get();
    view.active_reservation_ops = &active_reservation_ops_;
    view.reservation_next_generation = &reservation_next_generation_;
    view.physical_page_epochs = coherency_.page_epochs_data();
    view.physical_page_count = kPhysicalPageCount;
    view.global_write_epoch = coherency_.write_epoch_data();
    view.active_coherency_writers = coherency_.active_writers_data();
    view.coherency_commit_gate = &coherency_commit_gate_;
    view.coherency_journal_epochs = coherency_.write_journal_epochs_data();
    view.coherency_journal_ranges = coherency_.write_journal_ranges_data();
    view.coherency_journal_domains = coherency_.write_journal_domains_data();
    view.coherency_journal_capacity = GuestMemoryCoherency::kWriteJournalCapacity;
    view.executable_page_generations = executable_page_generations_.data();
    view.active_fast_readers = &active_fast_readers_;
    view.reclaim_pending = &reclaim_pending_;
  }
  return view;
}

xenon::cpu::MemoryAccessContext AddressSpace::access_context() noexcept {
  return xenon::cpu::MemoryAccessContext(*this, make_fast_memory_view());
}

const RegionDescriptor* AddressSpace::region_for(GuestAddress address) noexcept {
  for (const auto& region : kRegions) {
    if (address >= region.base && address <= region.end) return &region;
  }
  return nullptr;
}

std::uint32_t AddressSpace::physical_alias_address(GuestAddress address) const {
  if (address >= kGpuWritebackBase && address <= kGpuWritebackEnd)
    return address - kGpuWritebackBase;
  if (address >= kPhysical64KBase && address <= kPhysical16MEnd)
    return address & 0x1FFFFFFFu;
  if (address >= kPhysical4KBase && address <= kPhysical4KHeapEnd)
    return (address - kPhysical4KBase) + kPhysical4KViewOffset;
  return 0xFFFFFFFFu;
}

Protect AddressSpace::physical_protect(
    std::uint32_t physical_address) const noexcept {
  if (physical_address >= kPhysicalMemorySize) return Protect::None;
  return physical_page_metadata_[physical_address >> kPageShift].current_protect;
}

std::uint32_t AddressSpace::fetch32_be(GuestAddress address) {
  validate_guest_range(address, 4u, AccessKind::Execute);
  if (snapshot_mmio(address, 4u)) {
    fault(address, 4u, AccessKind::Execute, FaultReason::Protection,
          "instructions may not be fetched from MMIO");
  }
  std::lock_guard lock(mutex_);
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    const auto b = std::to_integer<std::uint8_t>(
        *resolve_byte_for_request(address + static_cast<GuestAddress>(i),
                                  AccessKind::Execute, address, 4u).ptr);
    value = (value << 8u) | b;
  }
  return value;
}

AddressSpace::ResolvedByte AddressSpace::resolve_byte(GuestAddress address,
                                                       AccessKind access) {
  return resolve_byte_for_request(address, access, address, 1u);
}

AddressSpace::ResolvedByte AddressSpace::resolve_byte_for_request(
    GuestAddress address, AccessKind access, GuestAddress request_address,
    std::size_t request_width) {
  const auto direct = physical_alias_address(address);
  if (direct != 0xFFFFFFFFu) {
    if (direct >= kPhysicalMemorySize) {
      fault_at(request_address, address, request_width, access,
               FaultReason::OutOfRange, "physical alias outside RAM");
    }
    const auto protect = physical_protect(direct);
    const bool allowed =
        access == AccessKind::Read
            ? has(protect, Protect::Read)
            : access == AccessKind::Write
                  ? has(protect, Protect::Write)
                  : has(protect, Protect::Execute);
    if (!allowed) {
      fault_at(request_address, address, request_width, access,
               FaultReason::Protection,
               "physical alias protection violation");
    }
    return {physical_->data() + direct, direct, true};
  }
  const auto* region = region_for(address);
  if (!region) {
    fault_at(request_address, address, request_width, access,
             FaultReason::Unmapped, "guest address has no region");
  }
  if (region->kind == RegionKind::Mmio) {
    fault_at(request_address, address, request_width, access,
             FaultReason::Unmapped, "MMIO range has no handler");
  }
  const auto& page = pages_[address >> kPageShift];
  if (page.state == PageState::Free) {
    fault_at(request_address, address, request_width, access,
             FaultReason::Unmapped, "guest page is free");
  }
  if (page.state != PageState::Committed) {
    fault_at(request_address, address, request_width, access,
             FaultReason::Uncommitted,
             "guest page is reserved but uncommitted");
  }
  const bool allowed =
      access == AccessKind::Read
          ? has(page.current_protect, Protect::Read)
          : access == AccessKind::Write
                ? has(page.current_protect, Protect::Write)
                : has(page.current_protect, Protect::Execute);
  if (!allowed) {
    fault_at(request_address, address, request_width, access,
             FaultReason::Protection, "guest page protection violation");
  }
  if (page.physical_page == kInvalidPhysicalPage) {
    fault_at(request_address, address, request_width, access,
             FaultReason::Uncommitted,
             "committed guest page has no physical backing");
  }
  const auto physical_address =
      page.physical_page * kBasePageSize +
      (address & (kBasePageSize - 1u));
  return {physical_->data() + physical_address, physical_address, true};
}

std::optional<AddressSpace::ResolvedByte> AddressSpace::resolve_contiguous(
    GuestAddress address, std::size_t width, AccessKind access) {
  if (!width) return std::nullopt;
  const std::uint64_t last_address64 = std::uint64_t(address) + width - 1u;
  if (last_address64 > 0xFFFFFFFFull) return std::nullopt;
  const auto last_address = static_cast<GuestAddress>(last_address64);

  const auto direct = physical_alias_address(address);
  if (direct != 0xFFFFFFFFu) {
    const auto direct_last = physical_alias_address(last_address);
    if (direct_last == direct + width - 1u && direct_last < kPhysicalMemorySize) {
      const auto first_page = direct >> kPageShift;
      const auto last_page = direct_last >> kPageShift;
      for (std::uint32_t page = first_page; page <= last_page; ++page) {
        const auto protect = physical_page_metadata_[page].current_protect;
        const bool allowed =
            access == AccessKind::Read
                ? has(protect, Protect::Read)
                : access == AccessKind::Write
                      ? has(protect, Protect::Write)
                      : has(protect, Protect::Execute);
        if (!allowed) {
          fault(address, width, access, FaultReason::Protection,
                "physical alias protection violation");
        }
      }
      return ResolvedByte{physical_->data() + direct, direct, true};
    }
    return std::nullopt;
  }

  if ((address >> kPageShift) != (last_address >> kPageShift)) return std::nullopt;
  const auto* region = region_for(address);
  if (!region || region->kind == RegionKind::Mmio || region->kind == RegionKind::PhysicalAlias ||
      region->kind == RegionKind::GpuWriteback) return std::nullopt;
  const auto& page = pages_[address >> kPageShift];
  if (page.state == PageState::Free) fault(address, width, access, FaultReason::Unmapped, "guest page is free");
  if (page.state != PageState::Committed)
    fault(address, width, access, FaultReason::Uncommitted, "guest page is reserved but uncommitted");
  const bool allowed = access == AccessKind::Read ? has(page.current_protect, Protect::Read)
                     : access == AccessKind::Write ? has(page.current_protect, Protect::Write)
                                                   : has(page.current_protect, Protect::Execute);
  if (!allowed) fault(address, width, access, FaultReason::Protection, "guest page protection violation");
  if (page.physical_page == kInvalidPhysicalPage)
    fault(address, width, access, FaultReason::Uncommitted, "committed guest page has no physical backing");
  const auto physical_address = page.physical_page * kBasePageSize + (address & (kBasePageSize - 1u));
  return ResolvedByte{physical_->data() + physical_address, physical_address, true};
}

AddressSpace::ConstResolvedByte AddressSpace::resolve_byte_const(GuestAddress address, AccessKind access) const {
  const auto r = const_cast<AddressSpace*>(this)->resolve_byte(address, access);
  return {r.ptr, r.physical_address, r.physical};
}

AddressSpace::MmioRangeRef AddressSpace::find_mmio_locked(
    GuestAddress address, std::uint32_t width) const {
  if (!width || mmio_ranges_.empty()) return {};
  const auto end = std::uint64_t{address} + width;
  if (end > (std::uint64_t{std::numeric_limits<GuestAddress>::max()} + 1u)) {
    return {};
  }

  const auto it = std::upper_bound(
      mmio_ranges_.begin(), mmio_ranges_.end(), address,
      [](GuestAddress value, const MmioRangeRef& range) {
        return value < range->base;
      });
  if (it == mmio_ranges_.begin()) return {};
  const auto& candidate = *std::prev(it);
  const auto range_end = std::uint64_t{candidate->base} + candidate->size;
  if (address < candidate->base || end > range_end) return {};
  return candidate;
}

AddressSpace::MmioRangeRef AddressSpace::snapshot_mmio(
    GuestAddress address, std::uint32_t width) const {
  std::lock_guard lock(mutex_);
  return find_mmio_locked(address, width);
}

bool AddressSpace::range_has_mmio(GuestAddress address,
                                  std::uint32_t width) const {
  if (!width) return false;
  const auto end = std::uint64_t{address} + width;
  if (end > (std::uint64_t{std::numeric_limits<GuestAddress>::max()} + 1u)) {
    return false;
  }
  std::lock_guard lock(mutex_);
  if (mmio_ranges_.empty()) return false;

  // Find the first range whose end is after the query start, then check whether
  // it starts before the query end. Ranges are sorted and non-overlapping.
  auto it = std::upper_bound(
      mmio_ranges_.begin(), mmio_ranges_.end(), address,
      [](GuestAddress value, const MmioRangeRef& range) {
        return value < range->base;
      });
  if (it != mmio_ranges_.begin()) {
    const auto& previous = *std::prev(it);
    if (std::uint64_t{previous->base} + previous->size > address) return true;
  }
  return it != mmio_ranges_.end() && std::uint64_t{(*it)->base} < end;
}

std::uint64_t AddressSpace::read_mmio(GuestAddress address,
                                      std::uint32_t width) const {
  validate_guest_range(address, width, AccessKind::Read);
  const auto range = snapshot_mmio(address, width);
  if (!range || !range->read) {
    const auto reason = range_has_mmio(address, width)
                            ? FaultReason::MmioWidth
                            : FaultReason::Unmapped;
    fault(address, width, AccessKind::Read, reason,
          reason == FaultReason::MmioWidth
              ? "MMIO read crosses a device boundary or unsupported width"
              : "MMIO read has no handler");
  }
  // Deliberately outside mutex_: device code may block, re-enter Xenon memory,
  // register another device, or synchronize with a management thread.
  return range->read(address, width);
}

void AddressSpace::write_mmio(GuestAddress address, std::uint32_t width,
                              std::uint64_t value) {
  validate_guest_range(address, width, AccessKind::Write);
  const auto range = snapshot_mmio(address, width);
  if (!range || !range->write) {
    const auto reason = range_has_mmio(address, width)
                            ? FaultReason::MmioWidth
                            : FaultReason::Unmapped;
    fault(address, width, AccessKind::Write, reason,
          reason == FaultReason::MmioWidth
              ? "MMIO write crosses a device boundary or unsupported width"
              : "MMIO write has no handler");
  }
  // Shared ownership keeps the handler alive if clear_mmio_ranges() runs while
  // the callback is executing. No global memory-management lock is held here.
  range->write(address, width, value);
}

template <typename T>
T AddressSpace::read_integer(GuestAddress address, bool little_endian) {
  validate_guest_range(address, sizeof(T), AccessKind::Read);
  // Direct MemoryPort callers get the same lock-free common-RAM path as
  // generated PPC. Only exceptional/unaligned accesses fall through to the
  // cold management-protected resolver below.
  {
    auto access = access_context();
    xenon::cpu::MemoryAccessContext::PhysicalResolution resolved{};
    if (access.resolve_physical_ram(address, sizeof(T), false, alignof(T),
                                    resolved)) {
      if constexpr (sizeof(T) == 2u) {
        return little_endian ? static_cast<T>(access.read16_le(address))
                             : static_cast<T>(access.read16_be(address));
      } else if constexpr (sizeof(T) == 4u) {
        return little_endian ? static_cast<T>(access.read32_le(address))
                             : static_cast<T>(access.read32_be(address));
      } else {
        return little_endian ? static_cast<T>(access.read64_le(address))
                             : static_cast<T>(access.read64_be(address));
      }
    }
  }

  if (const auto mmio = snapshot_mmio(address, sizeof(T)); mmio) {
    if (!mmio->read) {
      fault(address, sizeof(T), AccessKind::Read, FaultReason::Unmapped,
            "MMIO read has no handler");
    }
    const T v = static_cast<T>(mmio->read(address, sizeof(T)));
    return little_endian ? byteswap_if(v, true) : v;
  }
  if (range_has_mmio(address, static_cast<std::uint32_t>(sizeof(T)))) {
    fault(address, sizeof(T), AccessKind::Read, FaultReason::MmioWidth,
          "MMIO read crosses a device boundary or unsupported width");
  }

  std::lock_guard lock(mutex_);
  if (auto contiguous = resolve_contiguous(address, sizeof(T), AccessKind::Read);
      contiguous &&
      (reinterpret_cast<std::uintptr_t>(contiguous->ptr) & (alignof(T) - 1u)) == 0u) {
    const auto raw = xenon::cpu::detail::atomic_load_relaxed<T>(contiguous->ptr);
    const bool host_little = std::endian::native == std::endian::little;
    const bool swap = little_endian ? !host_little : host_little;
    return byteswap_if(raw, swap);
  }
  T value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    const auto resolved = resolve_byte_for_request(
        address + static_cast<GuestAddress>(i), AccessKind::Read, address,
        sizeof(T));
    const auto b = xenon::cpu::detail::atomic_load_relaxed<std::uint8_t>(resolved.ptr);
    if (little_endian) value |= static_cast<T>(b) << (i * 8u);
    else value = static_cast<T>((value << 8u) | b);
  }
  return value;
}

template <typename T>
void AddressSpace::write_integer(GuestAddress address, T value, bool little_endian) {
  if constexpr (sizeof(T) == 4u) {
    if (address == 0x62D78u || address == 0x62DC8u) {
      xenon::cpu::debug_signal_write_trap(address, static_cast<std::uint64_t>(value));
    }
  }
  validate_guest_range(address, sizeof(T), AccessKind::Write);
  {
    auto access = access_context();
    xenon::cpu::MemoryAccessContext::PhysicalResolution resolved{};
    if (access.resolve_physical_ram(address, sizeof(T), true, alignof(T),
                                    resolved)) {
      if constexpr (sizeof(T) == 2u) {
        if (little_endian) access.write16_le(address, static_cast<std::uint16_t>(value));
        else access.write16_be(address, static_cast<std::uint16_t>(value));
      } else if constexpr (sizeof(T) == 4u) {
        if (little_endian) access.write32_le(address, static_cast<std::uint32_t>(value));
        else access.write32_be(address, static_cast<std::uint32_t>(value));
      } else {
        if (little_endian) access.write64_le(address, static_cast<std::uint64_t>(value));
        else access.write64_be(address, static_cast<std::uint64_t>(value));
      }
      return;
    }
  }

  if (const auto mmio = snapshot_mmio(address, sizeof(T)); mmio) {
    if (!mmio->write) {
      fault(address, sizeof(T), AccessKind::Write, FaultReason::Unmapped,
            "MMIO write has no handler");
    }
    mmio->write(address, sizeof(T),
                little_endian ? byteswap_if(value, true) : value);
    return;
  }
  if (range_has_mmio(address, static_cast<std::uint32_t>(sizeof(T)))) {
    fault(address, sizeof(T), AccessKind::Write, FaultReason::MmioWidth,
          "MMIO write crosses a device boundary or unsupported width");
  }

  std::lock_guard lock(mutex_);
  if (auto contiguous = resolve_contiguous(address, sizeof(T), AccessKind::Write);
      contiguous &&
      (reinterpret_cast<std::uintptr_t>(contiguous->ptr) & (alignof(T) - 1u)) == 0u) {
    const bool host_little = std::endian::native == std::endian::little;
    const bool swap = little_endian ? !host_little : host_little;
    const T raw = byteswap_if(value, swap);
    const bool reservation_participant =
        begin_physical_write(contiguous->physical_address, sizeof(T));
    xenon::cpu::detail::atomic_store_relaxed<T>(contiguous->ptr, raw);
    complete_physical_write(contiguous->physical_address, sizeof(T),
                            reservation_participant, ordering_domain(address));
    return;
  }
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    const unsigned shift = little_endian ? static_cast<unsigned>(i * 8u)
                                         : static_cast<unsigned>((sizeof(T) - 1u - i) * 8u);
    auto resolved = resolve_byte_for_request(
        address + static_cast<GuestAddress>(i), AccessKind::Write, address,
        sizeof(T));
    const bool reservation_participant =
        begin_physical_write(resolved.physical_address, 1u);
    xenon::cpu::detail::atomic_store_relaxed<std::uint8_t>(
        resolved.ptr, static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    complete_physical_write(resolved.physical_address, 1u,
                            reservation_participant,
                            ordering_domain(address + static_cast<GuestAddress>(i)));
  }
}

std::uint8_t AddressSpace::read8(GuestAddress address) {
  validate_guest_range(address, 1u, AccessKind::Read);
  {
    auto access = access_context();
    xenon::cpu::MemoryAccessContext::PhysicalResolution resolved{};
    if (access.resolve_physical_ram(address, 1u, false, 1u, resolved)) {
      return xenon::cpu::detail::atomic_load_relaxed<std::uint8_t>(resolved.ptr);
    }
  }
  if (const auto mmio = snapshot_mmio(address, 1u); mmio) {
    if (!mmio->read) {
      fault(address, 1u, AccessKind::Read, FaultReason::Unmapped,
            "MMIO read has no handler");
    }
    return static_cast<std::uint8_t>(mmio->read(address, 1u));
  }

  std::lock_guard lock(mutex_);
  const auto resolved = resolve_byte(address, AccessKind::Read);
  return xenon::cpu::detail::atomic_load_relaxed<std::uint8_t>(resolved.ptr);
}

std::uint16_t AddressSpace::read16_be(GuestAddress a) { return read_integer<std::uint16_t>(a, false); }

std::uint32_t AddressSpace::read32_be(GuestAddress a) { return read_integer<std::uint32_t>(a, false); }

std::uint64_t AddressSpace::read64_be(GuestAddress a) { return read_integer<std::uint64_t>(a, false); }

xenon::cpu::Vector128 AddressSpace::read128(GuestAddress address) {
  validate_guest_range(address, 16u, AccessKind::Read);
  {
    auto access = access_context();
    xenon::cpu::MemoryAccessContext::PhysicalResolution resolved{};
    if (access.resolve_physical_ram(address, 16u, false,
                                    alignof(std::uint64_t), resolved)) {
      return access.read128(address);
    }
  }

  xenon::cpu::Vector128 v{};
  if (range_has_mmio(address, static_cast<std::uint32_t>(v.bytes.size()))) {
    for (std::size_t i = 0; i < v.bytes.size(); ++i) {
      try {
        v.bytes[i] = read8(address + static_cast<GuestAddress>(i));
      } catch (const MemoryFault& e) {
        fault_at(address, e.fault_address(), 16u, AccessKind::Read, e.reason(),
                 e.what());
      }
    }
    return v;
  }

  std::lock_guard lock(mutex_);
  if (auto contiguous = resolve_contiguous(address, v.bytes.size(), AccessKind::Read);
      contiguous &&
      (reinterpret_cast<std::uintptr_t>(contiguous->ptr) &
       (alignof(std::uint64_t) - 1u)) == 0u) {
    const auto lo = xenon::cpu::detail::atomic_load_relaxed<std::uint64_t>(
        contiguous->ptr);
    const auto hi = xenon::cpu::detail::atomic_load_relaxed<std::uint64_t>(
        contiguous->ptr + 8u);
    std::memcpy(v.bytes.data(), &lo, sizeof(lo));
    std::memcpy(v.bytes.data() + 8u, &hi, sizeof(hi));
    return v;
  }
  for (std::size_t i = 0; i < v.bytes.size(); ++i) {
    const auto resolved = resolve_byte_for_request(
        address + static_cast<GuestAddress>(i), AccessKind::Read, address, 16u);
    v.bytes[i] =
        xenon::cpu::detail::atomic_load_relaxed<std::uint8_t>(resolved.ptr);
  }
  return v;
}

void AddressSpace::write8(GuestAddress address, std::uint8_t value) {
  validate_guest_range(address, 1u, AccessKind::Write);
  {
    auto access = access_context();
    xenon::cpu::MemoryAccessContext::PhysicalResolution resolved{};
    if (access.resolve_physical_ram(address, 1u, true, 1u, resolved)) {
      access.write8(address, value);
      return;
    }
  }

  if (const auto mmio = snapshot_mmio(address, 1u); mmio) {
    if (!mmio->write) {
      fault(address, 1u, AccessKind::Write, FaultReason::Unmapped,
            "MMIO write has no handler");
    }
    mmio->write(address, 1u, value);
    return;
  }

  std::lock_guard lock(mutex_);
  auto resolved = resolve_byte(address, AccessKind::Write);
  const bool reservation_participant =
      begin_physical_write(resolved.physical_address, 1u);
  xenon::cpu::detail::atomic_store_relaxed<std::uint8_t>(resolved.ptr, value);
  complete_physical_write(resolved.physical_address, 1u,
                          reservation_participant, ordering_domain(address));
}

void AddressSpace::write16_be(GuestAddress a, std::uint16_t v) { write_integer(a, v, false); }

void AddressSpace::write32_be(GuestAddress a, std::uint32_t v) { write_integer(a, v, false); }

void AddressSpace::write64_be(GuestAddress a, std::uint64_t v) { write_integer(a, v, false); }

void AddressSpace::write128(GuestAddress address, const xenon::cpu::Vector128& value) {
  validate_guest_range(address, 16u, AccessKind::Write);
  {
    auto access = access_context();
    xenon::cpu::MemoryAccessContext::PhysicalResolution resolved{};
    if (access.resolve_physical_ram(address, 16u, true,
                                    alignof(std::uint64_t), resolved)) {
      access.write128(address, value);
      return;
    }
  }

  if (range_has_mmio(address, static_cast<std::uint32_t>(value.bytes.size()))) {
    for (std::size_t i = 0; i < value.bytes.size(); ++i) {
      try {
        write8(address + static_cast<GuestAddress>(i), value.bytes[i]);
      } catch (const MemoryFault& e) {
        fault_at(address, e.fault_address(), 16u, AccessKind::Write, e.reason(),
                 e.what());
      }
    }
    return;
  }

  std::lock_guard lock(mutex_);
  if (auto contiguous = resolve_contiguous(address, value.bytes.size(), AccessKind::Write);
      contiguous &&
      (reinterpret_cast<std::uintptr_t>(contiguous->ptr) &
       (alignof(std::uint64_t) - 1u)) == 0u) {
    std::uint64_t lo{}, hi{};
    std::memcpy(&lo, value.bytes.data(), sizeof(lo));
    std::memcpy(&hi, value.bytes.data() + 8u, sizeof(hi));
    const auto width = static_cast<std::uint32_t>(value.bytes.size());
    const bool reservation_participant =
        begin_physical_write(contiguous->physical_address, width);
    xenon::cpu::detail::atomic_store_relaxed<std::uint64_t>(
        contiguous->ptr, lo);
    xenon::cpu::detail::atomic_store_relaxed<std::uint64_t>(
        contiguous->ptr + 8u, hi);
    complete_physical_write(contiguous->physical_address, width,
                            reservation_participant, ordering_domain(address));
    return;
  }
  for (std::size_t i = 0; i < value.bytes.size(); ++i) {
    auto resolved = resolve_byte_for_request(
        address + static_cast<GuestAddress>(i), AccessKind::Write, address, 16u);
    const bool reservation_participant =
        begin_physical_write(resolved.physical_address, 1u);
    xenon::cpu::detail::atomic_store_relaxed<std::uint8_t>(
        resolved.ptr, value.bytes[i]);
    complete_physical_write(resolved.physical_address, 1u,
                            reservation_participant,
                            ordering_domain(address + static_cast<GuestAddress>(i)));
  }
}

std::uint16_t AddressSpace::read16_le(GuestAddress a) { return read_integer<std::uint16_t>(a, true); }

std::uint32_t AddressSpace::read32_le(GuestAddress a) { return read_integer<std::uint32_t>(a, true); }

std::uint64_t AddressSpace::read64_le(GuestAddress a) { return read_integer<std::uint64_t>(a, true); }

void AddressSpace::write16_le(GuestAddress a, std::uint16_t v) { write_integer(a, v, true); }

void AddressSpace::write32_le(GuestAddress a, std::uint32_t v) { write_integer(a, v, true); }

void AddressSpace::write64_le(GuestAddress a, std::uint64_t v) { write_integer(a, v, true); }

void AddressSpace::read_bytes(GuestAddress address,
                              std::span<std::byte> destination) {
  if (destination.empty()) return;
  validate_guest_range(address, destination.size(), AccessKind::Read);
  {
    auto access = access_context();
    bool all_fast = destination.size() <=
                    std::numeric_limits<GuestAddress>::max() -
                        std::uint64_t{address} + 1u;
    auto cursor = address;
    std::size_t remaining = destination.size();
    while (all_fast && remaining) {
      const auto page_remaining =
          kBasePageSize - (cursor & (kBasePageSize - 1u));
      const auto chunk = std::min<std::size_t>(remaining, page_remaining);
      xenon::cpu::MemoryAccessContext::PhysicalResolution resolved{};
      all_fast = access.resolve_physical_ram(cursor, chunk, false, 1u, resolved);
      cursor += static_cast<GuestAddress>(chunk);
      remaining -= chunk;
    }
    if (all_fast) {
      access.read_bytes(address, destination);
      return;
    }
  }

  // Exceptional/unaligned/MMIO range fallback. Each byte re-enters the slow
  // resolver independently; MMIO handlers are snapshotted under the management
  // lock and invoked only after it has been released.
  for (std::size_t i = 0; i < destination.size(); ++i) {
    try {
      destination[i] = static_cast<std::byte>(
          read8(address + static_cast<GuestAddress>(i)));
    } catch (const MemoryFault& e) {
      fault_at(address, e.fault_address(), destination.size(), AccessKind::Read,
               e.reason(), e.what());
    }
  }
}

void AddressSpace::write_bytes(GuestAddress address,
                               std::span<const std::byte> source) {
  if (source.empty()) return;
  validate_guest_range(address, source.size(), AccessKind::Write);
  {
    auto access = access_context();
    bool all_fast = source.size() <=
                    std::numeric_limits<GuestAddress>::max() -
                        std::uint64_t{address} + 1u;
    auto cursor = address;
    std::size_t remaining = source.size();
    while (all_fast && remaining) {
      const auto page_remaining =
          kBasePageSize - (cursor & (kBasePageSize - 1u));
      const auto chunk = std::min<std::size_t>(remaining, page_remaining);
      xenon::cpu::MemoryAccessContext::PhysicalResolution resolved{};
      all_fast = access.resolve_physical_ram(cursor, chunk, true, 1u, resolved);
      cursor += static_cast<GuestAddress>(chunk);
      remaining -= chunk;
    }
    if (all_fast) {
      access.write_bytes(address, source);
      return;
    }
  }

  for (std::size_t i = 0; i < source.size(); ++i) {
    try {
      write8(address + static_cast<GuestAddress>(i),
             std::to_integer<std::uint8_t>(source[i]));
    } catch (const MemoryFault& e) {
      fault_at(address, e.fault_address(), source.size(), AccessKind::Write,
               e.reason(), e.what());
    }
  }
}

void AddressSpace::fill_bytes(GuestAddress address, std::uint32_t size,
                              std::uint8_t value) {
  if (!size) return;
  validate_guest_range(address, size, AccessKind::Write);
  {
    auto access = access_context();
    bool all_fast = std::uint64_t{address} + size <=
                    std::uint64_t{std::numeric_limits<GuestAddress>::max()} + 1u;
    auto cursor = address;
    std::uint32_t remaining = size;
    while (all_fast && remaining) {
      const auto page_remaining =
          kBasePageSize - (cursor & (kBasePageSize - 1u));
      const auto chunk = std::min(remaining, page_remaining);
      xenon::cpu::MemoryAccessContext::PhysicalResolution resolved{};
      all_fast = access.resolve_physical_ram(cursor, chunk, true, 1u, resolved);
      cursor += chunk;
      remaining -= chunk;
    }
    if (all_fast) {
      access.fill_bytes(address, size, value);
      return;
    }
  }

  for (std::uint32_t i = 0; i < size; ++i) {
    try {
      write8(address + i, value);
    } catch (const MemoryFault& e) {
      fault_at(address, e.fault_address(), size, AccessKind::Write, e.reason(),
               e.what());
    }
  }
}

bool AddressSpace::begin_physical_write(std::uint32_t physical_address,
                                       std::uint32_t width) noexcept {
  if (!width || physical_address >= kPhysicalMemorySize) return false;
  auto view = make_fast_memory_view();
  return xenon::cpu::reservation_monitor_detail::enter_write(
      view, physical_address, width);
}

std::uint64_t AddressSpace::complete_physical_write(
    std::uint32_t physical_address, std::uint32_t width,
    bool reservation_participant,
    xenon::cpu::MemoryOrderingDomain ordering_domain) noexcept {
  if (!width || physical_address >= kPhysicalMemorySize) return 0u;
  auto view = make_fast_memory_view();
  return xenon::cpu::reservation_monitor_detail::finish_write(
      view, physical_address, width, reservation_participant, ordering_domain);
}

xenon::cpu::MemoryOrderingDomain AddressSpace::ordering_domain(
    GuestAddress address) const noexcept {
  const auto page_index = address >> kPageShift;
  if (page_index < hot_pages_.size()) {
    const auto entry = hot_pages_[page_index].load(std::memory_order_acquire);
    if (entry & xenon::cpu::fast_memory::kNoCache) {
      return xenon::cpu::MemoryOrderingDomain::CacheInhibited;
    }
    if (entry & xenon::cpu::fast_memory::kWriteCombine) {
      return xenon::cpu::MemoryOrderingDomain::WriteCombined;
    }
    if (entry & xenon::cpu::fast_memory::kSlow) {
      if (const auto* region = region_for(address);
          region && region->kind == RegionKind::Mmio) {
        return xenon::cpu::MemoryOrderingDomain::Device;
      }
      // MMIO overlays may live outside the dedicated top-of-address-space
      // region, so consult the cold dispatcher only for pages already marked
      // slow. Ordinary RAM never takes this lock for domain classification.
      if (snapshot_mmio(address, 1u)) {
        return xenon::cpu::MemoryOrderingDomain::Device;
      }
    }
  }
  return xenon::cpu::MemoryOrderingDomain::Normal;
}

void AddressSpace::zero(GuestAddress address, std::uint32_t size) {
  fill(address, size, 0);
}

void AddressSpace::fill(GuestAddress address, std::uint32_t size,
                        std::uint8_t value) {
  if (!size) return;
  validate_guest_range(address, size, AccessKind::Write);
  auto access = access_context();
  access.fill_bytes(address, size, value);
}

void AddressSpace::copy(GuestAddress dest, GuestAddress src, std::uint32_t size) {
  move(dest, src, size);
}

void AddressSpace::move(GuestAddress dest, GuestAddress src, std::uint32_t size) {
  if (!size || dest == src) return;
  validate_guest_range(src, size, AccessKind::Read);
  validate_guest_range(dest, size, AccessKind::Write);

  // Keep a read-side guard alive from mapping inspection through the copy. Cold
  // management may unpublish mappings concurrently, but retired physical pages
  // cannot be recycled until this context leaves.
  auto access = access_context();

  struct RangePlan {
    bool all_ram{true};
    bool linear{true};
    bool has_first{};
    std::uint32_t first_physical{};
  };

  constexpr std::size_t kPhysicalBitmapWords =
      (kPhysicalPageCount + 63u) / 64u;
  std::array<std::uint64_t, kPhysicalBitmapWords> source_pages{};
  RangePlan source_plan{};
  RangePlan destination_plan{};
  bool physical_page_overlap = false;

  auto inspect_source = [&](GuestAddress base, std::uint32_t length) {
    std::uint32_t remaining = length;
    std::uint32_t logical_offset = 0;
    auto cursor = base;
    while (remaining) {
      const auto page_remaining =
          kBasePageSize - (cursor & (kBasePageSize - 1u));
      const auto chunk = std::min(remaining, page_remaining);
      xenon::cpu::MemoryAccessContext::PhysicalResolution resolved{};
      if (!access.resolve_physical_ram(cursor, chunk, false, 1u, resolved)) {
        source_plan.all_ram = false;
        return;
      }
      if (!source_plan.has_first) {
        source_plan.has_first = true;
        source_plan.first_physical = resolved.physical_address;
      } else if (resolved.physical_address !=
                 source_plan.first_physical + logical_offset) {
        source_plan.linear = false;
      }
      const auto physical_page = resolved.physical_address >> kPageShift;
      source_pages[physical_page >> 6u] |=
          std::uint64_t{1} << (physical_page & 63u);
      cursor += chunk;
      logical_offset += chunk;
      remaining -= chunk;
    }
  };

  auto inspect_destination = [&](GuestAddress base, std::uint32_t length) {
    std::uint32_t remaining = length;
    std::uint32_t logical_offset = 0;
    auto cursor = base;
    while (remaining) {
      const auto page_remaining =
          kBasePageSize - (cursor & (kBasePageSize - 1u));
      const auto chunk = std::min(remaining, page_remaining);
      xenon::cpu::MemoryAccessContext::PhysicalResolution resolved{};
      if (!access.resolve_physical_ram(cursor, chunk, true, 1u, resolved)) {
        destination_plan.all_ram = false;
        return;
      }
      if (!destination_plan.has_first) {
        destination_plan.has_first = true;
        destination_plan.first_physical = resolved.physical_address;
      } else if (resolved.physical_address !=
                 destination_plan.first_physical + logical_offset) {
        destination_plan.linear = false;
      }
      const auto physical_page = resolved.physical_address >> kPageShift;
      if (source_pages[physical_page >> 6u] &
          (std::uint64_t{1} << (physical_page & 63u))) {
        physical_page_overlap = true;
      }
      cursor += chunk;
      logical_offset += chunk;
      remaining -= chunk;
    }
  };

  inspect_source(src, size);
  inspect_destination(dest, size);

  if (source_plan.all_ram && destination_plan.all_ram &&
      source_plan.linear && destination_plan.linear) {
    // The common case: both guest ranges map to linear physical RAM. One host
    // memmove preserves arbitrary virtual alias overlap and one range-level
    // reservation/coherency publication covers the destination.
    const bool reservation_participant =
        begin_physical_write(destination_plan.first_physical, size);
    xenon::cpu::detail::atomic_memmove_guest(
        physical_->data() + destination_plan.first_physical,
        physical_->data() + source_plan.first_physical, size);
    complete_physical_write(destination_plan.first_physical, size,
                            reservation_participant, ordering_domain(dest));
    return;
  }

  constexpr std::size_t kCopyScratchSize = 64u * 1024u;
  std::array<std::byte, kCopyScratchSize> scratch{};

  if (source_plan.all_ram && destination_plan.all_ram &&
      !physical_page_overlap) {
    // Non-linear mappings without physical overlap can stream through bounded
    // scratch. This keeps memory use constant regardless of transfer size.
    std::uint32_t offset = 0;
    while (offset < size) {
      const auto chunk = static_cast<std::uint32_t>(
          std::min<std::size_t>(size - offset, scratch.size()));
      auto bytes = std::span<std::byte>(scratch).first(chunk);
      access.read_bytes(src + offset, bytes);
      access.write_bytes(dest + offset, bytes);
      offset += chunk;
    }
    return;
  }

  // MMIO and pathological non-linear physical alias overlap are deliberately
  // slow-path cases. Snapshot all reads before any writes to preserve the V1
  // memmove-like observable ordering and device side effects. Importantly, RAM
  // access inside the snapshot is still page/range based rather than scalar.
  std::vector<std::byte> snapshot(size);
  access.read_bytes(src, snapshot);
  access.write_bytes(dest, snapshot);
}

void AddressSpace::validate_guest_range(GuestAddress address,
                                        std::size_t width,
                                        AccessKind access) const {
  if (!width) return;
  const auto end = std::uint64_t{address} + width;
  if (end > std::uint64_t{std::numeric_limits<GuestAddress>::max()} + 1u) {
    fault_at(address, address, width, access, FaultReason::OutOfRange,
             "guest memory access wraps the 32-bit address space");
  }
}

}  // namespace xenon::memory

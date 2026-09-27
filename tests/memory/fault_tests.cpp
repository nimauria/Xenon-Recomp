#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "xenon/memory/address_space.hpp"

using namespace xenon::memory;

static MemoryFaultInfo capture_fault(auto&& fn) {
  try {
    fn();
  } catch (const MemoryFault& fault) {
    // The compatibility accessors must remain exact views of the structured
    // record so existing tooling doesn't need to migrate atomically.
    assert(fault.address() == fault.info().fault_address);
    assert(fault.request_address() == fault.info().request_address);
    assert(fault.width() == fault.info().width);
    assert(fault.access() == fault.info().access);
    assert(fault.reason() == fault.info().reason);
    return fault.info();
  }
  assert(false && "expected MemoryFault");
  return {};
}

int main() {
  static_assert(std::is_trivially_copyable_v<MemoryFaultInfo>);

  AddressSpace memory;
  assert(memory.initialize());

  // The retail low-memory guard is committed/no-access but intentionally has
  // no backing. Query/fault state must distinguish it from both a reserved
  // allocation and a wholly free page.
  {
    const auto fault = capture_fault([&] { (void)memory.read8(0x00000000u); });
    assert(fault.request_address == 0x00000000u);
    assert(fault.fault_address == 0x00000000u);
    assert(fault.width == 1u);
    assert(fault.access == AccessKind::Read);
    assert(fault.reason == FaultReason::Protection);
    assert(fault.region_present);
    assert(fault.region_kind == RegionKind::Virtual);
    assert(fault.page_state == PageState::Committed);
    assert(!fault.mapped);
    assert(fault.committed);
    assert(fault.physical_address == 0xFFFFFFFFu);
    assert(fault.page_size == kBasePageSize);
  }

  // A free virtual page is distinct from the reserved low-memory guard.
  {
    constexpr GuestAddress kFree = 0x00700000u;
    const auto fault = capture_fault([&] { (void)memory.read8(kFree); });
    assert(fault.reason == FaultReason::Unmapped);
    assert(fault.region_present);
    assert(fault.region_kind == RegionKind::Virtual);
    assert(fault.page_state == PageState::Free);
    assert(!fault.mapped);
    assert(!fault.committed);
  }

  constexpr GuestAddress kPage = 0x00800000u;
  constexpr GuestAddress kNextPage = kPage + kBasePageSize;
  assert(memory.commit_fixed(kPage, kBasePageSize * 2u, kReadWrite));
  memory.write32_be(kPage, 0x11223344u);

  // Protection failures carry both mapping state and the current/allocation
  // protections. Cache type remains part of the captured canonical state.
  assert(memory.protect(kPage, kBasePageSize,
                        Protect::Read | Protect::NoCache));
  {
    const auto fault =
        capture_fault([&] { memory.write32_be(kPage, 0xAABBCCDDu); });
    assert(fault.reason == FaultReason::Protection);
    assert(fault.access == AccessKind::Write);
    assert(fault.mapped && fault.committed);
    assert(fault.page_state == PageState::Committed);
    assert(has(fault.allocation_protect, Protect::Write));
    assert(has(fault.current_protect, Protect::Read));
    assert(!has(fault.current_protect, Protect::Write));
    assert(has(fault.current_protect, Protect::NoCache));
    assert(fault.memory_type == MemoryType::CacheInhibited);
    assert(fault.host_policy == HostMappingPolicy::TranslatedCacheInhibited);
    assert(fault.physical_address != 0xFFFFFFFFu);
  }

  // Execute faults are independent of read permission and are ready for a
  // future kernel exception translator to identify without parsing text.
  {
    const auto fault = capture_fault([&] { (void)memory.fetch32_be(kPage); });
    assert(fault.reason == FaultReason::Protection);
    assert(fault.access == AccessKind::Execute);
    assert(fault.width == 4u);
    assert(fault.mapped && fault.committed);
    assert(!has(fault.current_protect, Protect::Execute));
  }

  // A cross-page scalar access reports the *requested* operation while also
  // pointing at the exact second page that failed. This is essential for a
  // precise future DSI/ISI-style exception translation.
  assert(memory.protect(kPage, kBasePageSize, kReadWrite));
  assert(memory.protect(kNextPage, kBasePageSize, Protect::None));
  {
    constexpr GuestAddress kCross = kNextPage - 2u;
    const auto fault = capture_fault([&] { (void)memory.read32_be(kCross); });
    assert(fault.request_address == kCross);
    assert(fault.fault_address == kNextPage);
    assert(fault.width == 4u);
    assert(fault.reason == FaultReason::Protection);
    assert(fault.access == AccessKind::Read);
    assert(fault.page_state == PageState::Committed);
    assert(fault.mapped && fault.committed);
    assert(fault.current_protect == Protect::None);
  }

  // Guest accesses are 32-bit. Wrapping a multi-byte operation must fault
  // before touching address zero or a device window after integer wraparound.
  {
    const auto fault = capture_fault(
        [&] { (void)memory.read64_be(0xFFFFFFFCu); });
    assert(fault.request_address == 0xFFFFFFFCu);
    assert(fault.fault_address == 0xFFFFFFFCu);
    assert(fault.width == 8u);
    assert(fault.reason == FaultReason::OutOfRange);
  }

  // Dedicated top-of-space MMIO without a registered handler is a device
  // region, but not a mapped device instance.
  {
    const auto fault =
        capture_fault([&] { (void)memory.read32_be(kMmioBase); });
    assert(fault.reason == FaultReason::Unmapped);
    assert(fault.region_present);
    assert(fault.region_kind == RegionKind::Mmio);
    assert(fault.mmio);
    assert(!fault.mapped);
    assert(fault.memory_type == MemoryType::Device);
    assert(fault.host_policy == HostMappingPolicy::DeviceDispatcher);
  }

  // A scalar access that only partially fits a registered device range is not
  // allowed to silently fall through to the underlying RAM page.
  constexpr GuestAddress kOverlay = 0x00900000u;
  assert(memory.commit_fixed(kOverlay, kBasePageSize, kReadWrite));
  assert(memory.add_mmio_range(
      kOverlay + 0x100u, 2u,
      [](GuestAddress, std::uint32_t) -> std::uint64_t { return 0xBEEFu; },
      [](GuestAddress, std::uint32_t, std::uint64_t) {}, "two-byte-device"));
  {
    const auto fault = capture_fault(
        [&] { (void)memory.read32_be(kOverlay + 0x100u); });
    assert(fault.reason == FaultReason::MmioWidth);
    assert(fault.request_address == kOverlay + 0x100u);
    assert(fault.width == 4u);
    assert(fault.mmio);
    assert(fault.mapped && fault.committed);
    assert(fault.region_kind == RegionKind::Virtual);
    assert(fault.memory_type == MemoryType::Device);
    assert(has(fault.current_protect, Protect::Read));
  }

  return 0;
}

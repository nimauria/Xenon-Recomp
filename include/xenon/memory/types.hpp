#pragma once

#include <cstddef>
#include <cstdint>

#include "xenon/cpu/types.hpp"

namespace xenon::memory {

using GuestAddress = xenon::cpu::GuestAddress;

constexpr std::uint32_t kPhysicalMemorySize = 0x20000000u;  // 512 MiB
constexpr std::uint32_t kBasePageSize = 0x1000u;            // 4 KiB
constexpr std::uint32_t kLargePageSize = 0x10000u;          // 64 KiB
constexpr std::uint32_t kHugePageSize = 0x01000000u;        // 16 MiB
constexpr std::uint32_t kReservationGranuleSize = 128u;
constexpr std::uint32_t kPhysicalSystemReserveSize = kHugePageSize;
constexpr std::uint32_t kPhysicalTopReservedSize = kLargePageSize;
constexpr std::uint32_t kPhysicalAllocatableEndExclusive =
    kPhysicalMemorySize - kPhysicalTopReservedSize;

// Xbox 360 guest address-space layout used by Xenon.
constexpr GuestAddress kVirtual4KBase = 0x00000000u;
constexpr GuestAddress kVirtual4KEnd = 0x3FFFFFFFu;
constexpr GuestAddress kVirtual64KBase = 0x40000000u;
constexpr GuestAddress kVirtual64KEnd = 0x7EFFFFFFu;
constexpr GuestAddress kGpuWritebackBase = 0x7F000000u;
constexpr GuestAddress kGpuWritebackEnd = 0x7FFFFFFFu;
constexpr GuestAddress kXex64KBase = 0x80000000u;
constexpr GuestAddress kXex64KEnd = 0x8FFFFFFFu;
constexpr GuestAddress kXex4KBase = 0x90000000u;
constexpr GuestAddress kXex4KEnd = 0x9FFFFFFFu;
constexpr GuestAddress kPhysical64KBase = 0xA0000000u;
constexpr GuestAddress kPhysical64KEnd = 0xBFFFFFFFu;
constexpr GuestAddress kPhysical16MBase = 0xC0000000u;
constexpr GuestAddress kPhysical16MEnd = 0xDFFFFFFFu;
constexpr GuestAddress kPhysical4KBase = 0xE0000000u;
constexpr GuestAddress kPhysical4KHeapEnd = 0xFFCFFFFFu;
constexpr GuestAddress kMmioBase = 0xFFD00000u;
constexpr GuestAddress kMmioEnd = 0xFFFFFFFFu;

// The 0xE... physical view is offset by 4 KiB relative to the other direct
// physical views on the retail memory map.
constexpr std::uint32_t kPhysical4KViewOffset = 0x1000u;
constexpr std::uint32_t kPhysical4KAddressableEndInclusive =
    kPhysical4KViewOffset + (kPhysical4KHeapEnd - kPhysical4KBase);

enum class RegionKind : std::uint8_t {
  Virtual,
  Xex,
  PhysicalAlias,
  GpuWriteback,
  Mmio,
};

enum class AccessKind : std::uint8_t {
  Read,
  Write,
  Execute,
};

enum class FaultReason : std::uint8_t {
  Unmapped,
  Uncommitted,
  Protection,
  OutOfRange,
  MmioWidth,
};

enum class PageState : std::uint8_t {
  Free,
  Reserved,
  Committed,
};

enum class PhysicalPageClass : std::uint8_t {
  Page4K,
  Page64K,
  Page16M,
};

enum class Protect : std::uint8_t {
  None = 0,
  Read = 1u << 0,
  Write = 1u << 1,
  Execute = 1u << 2,
  NoCache = 1u << 3,
  WriteCombine = 1u << 4,
};

constexpr Protect operator|(Protect a, Protect b) noexcept {
  return static_cast<Protect>(static_cast<std::uint8_t>(a) |
                              static_cast<std::uint8_t>(b));
}
constexpr Protect operator&(Protect a, Protect b) noexcept {
  return static_cast<Protect>(static_cast<std::uint8_t>(a) &
                              static_cast<std::uint8_t>(b));
}
constexpr Protect& operator|=(Protect& a, Protect b) noexcept {
  a = a | b;
  return a;
}
constexpr bool has(Protect value, Protect bit) noexcept {
  return static_cast<std::uint8_t>(value & bit) != 0;
}

constexpr Protect kReadWrite = Protect::Read | Protect::Write;
constexpr Protect kReadExecute = Protect::Read | Protect::Execute;
constexpr Protect kReadWriteExecute =
    Protect::Read | Protect::Write | Protect::Execute;

struct VirtualAllocationOptions {
  // Xbox/NT allocation policy is intentionally represented independently of
  // address selection. A caller may reserve address space without committing
  // backing and may request MEM_NOZERO-style commit behavior when appropriate.
  bool commit{true};
  bool zero_initialize{true};
};

struct PhysicalAllocationOptions {
  PhysicalPageClass page_class{PhysicalPageClass::Page4K};
  std::uint32_t alignment{};
  std::uint32_t minimum_address{kPhysicalSystemReserveSize};
  // Inclusive Xbox physical-address bound.
  std::uint32_t maximum_address{kPhysicalAllocatableEndExclusive - 1u};
  Protect protect{kReadWrite};
  // Xbox MmAllocatePhysicalMemoryEx allocates from the high end of the
  // selected physical heap unless a lower-level caller explicitly requests a
  // different policy.
  bool top_down{true};
  bool zero_initialize{true};
};

struct PhysicalAllocationInfo {
  std::uint32_t physical_address{};
  std::uint32_t allocation_base{};
  std::uint32_t allocation_size{};
  std::uint32_t page_size{kBasePageSize};
  PhysicalPageClass page_class{PhysicalPageClass::Page4K};
  Protect allocation_protect{kReadWrite};
  Protect current_protect{kReadWrite};
};

// Canonical Xbox-visible memory type. This is deliberately separate from host
// page-cache attributes: Xenon may translate a guest cache policy into ordering,
// coherency and access-path rules when the host cannot safely create an alias
// with the same native cache attribute.
enum class MemoryType : std::uint8_t {
  NormalCached,
  WriteCombined,
  CacheInhibited,
  Device,
};

// Describes how Xenon represents the guest memory type in host virtual memory.
// The shared 512 MiB backing has many aliases, so changing the native cache type
// of one alias independently can be illegal or unsafe on desktop hosts. WC/CI
// therefore use the compact translation path and explicit ordering/coherency
// semantics unless a future alias-safe backend supplies a native mapping mode.
enum class HostMappingPolicy : std::uint8_t {
  DefaultCachedShared,
  TranslatedWriteCombined,
  TranslatedCacheInhibited,
  DeviceDispatcher,
};

struct MemoryTypeInfo {
  MemoryType type{MemoryType::NormalCached};
  HostMappingPolicy host_policy{HostMappingPolicy::DefaultCachedShared};
  bool mapped{};
  bool executable{};
  bool direct_aperture_eligible{};
  bool explicit_device_visibility{};
};

[[nodiscard]] constexpr bool valid_memory_type_protection(
    Protect protect) noexcept {
  // Xbox PAGE_NOCACHE / PAGE_WRITECOMBINE are distinct cache modes. Mirroring
  // Win32/XDK semantics, they are not a meaningful combined mapping type.
  return !(has(protect, Protect::NoCache) &&
           has(protect, Protect::WriteCombine));
}

struct RegionDescriptor {
  GuestAddress base{};
  GuestAddress end{};
  std::uint32_t allocation_page_size{};
  RegionKind kind{};
};

struct MappingInfo {
  GuestAddress guest_address{};
  std::uint32_t physical_address{};
  std::uint32_t allocation_base{};
  std::uint32_t allocation_size{};
  std::uint32_t region_size{};
  std::uint32_t page_size{};
  PageState state{PageState::Free};
  Protect allocation_protect{Protect::None};
  Protect current_protect{Protect::None};
  RegionKind kind{RegionKind::Virtual};
};

}  // namespace xenon::memory

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

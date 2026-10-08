#pragma once

// Private implementation header shared by the AddressSpace translation units
// under src/memory/guest/. The helpers stay in an unnamed namespace, exactly
// as they were in the original single translation unit, so each including
// .cpp keeps its own internal-linkage copy.

#include "xenon/memory/address_space.hpp"
#include "xenon/memory/host_vm.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace xenon::memory {
namespace {

constexpr std::array<RegionDescriptor, 9> kRegions{{
    {kVirtual4KBase, kVirtual4KEnd, kBasePageSize, RegionKind::Virtual},
    {kVirtual64KBase, kVirtual64KEnd, kLargePageSize, RegionKind::Virtual},
    {kGpuWritebackBase, kGpuWritebackEnd, kLargePageSize, RegionKind::GpuWriteback},
    {kXex64KBase, kXex64KEnd, kLargePageSize, RegionKind::Xex},
    {kXex4KBase, kXex4KEnd, kBasePageSize, RegionKind::Xex},
    {kPhysical64KBase, kPhysical64KEnd, kLargePageSize, RegionKind::PhysicalAlias},
    {kPhysical16MBase, kPhysical16MEnd, kHugePageSize, RegionKind::PhysicalAlias},
    {kPhysical4KBase, kPhysical4KHeapEnd, kBasePageSize, RegionKind::PhysicalAlias},
    {kMmioBase, kMmioEnd, kBasePageSize, RegionKind::Mmio},
}};

constexpr std::uint32_t align_down(std::uint32_t value, std::uint32_t alignment) {
  return value & ~(alignment - 1u);
}
constexpr std::uint32_t align_up(std::uint32_t value, std::uint32_t alignment) {
  const std::uint64_t v = value;
  return static_cast<std::uint32_t>((v + alignment - 1u) & ~(std::uint64_t(alignment) - 1u));
}
constexpr std::uint32_t page_count_for(std::uint32_t size) {
  return (size + kBasePageSize - 1u) / kBasePageSize;
}
constexpr std::uint32_t physical_page_class_size(
    PhysicalPageClass page_class) noexcept {
  switch (page_class) {
    case PhysicalPageClass::Page4K:
      return kBasePageSize;
    case PhysicalPageClass::Page64K:
      return kLargePageSize;
    case PhysicalPageClass::Page16M:
      return kHugePageSize;
  }
  return kBasePageSize;
}

constexpr PhysicalPageClass physical_page_class_from_size(
    std::uint32_t page_size) noexcept {
  return page_size == kHugePageSize
             ? PhysicalPageClass::Page16M
             : page_size == kLargePageSize
                   ? PhysicalPageClass::Page64K
                   : PhysicalPageClass::Page4K;
}
constexpr bool overlaps(std::uint32_t a_base, std::uint32_t a_size,
                        std::uint32_t b_base, std::uint32_t b_size) {
  const std::uint64_t a_end = std::uint64_t(a_base) + a_size;
  const std::uint64_t b_end = std::uint64_t(b_base) + b_size;
  return std::uint64_t(a_base) < b_end && std::uint64_t(b_base) < a_end;
}

template <typename T>
constexpr T byteswap_if(T value, bool do_swap);

constexpr std::uint64_t kReservationTokenSlotMask = 0x7ull;

[[nodiscard]] constexpr std::uint64_t make_reservation_token(
    std::uint32_t slot_index, std::uint32_t generation) noexcept {
  return (std::uint64_t{generation} << 3u) |
         std::uint64_t{slot_index + 1u};
}

[[nodiscard]] constexpr bool decode_reservation_token(
    std::uint64_t token, std::uint32_t& slot_index,
    std::uint32_t& generation) noexcept {
  const auto encoded_slot =
      static_cast<std::uint32_t>(token & kReservationTokenSlotMask);
  if (encoded_slot == 0u || encoded_slot > 6u) return false;
  slot_index = encoded_slot - 1u;
  generation = static_cast<std::uint32_t>(token >> 3u);
  return generation != 0u;
}

template <typename T>
[[nodiscard]] T atomic_load_guest_be(std::byte* ptr) noexcept {
  T raw{};
  if ((reinterpret_cast<std::uintptr_t>(ptr) & (alignof(T) - 1u)) == 0u) {
    auto& object = *reinterpret_cast<T*>(ptr);
    std::atomic_ref<T> atomic(object);
    raw = atomic.load(std::memory_order_relaxed);
  } else {
    auto* bytes = reinterpret_cast<std::uint8_t*>(ptr);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
      std::atomic_ref<std::uint8_t> atomic(bytes[i]);
      raw = static_cast<T>((raw << 8u) |
                           atomic.load(std::memory_order_relaxed));
    }
    return raw;
  }
  return byteswap_if(raw, std::endian::native == std::endian::little);
}

template <typename T>
void atomic_store_guest_be(std::byte* ptr, T value) noexcept {
  if ((reinterpret_cast<std::uintptr_t>(ptr) & (alignof(T) - 1u)) == 0u) {
    const auto raw =
        byteswap_if(value, std::endian::native == std::endian::little);
    auto& object = *reinterpret_cast<T*>(ptr);
    std::atomic_ref<T> atomic(object);
    atomic.store(raw, std::memory_order_relaxed);
    return;
  }
  auto* bytes = reinterpret_cast<std::uint8_t*>(ptr);
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    const auto shift = static_cast<unsigned>((sizeof(T) - 1u - i) * 8u);
    std::atomic_ref<std::uint8_t> atomic(bytes[i]);
    atomic.store(static_cast<std::uint8_t>((value >> shift) & 0xFFu),
                 std::memory_order_relaxed);
  }
}

template <typename T>
constexpr T byteswap_if(T value, bool do_swap) {
  if (!do_swap) return value;
  if constexpr (sizeof(T) == 2) {
    const auto v = static_cast<std::uint16_t>(value);
    return static_cast<T>((v << 8) | (v >> 8));
  }
  else if constexpr (sizeof(T) == 4) {
    const auto v = static_cast<std::uint32_t>(value);
    return static_cast<T>(((v & 0x000000FFu) << 24) |
                          ((v & 0x0000FF00u) << 8) |
                          ((v & 0x00FF0000u) >> 8) |
                          ((v & 0xFF000000u) >> 24));
  }
  else if constexpr (sizeof(T) == 8) {
    const auto v = static_cast<std::uint64_t>(value);
    return static_cast<T>(((v & 0x00000000000000FFull) << 56) |
                          ((v & 0x000000000000FF00ull) << 40) |
                          ((v & 0x0000000000FF0000ull) << 24) |
                          ((v & 0x00000000FF000000ull) << 8) |
                          ((v & 0x000000FF00000000ull) >> 8) |
                          ((v & 0x0000FF0000000000ull) >> 24) |
                          ((v & 0x00FF000000000000ull) >> 40) |
                          ((v & 0xFF00000000000000ull) >> 56));
  }
  else {
    return value;
  }
}

}  // namespace
}  // namespace xenon::memory

#include "memory/guest/backing/physical_backing.hpp"
#include "memory/guest/physical/physical_page_tracking.hpp"

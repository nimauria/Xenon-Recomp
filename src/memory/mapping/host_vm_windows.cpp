#include "xenon/memory/host_vm.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
#ifndef MEM_COALESCE_PLACEHOLDERS
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif

namespace xenon::memory::host_vm {
namespace {

DWORD native_protection(Protection protection) noexcept {
  switch (protection) {
    case Protection::None: return PAGE_NOACCESS;
    case Protection::Read: return PAGE_READONLY;
    case Protection::ReadWrite: return PAGE_READWRITE;
    case Protection::ReadExecute: return PAGE_EXECUTE_READ;
    case Protection::ReadWriteExecute: return PAGE_EXECUTE_READWRITE;
  }
  return PAGE_NOACCESS;
}

DWORD mapping_access(Protection protection) noexcept {
  switch (protection) {
    case Protection::None:
    case Protection::Read: return FILE_MAP_READ;
    case Protection::ReadWrite: return FILE_MAP_READ | FILE_MAP_WRITE;
    case Protection::ReadExecute: return FILE_MAP_READ | FILE_MAP_EXECUTE;
    case Protection::ReadWriteExecute:
      return FILE_MAP_READ | FILE_MAP_WRITE | FILE_MAP_EXECUTE;
  }
  return FILE_MAP_READ;
}

const SYSTEM_INFO& system_info() noexcept {
  static const SYSTEM_INFO cached = [] {
    SYSTEM_INFO info{};
    ::GetSystemInfo(&info);
    return info;
  }();
  return cached;
}

HANDLE native_handle(std::uintptr_t native) noexcept {
  return reinterpret_cast<HANDLE>(native);
}

void close_shared_native(std::uintptr_t native) noexcept {
  if (native) (void)::CloseHandle(native_handle(native));
}

bool range_in_shared_object(const SharedMemory& shared, std::size_t offset,
                            std::size_t size) noexcept {
  return shared.valid() && size && offset <= shared.size() &&
         size <= shared.size() - offset;
}

using VirtualAlloc2Fn = void* (WINAPI*)(HANDLE, void*, SIZE_T, ULONG, ULONG,
                                        void*, ULONG);
using MapViewOfFile3Fn = void* (WINAPI*)(HANDLE, HANDLE, void*, ULONG64, SIZE_T,
                                         ULONG, ULONG, void*, ULONG);
using UnmapViewOfFile2Fn = BOOL (WINAPI*)(HANDLE, void*, ULONG);

struct PlaceholderApi {
  VirtualAlloc2Fn virtual_alloc2{};
  MapViewOfFile3Fn map_view_of_file3{};
  UnmapViewOfFile2Fn unmap_view_of_file2{};

  [[nodiscard]] bool valid() const noexcept {
    return virtual_alloc2 && map_view_of_file3 && unmap_view_of_file2;
  }
};

const PlaceholderApi& placeholder_api() noexcept {
  static const PlaceholderApi api = [] {
    PlaceholderApi result{};
    HMODULE module = ::GetModuleHandleW(L"KernelBase.dll");
    if (!module) module = ::GetModuleHandleW(L"Kernel32.dll");
    if (!module) return result;
    result.virtual_alloc2 = reinterpret_cast<VirtualAlloc2Fn>(
        ::GetProcAddress(module, "VirtualAlloc2"));
    result.map_view_of_file3 = reinterpret_cast<MapViewOfFile3Fn>(
        ::GetProcAddress(module, "MapViewOfFile3"));
    result.unmap_view_of_file2 = reinterpret_cast<UnmapViewOfFile2Fn>(
        ::GetProcAddress(module, "UnmapViewOfFile2"));
    return result;
  }();
  return api;
}

bool page_aligned(const void* address) noexcept {
  return reinterpret_cast<std::uintptr_t>(address) % page_size() == 0u;
}

bool exact_placeholder(void* address, std::size_t size) noexcept {
  if (!address || !size || !page_aligned(address) ||
      size % page_size() != 0u) {
    return false;
  }

  MEMORY_BASIC_INFORMATION info{};
  if (::VirtualQuery(address, &info, sizeof(info)) != sizeof(info) ||
      info.State != MEM_RESERVE) {
    return false;
  }
  const auto target = reinterpret_cast<std::uintptr_t>(address);
  auto region = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
  auto region_end = region + info.RegionSize;
  if (region_end < region || target < region || target + size < target ||
      target + size > region_end) {
    return false;
  }

  // VirtualFree's documented placeholder split operates from the beginning of
  // the placeholder. If the requested 4 KiB slice is in the middle of a larger
  // placeholder, first split off the prefix, then split the target slice from
  // the remainder. This avoids relying on undocumented middle-of-placeholder
  // MEM_RELEASE behavior.
  if (target != region) {
    const auto prefix_size = target - region;
    if (!::VirtualFree(info.BaseAddress, prefix_size,
                       MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
      return false;
    }
    info = {};
    if (::VirtualQuery(address, &info, sizeof(info)) != sizeof(info) ||
        info.State != MEM_RESERVE || info.BaseAddress != address) {
      return false;
    }
    region = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    region_end = region + info.RegionSize;
    if (region_end < region || target + size > region_end) return false;
  }

  if (info.RegionSize == size) return true;
  if (info.RegionSize < size) return false;
  if (!::VirtualFree(address, size,
                     MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
    return false;
  }

  MEMORY_BASIC_INFORMATION exact{};
  return ::VirtualQuery(address, &exact, sizeof(exact)) == sizeof(exact) &&
         exact.State == MEM_RESERVE && exact.BaseAddress == address &&
         exact.RegionSize == size;
}

bool restore_placeholder_range(void* address, std::size_t size) noexcept {
  const auto& api = placeholder_api();
  if (!api.valid() || !address || !size || !page_aligned(address) ||
      size % page_size() != 0u) {
    return false;
  }
  const auto begin = reinterpret_cast<std::uintptr_t>(address);
  const auto end = begin + size;
  if (end < begin) return false;

  auto cursor = begin;
  while (cursor < end) {
    MEMORY_BASIC_INFORMATION info{};
    if (::VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info)) !=
        sizeof(info)) {
      return false;
    }
    const auto region = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const auto region_end = region + info.RegionSize;
    if (region_end <= cursor) return false;
    const auto next = (std::min)(region_end, end);

    if (info.State == MEM_RESERVE) {
      cursor = next;
      continue;
    }
    if (info.Type != MEM_MAPPED ||
        reinterpret_cast<std::uintptr_t>(info.AllocationBase) < begin ||
        region_end > end) {
      return false;
    }
    if (!api.unmap_view_of_file2(::GetCurrentProcess(), info.AllocationBase,
                                 MEM_PRESERVE_PLACEHOLDER)) {
      return false;
    }
    cursor = next;
  }
  return true;
}

void release_placeholder_region(void* address, std::size_t size) noexcept {
  if (!address || !size) return;
  if (!restore_placeholder_range(address, size)) return;

  const auto begin = reinterpret_cast<std::uintptr_t>(address);
  const auto end = begin + size;
  if (end < begin) return;

  // Splitting creates independently addressable adjacent placeholders. Merge
  // from the aperture base until one reservation spans the whole range, then
  // release it in the normal MEM_RELEASE form.
  for (;;) {
    MEMORY_BASIC_INFORMATION first{};
    if (::VirtualQuery(address, &first, sizeof(first)) != sizeof(first) ||
        first.State != MEM_RESERVE || first.BaseAddress != address) {
      return;
    }
    const auto first_end = begin + first.RegionSize;
    if (first_end >= end) {
      (void)::VirtualFree(address, 0, MEM_RELEASE);
      return;
    }

    MEMORY_BASIC_INFORMATION second{};
    if (::VirtualQuery(reinterpret_cast<void*>(first_end), &second,
                       sizeof(second)) != sizeof(second) ||
        second.State != MEM_RESERVE ||
        reinterpret_cast<std::uintptr_t>(second.BaseAddress) != first_end) {
      return;
    }
    const auto combined = first.RegionSize + second.RegionSize;
    if (!::VirtualFree(address, combined,
                       MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) {
      return;
    }
  }
}

}  // namespace

SharedMemory::~SharedMemory() { close_shared_native(native_); }

SharedMemory::SharedMemory(SharedMemory&& other) noexcept
    : native_(other.native_), size_(other.size_) {
  other.native_ = 0;
  other.size_ = 0;
}

SharedMemory& SharedMemory::operator=(SharedMemory&& other) noexcept {
  if (this == &other) return *this;
  close_shared_native(native_);
  native_ = other.native_;
  size_ = other.size_;
  other.native_ = 0;
  other.size_ = 0;
  return *this;
}

std::size_t page_size() noexcept { return system_info().dwPageSize; }

std::size_t allocation_granularity() noexcept {
  return system_info().dwAllocationGranularity;
}

void* reserve(std::size_t size) noexcept {
  if (!size) return nullptr;
  return ::VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
}

bool commit(void* address, std::size_t size, Protection protection) noexcept {
  return address && size &&
         ::VirtualAlloc(address, size, MEM_COMMIT,
                        native_protection(protection)) == address;
}

bool decommit(void* address, std::size_t size) noexcept {
  return address && size && ::VirtualFree(address, size, MEM_DECOMMIT) != FALSE;
}

bool protect(void* address, std::size_t size, Protection protection) noexcept {
  if (!address || !size) return false;
  DWORD old_protection{};
  return ::VirtualProtect(address, size, native_protection(protection),
                          &old_protection) != FALSE;
}

bool discard(void* address, std::size_t size,
             Protection committed_protection) noexcept {
  if (!address || !size) return false;
  if (!::VirtualFree(address, size, MEM_DECOMMIT)) return false;
  return ::VirtualAlloc(address, size, MEM_COMMIT,
                        native_protection(committed_protection)) == address;
}

void release(void* address, std::size_t) noexcept {
  if (address) (void)::VirtualFree(address, 0, MEM_RELEASE);
}

SharedMemory create_shared(std::size_t size) noexcept {
  SharedMemory result;
  if (!size) return result;
  const auto bytes = static_cast<std::uint64_t>(size);
  HANDLE mapping = ::CreateFileMappingW(
      INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE,
      static_cast<DWORD>(bytes >> 32u), static_cast<DWORD>(bytes), nullptr);
  if (!mapping) return result;
  result.native_ = reinterpret_cast<std::uintptr_t>(mapping);
  result.size_ = size;
  return result;
}

void* map_shared(const SharedMemory& shared, std::size_t offset,
                 std::size_t size, Protection protection) noexcept {
  if (!range_in_shared_object(shared, offset, size) ||
      offset % allocation_granularity() != 0u) {
    return nullptr;
  }

  const auto offset64 = static_cast<std::uint64_t>(offset);
  void* result = ::MapViewOfFile(
      native_handle(shared.native_), mapping_access(protection),
      static_cast<DWORD>(offset64 >> 32u), static_cast<DWORD>(offset64), size);
  if (!result) return nullptr;

  // MapViewOfFile's access flags don't include a direct no-access mapping, and
  // VirtualProtect also gives us one consistent way to apply the requested
  // final page protection for all modes.
  DWORD old_protection{};
  if (!::VirtualProtect(result, size, native_protection(protection),
                        &old_protection)) {
    (void)::UnmapViewOfFile(result);
    return nullptr;
  }
  return result;
}

bool unmap(void* address, std::size_t) noexcept {
  return address && ::UnmapViewOfFile(address) != FALSE;
}

bool supports_fixed_shared_mapping() noexcept {
  return placeholder_api().valid();
}

std::size_t fixed_shared_mapping_granularity() noexcept {
  return supports_fixed_shared_mapping() ? page_size() : 0u;
}

void* reserve_fixed_shared_mapping_region(std::size_t size) noexcept {
  const auto& api = placeholder_api();
  if (!api.valid() || !size || size % page_size() != 0u) return nullptr;
  return api.virtual_alloc2(::GetCurrentProcess(), nullptr, size,
                            MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                            PAGE_NOACCESS, nullptr, 0u);
}

void release_fixed_shared_mapping_region(void* address,
                                         std::size_t size) noexcept {
  release_placeholder_region(address, size);
}

bool fixed_shared_mapping_requires_page_views() noexcept { return true; }

bool map_shared_fixed(const SharedMemory& shared, void* address,
                      std::size_t offset, std::size_t size,
                      Protection protection) noexcept {
  const auto& api = placeholder_api();
  if (!api.valid() || !range_in_shared_object(shared, offset, size) ||
      size != page_size() || offset % page_size() != 0u ||
      !page_aligned(address) || !exact_placeholder(address, size)) {
    return false;
  }

  void* result = api.map_view_of_file3(
      native_handle(shared.native_), ::GetCurrentProcess(), address,
      static_cast<ULONG64>(offset), size, MEM_REPLACE_PLACEHOLDER,
      native_protection(protection), nullptr, 0u);
  return result == address;
}

bool restore_reservation(void* address, std::size_t size) noexcept {
  return restore_placeholder_range(address, size);
}

}  // namespace xenon::memory::host_vm

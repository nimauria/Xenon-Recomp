#include "xenon/memory/host_vm.hpp"

#include <limits>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

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

bool supports_fixed_shared_mapping() noexcept { return false; }

bool map_shared_fixed(const SharedMemory&, void*, std::size_t, std::size_t,
                      Protection) noexcept {
  return false;
}

bool restore_reservation(void*, std::size_t) noexcept { return false; }

}  // namespace xenon::memory::host_vm

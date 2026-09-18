#include "xenon/memory/host_vm.hpp"

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

SYSTEM_INFO system_info() noexcept {
  SYSTEM_INFO info{};
  ::GetSystemInfo(&info);
  return info;
}

}  // namespace

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

}  // namespace xenon::memory::host_vm

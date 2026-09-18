#include "xenon/memory/host_vm.hpp"

#include <sys/mman.h>
#include <unistd.h>

namespace xenon::memory::host_vm {
namespace {

int native_protection(Protection protection) noexcept {
  switch (protection) {
    case Protection::None: return PROT_NONE;
    case Protection::Read: return PROT_READ;
    case Protection::ReadWrite: return PROT_READ | PROT_WRITE;
    case Protection::ReadExecute: return PROT_READ | PROT_EXEC;
    case Protection::ReadWriteExecute:
      return PROT_READ | PROT_WRITE | PROT_EXEC;
  }
  return PROT_NONE;
}

}  // namespace

std::size_t page_size() noexcept {
  const long value = ::sysconf(_SC_PAGESIZE);
  return value > 0 ? static_cast<std::size_t>(value) : 4096u;
}

std::size_t allocation_granularity() noexcept { return page_size(); }

void* reserve(std::size_t size) noexcept {
  if (!size) return nullptr;
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_NORESERVE)
  flags |= MAP_NORESERVE;
#endif
  void* result = ::mmap(nullptr, size, PROT_NONE, flags, -1, 0);
  return result == MAP_FAILED ? nullptr : result;
}

bool commit(void* address, std::size_t size, Protection protection) noexcept {
  return address && size &&
         ::mprotect(address, size, native_protection(protection)) == 0;
}

bool decommit(void* address, std::size_t size) noexcept {
  if (!address || !size) return false;
#if defined(MADV_DONTNEED)
  (void)::madvise(address, size, MADV_DONTNEED);
#endif
  return ::mprotect(address, size, PROT_NONE) == 0;
}

bool protect(void* address, std::size_t size, Protection protection) noexcept {
  return address && size &&
         ::mprotect(address, size, native_protection(protection)) == 0;
}

bool discard(void* address, std::size_t size,
             Protection committed_protection) noexcept {
  if (!address || !size) return false;
#if defined(MADV_DONTNEED)
  if (::madvise(address, size, MADV_DONTNEED) == 0) {
    return ::mprotect(address, size,
                      native_protection(committed_protection)) == 0;
  }
#endif
  // Portable POSIX fallback that preserves the reservation and guarantees
  // zero-filled anonymous pages without exposing this policy to AddressSpace.
  if (::mprotect(address, size, PROT_NONE) != 0) return false;
#if defined(MADV_DONTNEED)
  (void)::madvise(address, size, MADV_DONTNEED);
#endif
  return ::mprotect(address, size,
                    native_protection(committed_protection)) == 0;
}

void release(void* address, std::size_t size) noexcept {
  if (address && size) (void)::munmap(address, size);
}

}  // namespace xenon::memory::host_vm

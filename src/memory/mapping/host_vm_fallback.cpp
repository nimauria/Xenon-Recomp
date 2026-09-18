#include "xenon/memory/host_vm.hpp"

#include <cstring>
#include <new>

namespace xenon::memory::host_vm {

std::size_t page_size() noexcept { return 4096u; }
std::size_t allocation_granularity() noexcept { return 4096u; }

void* reserve(std::size_t size) noexcept {
  return size ? static_cast<void*>(new (std::nothrow) std::byte[size]) : nullptr;
}

bool commit(void* address, std::size_t size, Protection) noexcept {
  return address && size;
}

bool decommit(void* address, std::size_t size) noexcept {
  if (!address || !size) return false;
  std::memset(address, 0, size);
  return true;
}

bool protect(void* address, std::size_t size, Protection) noexcept {
  return address && size;
}

bool discard(void* address, std::size_t size, Protection) noexcept {
  if (!address || !size) return false;
  std::memset(address, 0, size);
  return true;
}

void release(void* address, std::size_t) noexcept {
  delete[] static_cast<std::byte*>(address);
}

}  // namespace xenon::memory::host_vm

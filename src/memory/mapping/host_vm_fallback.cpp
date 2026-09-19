#include "xenon/memory/host_vm.hpp"

#include <cstring>
#include <new>

namespace xenon::memory::host_vm {
namespace {

void close_shared_native(std::uintptr_t native) noexcept {
  delete[] reinterpret_cast<std::byte*>(native);
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

SharedMemory create_shared(std::size_t size) noexcept {
  SharedMemory result;
  if (!size) return result;
  auto* bytes = new (std::nothrow) std::byte[size]{};
  if (!bytes) return result;
  result.native_ = reinterpret_cast<std::uintptr_t>(bytes);
  result.size_ = size;
  return result;
}

void* map_shared(const SharedMemory& shared, std::size_t offset,
                 std::size_t size, Protection) noexcept {
  if (!range_in_shared_object(shared, offset, size)) return nullptr;
  return reinterpret_cast<std::byte*>(shared.native_) + offset;
}

bool unmap(void* address, std::size_t) noexcept {
  // The fallback has no independent VM mappings. Views alias the backing
  // allocation directly and are invalidated when SharedMemory is destroyed.
  return address != nullptr;
}

bool supports_fixed_shared_mapping() noexcept { return false; }

void* reserve_fixed_shared_mapping_region(std::size_t) noexcept {
  return nullptr;
}

void release_fixed_shared_mapping_region(void*, std::size_t) noexcept {}

bool fixed_shared_mapping_requires_page_views() noexcept { return true; }

bool map_shared_fixed(const SharedMemory&, void*, std::size_t, std::size_t,
                      Protection) noexcept {
  return false;
}

bool restore_reservation(void*, std::size_t) noexcept { return false; }

}  // namespace xenon::memory::host_vm

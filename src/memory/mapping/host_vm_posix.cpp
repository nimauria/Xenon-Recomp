#include "xenon/memory/host_vm.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <limits>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

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

constexpr int anonymous_mapping_flag() noexcept {
#if defined(MAP_ANONYMOUS)
  return MAP_ANONYMOUS;
#elif defined(MAP_ANON)
  return MAP_ANON;
#else
  return 0;
#endif
}

bool range_in_shared_object(const SharedMemory& shared, std::size_t offset,
                            std::size_t size) noexcept {
  return shared.valid() && size && offset <= shared.size() &&
         size <= shared.size() - offset;
}

int create_shared_fd(std::size_t size) noexcept {
#if defined(__linux__) && defined(SYS_memfd_create)
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
  const int memfd = static_cast<int>(
      ::syscall(SYS_memfd_create, "xenon-memory", MFD_CLOEXEC));
  if (memfd >= 0) {
    if (::ftruncate(memfd, static_cast<off_t>(size)) == 0) return memfd;
    ::close(memfd);
  }
#endif

  // Portable POSIX fallback. Unlink immediately so lifetime is tied solely to
  // the open descriptor and no persistent namespace entry remains.
  static std::atomic<std::uint64_t> sequence{1};
  for (unsigned attempt = 0; attempt < 32; ++attempt) {
    char name[96]{};
    const auto serial = sequence.fetch_add(1, std::memory_order_relaxed);
    const int written = std::snprintf(
        name, sizeof(name), "/xenon-memory-%ld-%llu",
        static_cast<long>(::getpid()),
        static_cast<unsigned long long>(serial));
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(name)) {
      return -1;
    }
    const int fd = ::shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
      if (errno == EEXIST) continue;
      return -1;
    }
    (void)::shm_unlink(name);
    if (::ftruncate(fd, static_cast<off_t>(size)) == 0) return fd;
    ::close(fd);
    return -1;
  }
  return -1;
}

void close_shared_native(std::uintptr_t native) noexcept {
  if (!native) return;
  const auto encoded = native - 1u;
  if (encoded <= static_cast<std::uintptr_t>(std::numeric_limits<int>::max())) {
    (void)::close(static_cast<int>(encoded));
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

std::size_t page_size() noexcept {
  static const std::size_t cached = [] {
    const long value = ::sysconf(_SC_PAGESIZE);
    return value > 0 ? static_cast<std::size_t>(value) : 4096u;
  }();
  return cached;
}

std::size_t allocation_granularity() noexcept { return page_size(); }

void* reserve(std::size_t size) noexcept {
  if (!size || anonymous_mapping_flag() == 0) return nullptr;
  int flags = MAP_PRIVATE | anonymous_mapping_flag();
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
  if (!address || !size || anonymous_mapping_flag() == 0) return false;
#if defined(MADV_DONTNEED)
  if (::madvise(address, size, MADV_DONTNEED) == 0) {
    return ::mprotect(address, size, PROT_NONE) == 0;
  }
#endif
  // Replacing this slice of the anonymous reservation guarantees that a later
  // commit observes zero-filled pages even on hosts where MADV_DONTNEED is not
  // available or fails. MAP_FIXED is intentionally contained in this host VM
  // backend and is never exposed as Xbox policy.
  void* result = ::mmap(address, size, PROT_NONE,
                        MAP_PRIVATE | anonymous_mapping_flag() | MAP_FIXED, -1,
                        0);
  return result == address;
}

bool protect(void* address, std::size_t size, Protection protection) noexcept {
  return address && size &&
         ::mprotect(address, size, native_protection(protection)) == 0;
}

bool discard(void* address, std::size_t size,
             Protection committed_protection) noexcept {
  if (!address || !size || anonymous_mapping_flag() == 0) return false;
#if defined(MADV_DONTNEED)
  if (::madvise(address, size, MADV_DONTNEED) == 0) {
    return ::mprotect(address, size,
                      native_protection(committed_protection)) == 0;
  }
#endif
  void* result = ::mmap(address, size,
                        native_protection(committed_protection),
                        MAP_PRIVATE | anonymous_mapping_flag() | MAP_FIXED, -1,
                        0);
  return result == address;
}

void release(void* address, std::size_t size) noexcept {
  if (address && size) (void)::munmap(address, size);
}

SharedMemory create_shared(std::size_t size) noexcept {
  SharedMemory result;
  if (!size || size > static_cast<std::size_t>(
                          std::numeric_limits<off_t>::max())) {
    return result;
  }
  const int fd = create_shared_fd(size);
  if (fd < 0) return result;
  // Encode fd + 1 because descriptor 0 is valid while zero is our invalid
  // opaque token.
  result.native_ = static_cast<std::uintptr_t>(fd) + 1u;
  result.size_ = size;
  return result;
}

void* map_shared(const SharedMemory& shared, std::size_t offset,
                 std::size_t size, Protection protection) noexcept {
  if (!range_in_shared_object(shared, offset, size) ||
      offset % allocation_granularity() != 0u ||
      offset > static_cast<std::size_t>(std::numeric_limits<off_t>::max())) {
    return nullptr;
  }
  const auto encoded = shared.native_ - 1u;
  if (encoded > static_cast<std::uintptr_t>(std::numeric_limits<int>::max())) {
    return nullptr;
  }
  void* result = ::mmap(nullptr, size, native_protection(protection), MAP_SHARED,
                        static_cast<int>(encoded), static_cast<off_t>(offset));
  return result == MAP_FAILED ? nullptr : result;
}

bool unmap(void* address, std::size_t size) noexcept {
  return address && size && ::munmap(address, size) == 0;
}

bool supports_fixed_shared_mapping() noexcept {
#if defined(__SANITIZE_THREAD__)
  // TSan tracks memory identity by host virtual address and does not model two
  // MAP_SHARED aliases of the same bytes as one synchronization domain. Keep
  // sanitizer builds on the compact path rather than reporting misleading
  // alias races or paying the cost of instrumenting a 4 GiB aperture.
  return false;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
  return false;
#else
  return anonymous_mapping_flag() != 0;
#endif
#else
  return anonymous_mapping_flag() != 0;
#endif
}

std::size_t fixed_shared_mapping_granularity() noexcept {
  return supports_fixed_shared_mapping() ? page_size() : 0u;
}

void* reserve_fixed_shared_mapping_region(std::size_t size) noexcept {
  return supports_fixed_shared_mapping() ? reserve(size) : nullptr;
}

void release_fixed_shared_mapping_region(void* address,
                                         std::size_t size) noexcept {
  release(address, size);
}

bool fixed_shared_mapping_requires_page_views() noexcept { return false; }

bool map_shared_fixed(const SharedMemory& shared, void* address,
                      std::size_t offset, std::size_t size,
                      Protection protection) noexcept {
  if (!address || !supports_fixed_shared_mapping() ||
      !range_in_shared_object(shared, offset, size) ||
      offset % allocation_granularity() != 0u ||
      reinterpret_cast<std::uintptr_t>(address) % page_size() != 0u ||
      size % page_size() != 0u ||
      offset > static_cast<std::size_t>(std::numeric_limits<off_t>::max())) {
    return false;
  }
  const auto encoded = shared.native_ - 1u;
  if (encoded > static_cast<std::uintptr_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  void* result = ::mmap(address, size, native_protection(protection),
                        MAP_SHARED | MAP_FIXED, static_cast<int>(encoded),
                        static_cast<off_t>(offset));
  return result == address;
}

bool restore_reservation(void* address, std::size_t size) noexcept {
  if (!address || !size || !supports_fixed_shared_mapping() ||
      reinterpret_cast<std::uintptr_t>(address) % page_size() != 0u ||
      size % page_size() != 0u) {
    return false;
  }
  void* result = ::mmap(address, size, PROT_NONE,
                        MAP_PRIVATE | anonymous_mapping_flag() | MAP_FIXED, -1,
                        0);
  return result == address;
}

}  // namespace xenon::memory::host_vm

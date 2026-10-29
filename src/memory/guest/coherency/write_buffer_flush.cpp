#include "xenon/memory/coherency.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <linux/membarrier.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace xenon::memory {

#if defined(_WIN32)

void flush_process_write_buffers() noexcept { FlushProcessWriteBuffers(); }

#else

namespace {

// Changing a page's protection makes the kernel shoot down that page's TLB
// entry on every CPU running this process, which interrupts each of them and
// drains its store buffer. It is the portable equivalent of membarrier.
void flush_by_protection_change() noexcept {
  static void* page = [] {
    void* p = mmap(nullptr, static_cast<std::size_t>(sysconf(_SC_PAGESIZE)),
                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
  }();
  if (!page) {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return;
  }
  const auto size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
  // The page must be touched while writable for the shootdown to reach every
  // CPU that may have it cached.
  *static_cast<volatile char*>(page) = 1;
  mprotect(page, size, PROT_READ);
  mprotect(page, size, PROT_READ | PROT_WRITE);
}

}  // namespace

void flush_process_write_buffers() noexcept {
#if defined(__linux__)
  static const bool expedited = [] {
    return syscall(SYS_membarrier, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0, 0) == 0;
  }();
  if (expedited &&
      syscall(SYS_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0) == 0) {
    return;
  }
#endif
  flush_by_protection_change();
}

#endif

}  // namespace xenon::memory

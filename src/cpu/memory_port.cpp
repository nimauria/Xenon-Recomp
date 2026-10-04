#include "xenon/cpu/memory_port.hpp"

#include <cstdio>
#include <mutex>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace xenon::cpu {

// See the declaration's comment in memory_port.hpp for why these are not
// defined inline in the header.
void ReservationCommitGate::acquire() noexcept {
  const auto my_ticket = next_ticket_.fetch_add(1u, std::memory_order_relaxed);
  while (now_serving_.load(std::memory_order_acquire) != my_ticket) {
    std::this_thread::yield();
  }
}

void ReservationCommitGate::release() noexcept {
  now_serving_.fetch_add(1u, std::memory_order_release);
}

// Defined here, in xenon_cpu alongside its declaration, so every xenon_cpu
// consumer links (it used to live in xenon_memory, leaving xenon_cpu-only
// targets such as xenon_cpu_tests with an unresolved external).
void debug_signal_write_trap(GuestAddress address, std::uint64_t value) {
#if defined(_WIN32)
  void* frames[24] = {};
  const USHORT captured = CaptureStackBackTrace(0, 24, frames, nullptr);

  static std::mutex trap_mutex;
  std::lock_guard<std::mutex> lock(trap_mutex);

  static bool sym_initialized = false;
  const HANDLE process = GetCurrentProcess();
  if (!sym_initialized) {
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);
    sym_initialized = true;
  }

  if (FILE* d = std::fopen("signal_write_trap_diag.log", "a")) {
    std::fprintf(d, "WRITE to guest 0x%08X value=0x%08X (thread os_tid=%lu):\n",
                 (unsigned)address, (unsigned)value, GetCurrentThreadId());
    char symbol_buffer[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    for (USHORT i = 0; i < captured; ++i) {
      const auto addr = reinterpret_cast<DWORD64>(frames[i]);
      DWORD64 displacement = 0;
      if (SymFromAddr(process, addr, &displacement, symbol)) {
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD line_disp = 0;
        if (SymGetLineFromAddr64(process, addr, &line_disp, &line)) {
          std::fprintf(d, "  [%u] %s+0x%llX (%s:%lu)\n", i, symbol->Name,
                       (unsigned long long)displacement, line.FileName, line.LineNumber);
        } else {
          std::fprintf(d, "  [%u] %s+0x%llX\n", i, symbol->Name,
                       (unsigned long long)displacement);
        }
      } else {
        std::fprintf(d, "  [%u] 0x%p (unresolved)\n", i, frames[i]);
      }
    }
    std::fclose(d);
  }
#else
  (void)address;
  (void)value;
#endif
}


}  // namespace xenon::cpu

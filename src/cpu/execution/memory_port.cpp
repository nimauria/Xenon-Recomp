#include "xenon/cpu/memory_port.hpp"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <span>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

#include "xenon/cpu/state.hpp"
#include "xenon/logging/probe_log.hpp"

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

void set_write_traps(std::span<const GuestAddress> addresses) noexcept {
  const auto count = std::min(addresses.size(), kMaxWriteTraps);
  write_trap_detail::trap_count.store(0, std::memory_order_relaxed);
  for (std::size_t i = 0; i < count; ++i)
    write_trap_detail::trap_addresses[i].store(addresses[i], std::memory_order_relaxed);
  write_trap_detail::trap_count.store(static_cast<std::uint32_t>(count),
                                      std::memory_order_release);
}

namespace {

// XENON_WRITE_TRAP=<hex>[,<hex>...], read once at startup.
[[maybe_unused]] const bool g_write_traps_from_environment = [] {
  const char* value = std::getenv("XENON_WRITE_TRAP");
  if (!value) return false;
  std::vector<GuestAddress> addresses;
  for (const char* cursor = value; *cursor != '\0';) {
    char* end = nullptr;
    const auto parsed = std::strtoul(cursor, &end, 16);
    if (end == cursor) break;
    addresses.push_back(static_cast<GuestAddress>(parsed));
    cursor = *end == ',' ? end + 1 : end;
    if (*end != ',') break;
  }
  set_write_traps(addresses);
  return !addresses.empty();
}();

}  // namespace

namespace {
thread_local const CpuState* t_write_trap_state = nullptr;
}  // namespace

const CpuState* set_write_trap_guest_state(const CpuState* state) noexcept {
  const auto* previous = t_write_trap_state;
  t_write_trap_state = state;
  return previous;
}

// Defined here, in xenon_cpu alongside its declaration, so every xenon_cpu
// consumer links (it used to live in xenon_memory, leaving xenon_cpu-only
// targets such as xenon_cpu_tests with an unresolved external).
void debug_signal_write_trap(GuestAddress address, std::uint64_t value) {
  constexpr const char* kFile = "signal_write_trap_diag.log";
  if (!logging::probe_logs_enabled()) return;
#if defined(_WIN32)
  void* frames[24] = {};
  const USHORT captured = CaptureStackBackTrace(0, 24, frames, nullptr);

  // DbgHelp is single-threaded.
  static std::mutex trap_mutex;
  std::lock_guard<std::mutex> lock(trap_mutex);

  static bool sym_initialized = false;
  const HANDLE process = GetCurrentProcess();
  if (!sym_initialized) {
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);
    sym_initialized = true;
  }

  logging::append_probe_log(kFile, "WRITE to guest 0x%08X value=0x%08X (thread os_tid=%lu):\n",
                            (unsigned)address, (unsigned)value, GetCurrentThreadId());
  if (const auto* guest = t_write_trap_state) {
    logging::append_probe_log(kFile, "  guest cia=0x%08X lr=0x%08X r1=0x%08X r3=0x%08X\n",
                              static_cast<unsigned>(guest->cia), static_cast<unsigned>(guest->lr),
                              static_cast<unsigned>(guest->gpr[1]),
                              static_cast<unsigned>(guest->gpr[3]));
  }
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
        logging::append_probe_log(kFile, "  [%u] %s+0x%llX (%s:%lu)\n", i, symbol->Name,
                                  (unsigned long long)displacement, line.FileName,
                                  line.LineNumber);
      } else {
        logging::append_probe_log(kFile, "  [%u] %s+0x%llX\n", i, symbol->Name,
                                  (unsigned long long)displacement);
      }
    } else {
      logging::append_probe_log(kFile, "  [%u] 0x%p (unresolved)\n", i, frames[i]);
    }
  }
#else
  // No host stack capture off Windows; the store itself is still reported.
  logging::append_probe_log(kFile, "WRITE to guest 0x%08X value=0x%08X\n", (unsigned)address,
                            (unsigned)value);
  if (const auto* guest = t_write_trap_state) {
    logging::append_probe_log(kFile, "  guest cia=0x%08X lr=0x%08X\n",
                              static_cast<unsigned>(guest->cia), static_cast<unsigned>(guest->lr));
  }
#endif
}


}  // namespace xenon::cpu

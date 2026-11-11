#include "xenon/cpu/memory_port.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <iterator>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <share.h>
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

// XENON_WRITE_WATCH=<physical hex>[:<length hex>], read once at startup.
[[maybe_unused]] const bool g_write_watch_from_environment = [] {
  const char* value = std::getenv("XENON_WRITE_WATCH");
  if (!value) return false;
  char* end = nullptr;
  const auto base = std::strtoul(value, &end, 16);
  if (end == value) return false;
  const auto length = *end == ':' ? std::strtoul(end + 1, nullptr, 16) : 16ul;
  set_write_watch(static_cast<std::uint32_t>(base & 0x1FFFFFFFu),
                  static_cast<std::uint32_t>(length));
  return length != 0;
}();

thread_local const CpuState* t_write_trap_state = nullptr;

// Reports from the trap and the watch are written one at a time, and DbgHelp
// is single-threaded.
std::mutex g_report_mutex;

// The storing thread's guest registers and its host stack, which names the
// translated function; called with g_report_mutex held.
using Append = void (*)(const char* file, const char* format, ...);

// The watch's own log: written whenever a watch is set, without the
// investigation probes (XENON_PROBE_LOGS), whose cost slows a whole run.
// It goes beside them (probe_log_directory()), up to 8 MiB.
void append_watch_log(const char* file, const char* format, ...) {
  static std::FILE* out = [&] {
    const auto directory = logging::probe_log_directory();
    const auto path = directory.empty() ? std::string(file) : directory + "/" + file;
    std::FILE* opened = nullptr;
#if defined(_WIN32)
    // Shared, so the log can be read while the title runs (fopen_s would
    // deny every other reader until exit).
    opened = _fsopen(path.c_str(), "wb", _SH_DENYNO);
#else
    opened = std::fopen(path.c_str(), "wb");
#endif
    return opened;
  }();
  static std::size_t written = 0;
  if (!out || written >= (std::size_t{8} << 20)) return;
  char line[1024];
  va_list arguments;
  va_start(arguments, format);
  const int length = std::vsnprintf(line, sizeof(line), format, arguments);
  va_end(arguments);
  if (length <= 0) return;
  written += std::fwrite(line, 1, (std::min)(std::size_t(length), sizeof(line) - 1), out);
  std::fflush(out);
}

// The guest page table reports read the guest stack through.
struct GuestView {
  std::atomic<std::byte*> physical_base{};
  std::atomic<const std::atomic<std::uint64_t>*> page_table{};
  std::uint32_t page_count{};
  std::uint32_t page_shift{12};
};
GuestView g_guest_view;

void append_store_context(Append append, const char* file) {
  if (const auto* guest = t_write_trap_state) {
    append(file, "  guest cia=0x%08X lr=0x%08X r1=0x%08X r3=0x%08X\n",
                              static_cast<unsigned>(guest->cia), static_cast<unsigned>(guest->lr),
                              static_cast<unsigned>(guest->gpr[1]),
                              static_cast<unsigned>(guest->gpr[3]));
    // Every GPR: a copy loop's source pointer is in one of them.
    for (unsigned row = 0; row < 32u; row += 8u) {
      const auto* r = &guest->gpr[row];
      append(file, "  r%-2u %08X %08X %08X %08X %08X %08X %08X %08X\n", row,
             unsigned(r[0]), unsigned(r[1]), unsigned(r[2]), unsigned(r[3]), unsigned(r[4]),
             unsigned(r[5]), unsigned(r[6]), unsigned(r[7]));
    }
    GuestAddress callers[16]{};
    const auto count = write_watch_guest_backtrace(static_cast<GuestAddress>(guest->gpr[1]),
                                                   callers, std::size(callers));
    if (count != 0u) {
      char line[16 * 9 + 1] = {};
      std::size_t used = 0;
      for (std::size_t i = 0; i < count; ++i)
        used += static_cast<std::size_t>(
            std::snprintf(line + used, sizeof(line) - used, " %08X", unsigned(callers[i])));
      append(file, "  guest stack:%s\n", line);
    }
  }
#if defined(_WIN32)
  void* frames[24] = {};
  const USHORT captured = CaptureStackBackTrace(1, 24, frames, nullptr);
  static bool sym_initialized = false;
  const HANDLE process = GetCurrentProcess();
  if (!sym_initialized) {
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);
    sym_initialized = true;
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
        append(file, "  [%u] %s+0x%llX (%s:%lu)\n", i, symbol->Name,
                                  (unsigned long long)displacement, line.FileName,
                                  line.LineNumber);
      } else {
        append(file, "  [%u] %s+0x%llX\n", i, symbol->Name,
                                  (unsigned long long)displacement);
      }
    } else {
      append(file, "  [%u] 0x%p (unresolved)\n", i, frames[i]);
    }
  }
#endif
}

unsigned long current_thread_id() noexcept {
#if defined(_WIN32)
  return GetCurrentThreadId();
#else
  return 0;
#endif
}

}  // namespace

void set_write_watch_page_table(std::byte* physical_base,
                                const std::atomic<std::uint64_t>* page_table,
                                std::uint32_t page_count, std::uint32_t page_shift) noexcept {
  g_guest_view.page_count = page_count;
  g_guest_view.page_shift = page_shift;
  g_guest_view.physical_base.store(physical_base, std::memory_order_relaxed);
  g_guest_view.page_table.store(page_table, std::memory_order_release);
}

void release_write_watch_page_table(const std::atomic<std::uint64_t>* page_table) noexcept {
  auto* expected = page_table;
  g_guest_view.page_table.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
}

bool write_watch_read_guest32(GuestAddress address, std::uint32_t& value) noexcept {
  const auto* table = g_guest_view.page_table.load(std::memory_order_acquire);
  auto* physical = g_guest_view.physical_base.load(std::memory_order_relaxed);
  if (!table || !physical || (address & 3u) != 0u) return false;
  const auto shift = g_guest_view.page_shift;
  const auto index = address >> shift;
  if (index >= g_guest_view.page_count) return false;
  const auto entry = table[index].load(std::memory_order_acquire);
  using namespace fast_memory;
  if ((entry & (kMapped | kRead | kSlow)) != (kMapped | kRead)) return false;
  const auto offset = address & ((1u << shift) - 1u);
  std::uint32_t raw{};
  std::memcpy(&raw, physical + ((std::size_t{entry & kPhysicalPageMask} << shift) + offset),
              sizeof(raw));
  if constexpr (std::endian::native == std::endian::little)
    raw = (raw >> 24u) | ((raw >> 8u) & 0xFF00u) | ((raw << 8u) & 0xFF0000u) | (raw << 24u);
  value = raw;
  return true;
}

std::size_t write_watch_guest_backtrace(GuestAddress r1, GuestAddress* out,
                                        std::size_t capacity) noexcept {
  std::size_t count = 0;
  GuestAddress frame = r1;
  while (count < capacity) {
    std::uint32_t caller_frame = 0;
    std::uint32_t saved_lr = 0;
    // Stacks grow down: each caller's frame lies above, within one stack.
    if (!write_watch_read_guest32(frame, caller_frame) || caller_frame <= frame ||
        caller_frame - frame > 0x100000u ||
        !write_watch_read_guest32(caller_frame - 8u, saved_lr) || saved_lr == 0u)
      break;
    out[count++] = saved_lr;
    frame = caller_frame;
  }
  return count;
}

void set_write_watch(std::uint32_t physical, std::uint32_t length) noexcept {
  write_watch_detail::length.store(0, std::memory_order_relaxed);
  write_watch_detail::base.store(physical, std::memory_order_relaxed);
  write_watch_detail::length.store(length, std::memory_order_release);
}

#if defined(_WIN32)
namespace {

// The host pages that hold the watched range, in the physical view and in
// the guest aperture's physical aliases. Separately compiled game code
// inlines its stores, so the watch reaches it by write-protecting these pages:
// a store faults, is reported, and runs once with the page writable under a
// single step that protects the page again.
// The page list is filled once, before `count` publishes it to the handler
// and the protecting thread.
constexpr std::uint32_t kMaxGuardedWatchPages = 16;  // 64 KiB of the range.
struct WatchGuard {
  // Each watched page in the physical view, its three physical aliases and
  // a virtual alias.
  std::array<std::byte*, kMaxGuardedWatchPages * 5u> pages{};
  std::atomic<std::size_t> count{};
  std::byte* physical_base{};
  std::byte* aperture_base{};
  // A virtual alias of the range (page aligned), guarded with it; 0 for none.
  std::uint64_t alias_virtual{};
  std::uint32_t alias_physical{};
};
WatchGuard g_watch_guard;
thread_local std::byte* t_guard_page = nullptr;
thread_local std::byte* t_guard_store = nullptr;
thread_local std::uint32_t t_guard_physical = 0;
constexpr std::size_t kGuardPageBytes = 4096;

std::uint32_t guarded_physical(std::byte* host) noexcept {
  const auto& guard = g_watch_guard;
  if (host >= guard.physical_base && host < guard.physical_base + 0x20000000u)
    return static_cast<std::uint32_t>(host - guard.physical_base);
  if (guard.aperture_base && host >= guard.aperture_base) {
    const auto guest = static_cast<std::uint64_t>(host - guard.aperture_base);
    if (guard.alias_virtual && guest >= guard.alias_virtual &&
        guest < guard.alias_virtual + kMaxGuardedWatchPages * kGuardPageBytes)
      return static_cast<std::uint32_t>(guard.alias_physical + (guest - guard.alias_virtual));
    const auto offset = guest >= 0xE0000000ull ? 0x1000ull : 0ull;
    return static_cast<std::uint32_t>(((guest & 0x1FFFFFFFull) + offset) & 0x1FFFFFFFull);
  }
  return 0;
}

LONG CALLBACK write_watch_guard_handler(EXCEPTION_POINTERS* info) {
  const auto* record = info->ExceptionRecord;
  if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2 &&
      record->ExceptionInformation[0] == 1) {
    auto* host = reinterpret_cast<std::byte*>(record->ExceptionInformation[1]);
    auto* page = reinterpret_cast<std::byte*>(reinterpret_cast<std::uintptr_t>(host) &
                                              ~std::uintptr_t{kGuardPageBytes - 1});
    const auto& guard = g_watch_guard;
    const auto count = guard.count.load(std::memory_order_acquire);
    if (std::find(guard.pages.begin(), guard.pages.begin() + count, page) ==
        guard.pages.begin() + count)
      return EXCEPTION_CONTINUE_SEARCH;
    DWORD previous{};
    if (!VirtualProtect(page, kGuardPageBytes, PAGE_READWRITE, &previous))
      return EXCEPTION_CONTINUE_SEARCH;
    t_guard_page = page;
    t_guard_store = host;
    t_guard_physical = guarded_physical(host);
    info->ContextRecord->EFlags |= 0x100u;  // trap after the store
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (record->ExceptionCode == EXCEPTION_SINGLE_STEP && t_guard_page) {
    if (write_watch_hit(t_guard_physical, 16u)) {
      // The store has run: report what it left, with the stack of the code
      // that made it (the handler runs on the storing thread's stack).
      debug_signal_write_watch(0, t_guard_physical, 16u, t_guard_store, 16u);
    }
    DWORD previous{};
    (void)VirtualProtect(t_guard_page, kGuardPageBytes, PAGE_READONLY, &previous);
    t_guard_page = nullptr;
    info->ContextRecord->EFlags &= ~DWORD{0x100u};
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

void protect_guarded_pages() noexcept {
  const auto count = g_watch_guard.count.load(std::memory_order_acquire);
  for (std::size_t i = 0; i < count; ++i) {
    DWORD previous{};
    (void)VirtualProtect(g_watch_guard.pages[i], kGuardPageBytes, PAGE_READONLY, &previous);
  }
}

// Guards the pages of the watched range (its first 64 KiB), installs the
// handler and starts the protecting thread. Runs once per process: the
// caller holds g_guard_mutex and has checked that nothing is guarded yet.
void guard_watched_range() noexcept {
  auto& guard = g_watch_guard;
  const auto length = write_watch_detail::length.load(std::memory_order_acquire);
  const auto base = write_watch_detail::base.load(std::memory_order_relaxed);
  std::size_t count = 0;
  const auto add = [&](std::byte* host) {
    auto* page = reinterpret_cast<std::byte*>(reinterpret_cast<std::uintptr_t>(host) &
                                              ~std::uintptr_t{kGuardPageBytes - 1});
    if (count < guard.pages.size() &&
        std::find(guard.pages.begin(), guard.pages.begin() + count, page) ==
            guard.pages.begin() + count)
      guard.pages[count++] = page;
  };
  const auto last = std::uint64_t{base} +
                    (std::min)(length, kMaxGuardedWatchPages * std::uint32_t(kGuardPageBytes)) -
                    1u;
  for (std::uint64_t physical = base & ~std::uint32_t(kGuardPageBytes - 1); physical <= last;
       physical += kGuardPageBytes) {
    add(guard.physical_base + physical);
    if (guard.aperture_base && guard.alias_virtual)
      add(guard.aperture_base + guard.alias_virtual + (physical - guard.alias_physical));
    if (guard.aperture_base) {
      add(guard.aperture_base + 0xA0000000ull + physical);
      add(guard.aperture_base + 0xC0000000ull + physical);
      if (physical >= 0x1000ull) add(guard.aperture_base + 0xE0000000ull + physical - 0x1000ull);
    }
  }
  guard.count.store(count, std::memory_order_release);
  AddVectoredExceptionHandler(1, write_watch_guard_handler);
  protect_guarded_pages();
  // Aperture pages are mapped as the guest first touches them, so the
  // protection is applied again every half second.
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      protect_guarded_pages();
    }
  }).detach();
}

std::mutex g_guard_mutex;

}  // namespace
#endif

void guard_write_watch_pages(std::byte* physical_base, std::byte* aperture_base) noexcept {
#if defined(_WIN32)
  std::lock_guard<std::mutex> lock(g_guard_mutex);
  auto& guard = g_watch_guard;
  if (!physical_base || guard.physical_base) return;
  guard.physical_base = physical_base;
  guard.aperture_base = aperture_base;
  if (write_watch_detail::length.load(std::memory_order_acquire) != 0u) guard_watched_range();
#else
  (void)physical_base;
  (void)aperture_base;
#endif
}

bool arm_write_watch(std::uint32_t physical, std::uint32_t length, GuestAddress alias) noexcept {
  if (length == 0u) return false;
  physical &= 0x1FFFFFFFu;
#if defined(_WIN32)
  std::lock_guard<std::mutex> lock(g_guard_mutex);
  if (g_watch_guard.count.load(std::memory_order_acquire) != 0u) return false;
  if (alias != 0u && alias < 0xA0000000u) {
    // Page-aligned pair; the physical page offset matches the virtual one.
    g_watch_guard.alias_virtual = alias & ~GuestAddress(kGuardPageBytes - 1);
    g_watch_guard.alias_physical = physical & ~std::uint32_t(kGuardPageBytes - 1);
  }
  set_write_watch(physical, length);
  if (g_watch_guard.physical_base) guard_watched_range();
#else
  (void)alias;
  set_write_watch(physical, length);
#endif
  return true;
}

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
  std::lock_guard<std::mutex> lock(g_report_mutex);
  logging::append_probe_log(kFile, "WRITE to guest 0x%08X value=0x%08X (thread os_tid=%lu):\n",
                            (unsigned)address, (unsigned)value, current_thread_id());
  append_store_context(&logging::append_probe_log, kFile);
}

std::uint32_t write_watch_report_limit() noexcept {
  static const std::uint32_t limit = [] {
    const char* value = std::getenv("XENON_WRITE_WATCH_REPORTS");
    if (!value || !*value) return kMaxWriteWatchReports;
    char* end = nullptr;
    const auto parsed = std::strtoul(value, &end, 10);
    return end != value && *end == '\0' ? static_cast<std::uint32_t>(parsed)
                                         : kMaxWriteWatchReports;
  }();
  return limit;
}

void debug_signal_write_watch(GuestAddress address, std::uint32_t physical, std::uint32_t size,
                              const void* data, std::uint32_t data_size, bool fill) {
  constexpr const char* kFile = "write_watch_diag.log";
  static std::atomic<std::uint32_t> reports{0};
  if (reports.fetch_add(1u, std::memory_order_relaxed) >= write_watch_report_limit()) return;
  char bytes[40] = {};
  std::size_t length = 0;
  const auto* source = static_cast<const unsigned char*>(data);
  for (std::uint32_t i = 0; i < (std::min)(data_size, 16u); ++i)
    length += static_cast<std::size_t>(
        std::snprintf(bytes + length, sizeof(bytes) - length, "%02X", unsigned(source[i])));
  std::lock_guard<std::mutex> lock(g_report_mutex);
  append_watch_log(kFile,
                            "%s of %u bytes at guest 0x%08X (physical 0x%08X), %s %s "
                            "(thread os_tid=%lu):\n",
                            fill ? "FILL" : "STORE", size, static_cast<unsigned>(address),
                            static_cast<unsigned>(physical), fill ? "byte" : "data", bytes,
                            current_thread_id());
  append_store_context(&append_watch_log, kFile);
}

}  // namespace xenon::cpu

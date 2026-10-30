#include "crash_report.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#endif

namespace xenon::runtime_host {
namespace {

// Everything the handlers touch lives here, prepared at installation, so a
// crash on an exhausted stack or a corrupted heap still gets its line out.
#if defined(_WIN32)
wchar_t g_path[1024]{};
char g_module_path[MAX_PATH]{};
// The filter installed before ours (the C runtime's), which turns an uncaught
// C++ exception into std::terminate.
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;
constexpr DWORD kCppExceptionCode = 0xE06D7363u;
#else
char g_path[4096]{};
alignas(16) char g_alt_stack[64 * 1024]{};
#endif
char g_line[1024]{};
std::atomic<bool> g_installed{false};
std::atomic_flag g_writing = ATOMIC_FLAG_INIT;

class LineBuilder {
 public:
  LineBuilder(char* out, std::size_t capacity) noexcept : out_(out), capacity_(capacity) {}
  void text(std::string_view s) noexcept {
    for (const char c : s) put(c);
  }
  void decimal(std::uint64_t value, int min_digits = 1) noexcept {
    char digits[20];
    int n = 0;
    do {
      digits[n++] = static_cast<char>('0' + value % 10u);
      value /= 10u;
    } while (value && n < 20);
    while (n < min_digits && n < 20) digits[n++] = '0';
    while (n) put(digits[--n]);
  }
  void hex(std::uint64_t value) noexcept {
    text("0x");
    char digits[16];
    int n = 0;
    do {
      digits[n++] = "0123456789ABCDEF"[value & 0xFu];
      value >>= 4u;
    } while (value);
    while (n) put(digits[--n]);
  }
  // Leaves room for the final newline.
  std::size_t finish() noexcept {
    if (capacity_ == 0u) return 0u;
    if (length_ >= capacity_) length_ = capacity_ - 1u;
    out_[length_++] = '\n';
    return length_;
  }

 private:
  void put(char c) noexcept {
    if (capacity_ && length_ + 1u < capacity_) out_[length_++] = c;
  }
  char* out_;
  std::size_t capacity_;
  std::size_t length_{};
};

// Days since 1970-01-01 to a proleptic Gregorian date (Howard Hinnant's
// civil_from_days), so the stamp needs no C library time call.
void civil_from_days(std::int64_t days, std::int64_t& year, unsigned& month, unsigned& day) noexcept {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const auto doe = static_cast<unsigned>(days - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  day = doy - (153 * mp + 2) / 5 + 1;
  month = mp < 10 ? mp + 3 : mp - 9;
  year = static_cast<std::int64_t>(yoe) + era * 400 + (month <= 2 ? 1 : 0);
}

// "2026-10-30T20:15:02.123Z", as the logger stamps its error files.
void append_stamp(LineBuilder& line, std::int64_t unix_seconds,
                  std::uint32_t milliseconds) noexcept {
  std::int64_t days = unix_seconds / 86400;
  std::int64_t second_of_day = unix_seconds % 86400;
  if (second_of_day < 0) {
    second_of_day += 86400;
    --days;
  }
  std::int64_t year = 0;
  unsigned month = 0, day = 0;
  civil_from_days(days, year, month, day);
  line.decimal(static_cast<std::uint64_t>(year), 4);
  line.text("-");
  line.decimal(month, 2);
  line.text("-");
  line.decimal(day, 2);
  line.text("T");
  line.decimal(static_cast<std::uint64_t>(second_of_day / 3600), 2);
  line.text(":");
  line.decimal(static_cast<std::uint64_t>(second_of_day / 60 % 60), 2);
  line.text(":");
  line.decimal(static_cast<std::uint64_t>(second_of_day % 60), 2);
  line.text(".");
  line.decimal(milliseconds % 1000u, 3);
  line.text("Z");
}

void append_line(const char* data, std::size_t length) noexcept {
  if (!g_installed.load(std::memory_order_acquire) || length == 0u) return;
#if defined(_WIN32)
  const HANDLE file = CreateFileW(g_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  WriteFile(file, data, static_cast<DWORD>(length), &written, nullptr);
  CloseHandle(file);
#else
  const int file = ::open(g_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (file < 0) return;
  [[maybe_unused]] const auto written = ::write(file, data, length);
  ::close(file);
#endif
}

void now_utc(std::int64_t& seconds, std::uint32_t& millis) noexcept {
  const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count();
  seconds = ms / 1000;
  millis = static_cast<std::uint32_t>(ms % 1000);
}

#if defined(_WIN32)
LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* pointers) {
  // An uncaught C++ exception goes on to std::terminate, whose handler
  // records it with its message.
  if (pointers && pointers->ExceptionRecord &&
      pointers->ExceptionRecord->ExceptionCode == kCppExceptionCode && g_previous_filter) {
    return g_previous_filter(pointers);
  }
  if (pointers && pointers->ExceptionRecord) {
    const auto& record = *pointers->ExceptionRecord;
    CrashInfo info{};
    info.code = record.ExceptionCode;
    info.address = reinterpret_cast<std::uint64_t>(record.ExceptionAddress);
    info.thread_id = GetCurrentThreadId();
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(record.ExceptionAddress), &module) &&
        module) {
      const DWORD length = GetModuleFileNameA(module, g_module_path, MAX_PATH);
      std::string_view path(g_module_path, length);
      const auto slash = path.find_last_of("\\/");
      info.module = slash == std::string_view::npos ? path : path.substr(slash + 1u);
      info.module_offset = info.address - reinterpret_cast<std::uint64_t>(module);
    }
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2u) {
      info.has_access = true;
      info.access_kind = static_cast<std::uint32_t>(record.ExceptionInformation[0]);
      info.access_address = record.ExceptionInformation[1];
    }
    write_crash_report(info);
  }
  // Let the process end with the exception's own code, as it would have.
  return EXCEPTION_CONTINUE_SEARCH;
}
#else
void fatal_signal_handler(int signal_number, siginfo_t* signal_info, void*) {
  CrashInfo info{};
  info.code = static_cast<std::uint32_t>(signal_number);
  info.is_signal = true;
  info.thread_id = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
      reinterpret_cast<void*>(pthread_self())));
  if (signal_info && (signal_number == SIGSEGV || signal_number == SIGBUS)) {
    info.has_access = true;
    info.access_address = reinterpret_cast<std::uint64_t>(signal_info->si_addr);
  }
  write_crash_report(info);
  // Re-raise with the default action so the exit status still names it.
  std::signal(signal_number, SIG_DFL);
  ::raise(signal_number);
}
#endif

[[noreturn]] void terminate_handler() {
  // An uncaught C++ exception: name it when there is one to name.
  static char line[1024];
  std::string_view what = "no active exception";
  if (const auto current = std::current_exception()) {
    try {
      std::rethrow_exception(current);
    } catch (const std::exception& error) {
      what = error.what();
    } catch (...) {
      what = "a non-standard exception";
    }
  }
  std::int64_t seconds = 0;
  std::uint32_t millis = 0;
  now_utc(seconds, millis);
  LineBuilder builder(line, sizeof(line));
  append_stamp(builder, seconds, millis);
  builder.text(" fatal: host process terminated: std::terminate (");
  builder.text(what);
  builder.text(")");
  const auto length = builder.finish();
  if (!g_writing.test_and_set()) {
    append_line(line, length);
    g_writing.clear();
  }
  std::abort();
}

}  // namespace

std::string_view crash_code_name(std::uint32_t code, bool is_signal) noexcept {
  if (is_signal) {
#if !defined(_WIN32)
    switch (code) {
      case SIGSEGV: return "segmentation fault";
      case SIGBUS: return "bus error";
      case SIGILL: return "illegal instruction";
      case SIGFPE: return "arithmetic exception";
      case SIGABRT: return "abort";
      default: return {};
    }
#else
    return {};
#endif
  }
  switch (code) {
    case 0xC00000FDu: return "stack overflow";
    case 0xC0000005u: return "access violation";
    case 0xC0000006u: return "in-page error";
    case 0xC000001Du: return "illegal instruction";
    case 0xC0000094u: return "integer divide by zero";
    case 0xC0000095u: return "integer overflow";
    case 0xC0000096u: return "privileged instruction";
    case 0xC0000409u: return "stack buffer overrun";
    case 0xC0000374u: return "heap corruption";
    case 0xC000008Eu: return "floating-point divide by zero";
    case 0xE06D7363u: return "unhandled C++ exception";
    case 0x80000003u: return "breakpoint";
    default: return {};
  }
}

std::size_t format_crash_line(char* out, std::size_t capacity, const CrashInfo& info,
                              std::int64_t unix_seconds, std::uint32_t milliseconds) noexcept {
  LineBuilder line(out, capacity);
  append_stamp(line, unix_seconds, milliseconds);
  line.text(" fatal: host process crashed: ");
  if (info.is_signal) {
    line.text("signal ");
    line.decimal(info.code);
  } else {
    line.text("exception ");
    line.hex(info.code);
  }
  if (const auto name = crash_code_name(info.code, info.is_signal); !name.empty()) {
    line.text(" (");
    line.text(name);
    line.text(")");
  }
  if (info.address) {
    line.text(" at ");
    line.hex(info.address);
    if (!info.module.empty()) {
      line.text(" (");
      line.text(info.module);
      line.text("+");
      line.hex(info.module_offset);
      line.text(")");
    }
  }
  if (info.has_access) {
    line.text(info.access_kind == 1u ? ", writing " : info.access_kind == 8u ? ", executing " : ", reading ");
    line.hex(info.access_address);
  }
  line.text(" on host thread ");
  line.decimal(info.thread_id);
  return line.finish();
}

void write_crash_report(const CrashInfo& info) noexcept {
  // One report at a time; a second thread crashing meanwhile is not written
  // over the first.
  if (g_writing.test_and_set()) return;
  std::int64_t seconds = 0;
  std::uint32_t millis = 0;
  now_utc(seconds, millis);
  const auto length = format_crash_line(g_line, sizeof(g_line), info, seconds, millis);
  append_line(g_line, length);
  g_writing.clear();
}

bool install_crash_report(const std::filesystem::path& run_dir) {
  if (run_dir.empty()) return false;
  const auto path = run_dir / "crash.log";
#if defined(_WIN32)
  const auto& wide = path.native();
  if (wide.size() + 1u > std::size(g_path)) return false;
  std::memcpy(g_path, wide.c_str(), (wide.size() + 1u) * sizeof(wchar_t));
  g_installed.store(true, std::memory_order_release);
  const auto previous = SetUnhandledExceptionFilter(&unhandled_exception_filter);
  if (previous != &unhandled_exception_filter) g_previous_filter = previous;
  // Room for the filter on the main thread after a stack overflow.
  ULONG guarantee = 64u * 1024u;
  SetThreadStackGuarantee(&guarantee);
#else
  const auto narrow = path.string();
  if (narrow.size() + 1u > sizeof(g_path)) return false;
  std::memcpy(g_path, narrow.c_str(), narrow.size() + 1u);
  g_installed.store(true, std::memory_order_release);
  stack_t alternate{};
  alternate.ss_sp = g_alt_stack;
  alternate.ss_size = sizeof(g_alt_stack);
  sigaltstack(&alternate, nullptr);
  struct sigaction action{};
  action.sa_sigaction = &fatal_signal_handler;
  action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
  sigemptyset(&action.sa_mask);
  for (const int signal_number : {SIGSEGV, SIGBUS, SIGILL, SIGFPE}) {
    sigaction(signal_number, &action, nullptr);
  }
#endif
  std::set_terminate(&terminate_handler);
  return true;
}

std::filesystem::path crash_report_path() {
  if (!g_installed.load(std::memory_order_acquire)) return {};
#if defined(_WIN32)
  return std::filesystem::path(std::wstring(g_path));
#else
  return std::filesystem::path(std::string(g_path));
#endif
}

}  // namespace xenon::runtime_host

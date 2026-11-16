#include "xenon/logging/host_sampler.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "xenon/logging/probe_log.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h first: these depend on its types.
#include <dbghelp.h>
#include <tlhelp32.h>
#pragma comment(lib, "dbghelp.lib")
#define XENON_HOST_SAMPLER_WINDOWS 1
#elif defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#include <cxxabi.h>
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#define XENON_HOST_SAMPLER_LINUX 1
#endif

namespace xenon::logging {

struct HostSampler::State {
  struct Thread {
    std::uint64_t id{};
    std::string name;
#if defined(XENON_HOST_SAMPLER_WINDOWS)
    HANDLE handle{};
#endif
    std::uint64_t samples{};
    std::uint64_t running{};
    std::uint64_t last_cpu{};
    bool have_cpu{};
    std::uint64_t cpu_ns{};
    bool alive{true};
    std::unordered_map<std::uintptr_t, std::uint64_t> self;
    std::unordered_map<std::uintptr_t, std::uint64_t> inclusive;
  };

  HostSamplerOptions options{};
  // A sample counts as running when the thread's CPU counter advanced by
  // more than this since the previous one (see calibrate_running_threshold).
  std::uint64_t running_threshold{1};
  std::atomic<bool> running{false};
  std::atomic<std::uint64_t> total{0};
  std::thread worker;
  mutable std::mutex mutex;  // threads, while the worker runs
  std::map<std::uint64_t, Thread> threads;

  void run();
};

namespace {

#if defined(XENON_HOST_SAMPLER_WINDOWS)

std::uint64_t filetime_ns(const FILETIME& time) {
  return ((std::uint64_t{time.dwHighDateTime} << 32u) | time.dwLowDateTime) * 100u;
}

std::uint64_t thread_cpu_ns(HANDLE handle) {
  FILETIME creation{}, exit{}, kernel{}, user{};
  if (!GetThreadTimes(handle, &creation, &exit, &kernel, &user)) return 0;
  return filetime_ns(kernel) + filetime_ns(user);
}

std::string thread_name(HANDLE handle) {
  PWSTR wide = nullptr;
  std::string name;
  if (SUCCEEDED(GetThreadDescription(handle, &wide)) && wide) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (bytes > 1) {
      name.resize(static_cast<std::size_t>(bytes - 1));
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, name.data(), bytes, nullptr, nullptr);
    }
    LocalFree(wide);
  }
  return name;
}

// Walks a suspended thread's stack from `context`. Runs with no allocation
// and under SEH: a frame that does not unwind ends the walk.
std::size_t unwind_stack(CONTEXT& context, std::uintptr_t* out, std::size_t max) noexcept {
  std::size_t count = 0;
#if defined(_M_X64)
  __try {
    while (count < max) {
      const DWORD64 pc = context.Rip;
      if (!pc) break;
      // A return address points after its call: attribute it to the call.
      out[count] = static_cast<std::uintptr_t>(count ? pc - 1u : pc);
      ++count;
      DWORD64 image_base = 0;
      const PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(pc, &image_base, nullptr);
      if (!function) {
        // A leaf function: its return address is at the stack pointer.
        if (!context.Rsp) break;
        context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
        context.Rsp += 8u;
      } else {
        PVOID handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, pc, function, &context, &handler_data,
                         &establisher, nullptr);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
#elif defined(_M_ARM64)
  if (max && context.Pc) out[count++] = static_cast<std::uintptr_t>(context.Pc);
#endif
  return count;
}

std::size_t capture_thread(HANDLE handle, std::uintptr_t* out, std::size_t max) noexcept {
  if (SuspendThread(handle) == static_cast<DWORD>(-1)) return 0;
  CONTEXT context{};
  context.ContextFlags = CONTEXT_FULL;
  std::size_t count = 0;
  if (GetThreadContext(handle, &context)) count = unwind_stack(context, out, max);
  ResumeThread(handle);
  return count;
}

#elif defined(XENON_HOST_SAMPLER_LINUX)

std::atomic<std::uintptr_t> g_signal_pc{0};
std::atomic<std::uint32_t> g_signal_sequence{0};

void sample_signal_handler(int, siginfo_t*, void* raw_context) {
  const auto* context = static_cast<const ucontext_t*>(raw_context);
#if defined(__x86_64__)
  const auto pc = static_cast<std::uintptr_t>(context->uc_mcontext.gregs[REG_RIP]);
#else
  const auto pc = static_cast<std::uintptr_t>(context->uc_mcontext.pc);
#endif
  g_signal_pc.store(pc, std::memory_order_relaxed);
  g_signal_sequence.fetch_add(1u, std::memory_order_release);
}

int sample_signal() { return SIGRTMIN + 3; }

std::string read_small_file(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return {};
  char buffer[512];
  const auto length = std::fread(buffer, 1, sizeof(buffer) - 1, file);
  std::fclose(file);
  std::string text(buffer, length);
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
  return text;
}

// Nanoseconds on CPU (the first field of schedstat).
std::uint64_t task_cpu_ns(std::uint64_t tid) {
  const auto text = read_small_file("/proc/self/task/" + std::to_string(tid) + "/schedstat");
  return text.empty() ? 0u : std::strtoull(text.c_str(), nullptr, 10);
}

#endif

}  // namespace

void HostSampler::State::run() {
  constexpr std::size_t kFrameCapacity = 128;
  std::uintptr_t frames[kFrameCapacity];
  const std::size_t max_frames = std::clamp<std::size_t>(options.max_frames, 1u, kFrameCapacity);
  auto next_refresh = std::chrono::steady_clock::time_point{};
#if defined(XENON_HOST_SAMPLER_WINDOWS)
  const DWORD self_id = GetCurrentThreadId();
  const DWORD process_id = GetCurrentProcessId();
#elif defined(XENON_HOST_SAMPLER_LINUX)
  const auto self_id = static_cast<std::uint64_t>(syscall(SYS_gettid));
  const auto process_id = getpid();
#endif
  std::unordered_set<std::uintptr_t> seen;
#if defined(XENON_HOST_SAMPLER_WINDOWS)
  // sleep_for rounds up to the 15.6 ms system tick unless the process raised
  // the timer resolution; a high-resolution waitable timer keeps the interval.
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
#endif
  const auto wait_interval = [&] {
#if defined(XENON_HOST_SAMPLER_WINDOWS)
    if (timer) {
      LARGE_INTEGER due{};
      due.QuadPart = -static_cast<LONGLONG>(options.interval.count()) * 10;  // 100 ns units
      if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(timer, INFINITE);
        return;
      }
    }
#endif
    std::this_thread::sleep_for(options.interval);
  };
  while (running.load(std::memory_order_acquire)) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_refresh) {
      next_refresh = now + std::chrono::milliseconds(250);
      std::unordered_set<std::uint64_t> live;
#if defined(XENON_HOST_SAMPLER_WINDOWS)
      const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      if (snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL more = Thread32First(snapshot, &entry); more;
             more = Thread32Next(snapshot, &entry)) {
          if (entry.th32OwnerProcessID != process_id || entry.th32ThreadID == self_id) continue;
          live.insert(entry.th32ThreadID);
        }
        CloseHandle(snapshot);
      }
#elif defined(XENON_HOST_SAMPLER_LINUX)
      if (DIR* tasks = opendir("/proc/self/task")) {
        while (const dirent* task = readdir(tasks)) {
          if (task->d_name[0] < '0' || task->d_name[0] > '9') continue;
          const auto tid = std::strtoull(task->d_name, nullptr, 10);
          if (tid != self_id) live.insert(tid);
        }
        closedir(tasks);
      }
#endif
      std::scoped_lock lock(mutex);
      for (auto& [id, thread] : threads) {
        if (!thread.alive || live.contains(id)) continue;
        thread.alive = false;
#if defined(XENON_HOST_SAMPLER_WINDOWS)
        thread.cpu_ns = thread_cpu_ns(thread.handle);
        CloseHandle(thread.handle);
        thread.handle = nullptr;
#endif
      }
      for (const auto id : live) {
        auto found = threads.find(id);
        if (found != threads.end() && found->second.alive) continue;
        if (found != threads.end()) threads.erase(found);  // a reused id
        Thread thread{};
        thread.id = id;
#if defined(XENON_HOST_SAMPLER_WINDOWS)
        thread.handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                       THREAD_QUERY_INFORMATION,
                                   FALSE, static_cast<DWORD>(id));
        if (!thread.handle) continue;
        thread.name = thread_name(thread.handle);
#elif defined(XENON_HOST_SAMPLER_LINUX)
        thread.name = read_small_file("/proc/self/task/" + std::to_string(id) + "/comm");
#endif
        threads.emplace(id, std::move(thread));
      }
    }

    {
      std::scoped_lock lock(mutex);
      for (auto& [id, thread] : threads) {
        if (!thread.alive) continue;
        std::size_t count = 0;
        std::uint64_t cpu = 0;
#if defined(XENON_HOST_SAMPLER_WINDOWS)
        ULONG64 cycles = 0;
        if (QueryThreadCycleTime(thread.handle, &cycles)) cpu = cycles;
        count = capture_thread(thread.handle, frames, max_frames);
#elif defined(XENON_HOST_SAMPLER_LINUX)
        cpu = task_cpu_ns(id);
        const auto before = g_signal_sequence.load(std::memory_order_acquire);
        if (syscall(SYS_tgkill, process_id, static_cast<pid_t>(id), sample_signal()) == 0) {
          const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
          while (g_signal_sequence.load(std::memory_order_acquire) == before &&
                 std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
          if (g_signal_sequence.load(std::memory_order_acquire) != before) {
            frames[0] = g_signal_pc.load(std::memory_order_relaxed);
            count = 1;
          }
        }
#endif
        if (!count) continue;
        ++thread.samples;
        if (thread.have_cpu && cpu - thread.last_cpu > running_threshold) ++thread.running;
        thread.have_cpu = true;
        thread.last_cpu = cpu;
        ++thread.self[frames[0]];
        // Each address once per sample, whatever recursion repeats it.
        seen.clear();
        for (std::size_t i = 0; i < count; ++i)
          if (seen.insert(frames[i]).second) ++thread.inclusive[frames[i]];
      }
    }
    total.fetch_add(1u, std::memory_order_relaxed);
    wait_interval();
  }
#if defined(XENON_HOST_SAMPLER_WINDOWS)
  if (timer) CloseHandle(timer);
#endif
  std::scoped_lock lock(mutex);
  for (auto& [id, thread] : threads) {
    if (!thread.alive) continue;
#if defined(XENON_HOST_SAMPLER_WINDOWS)
    thread.cpu_ns = thread_cpu_ns(thread.handle);
    CloseHandle(thread.handle);
    thread.handle = nullptr;
#elif defined(XENON_HOST_SAMPLER_LINUX)
    thread.cpu_ns = task_cpu_ns(id);
#endif
    thread.alive = false;
  }
}

namespace {

// The CPU-counter advance that marks a sample as one the thread ran in: a
// tenth of the interval. Windows counts cycles (QueryThreadCycleTime), and
// suspending a waiting thread runs a kernel APC on it, so a blocked thread's
// counter moves a little at every sample; Linux counts nanoseconds.
std::uint64_t calibrate_running_threshold(std::chrono::microseconds interval) {
  const auto tenth_us = static_cast<std::uint64_t>((std::max)(interval.count() / 10, 1ll));
#if defined(XENON_HOST_SAMPLER_WINDOWS)
  // Cycles per microsecond of this thread's counter, measured while it spins.
  ULONG64 cycles_start = 0, cycles_end = 0;
  const auto start = std::chrono::steady_clock::now();
  QueryThreadCycleTime(GetCurrentThread(), &cycles_start);
  while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(10)) {
  }
  QueryThreadCycleTime(GetCurrentThread(), &cycles_end);
  const auto elapsed_us = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start)
          .count());
  const auto per_us = elapsed_us ? (cycles_end - cycles_start) / elapsed_us : 1000u;
  return tenth_us * (std::max<std::uint64_t>)(per_us, 1u);
#else
  return tenth_us * 1000u;
#endif
}

}  // namespace

HostSampler::HostSampler() : state_(std::make_unique<State>()) {}

HostSampler::~HostSampler() { stop(); }

bool HostSampler::supported() noexcept {
#if defined(XENON_HOST_SAMPLER_WINDOWS) || defined(XENON_HOST_SAMPLER_LINUX)
  return true;
#else
  return false;
#endif
}

bool HostSampler::start(const HostSamplerOptions& options) {
  if (!supported() || state_->running.load()) return false;
  if (state_->worker.joinable()) state_->worker.join();
#if defined(XENON_HOST_SAMPLER_LINUX)
  struct sigaction action {};
  action.sa_sigaction = sample_signal_handler;
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&action.sa_mask);
  if (sigaction(sample_signal(), &action, nullptr) != 0) return false;
#endif
  {
    std::scoped_lock lock(state_->mutex);
    state_->threads.clear();
  }
  state_->options = options;
  if (state_->options.interval < std::chrono::microseconds(100))
    state_->options.interval = std::chrono::microseconds(100);
  state_->total.store(0u);
  state_->running_threshold = calibrate_running_threshold(state_->options.interval);
  state_->running.store(true, std::memory_order_release);
  state_->worker = std::thread([state = state_.get()] {
#if defined(XENON_HOST_SAMPLER_WINDOWS)
    (void)SetThreadDescription(GetCurrentThread(), L"Xenon host sampler");
    // The sampler must run on time even while the threads it samples are
    // busy, or it samples only the moments they yield.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
    state->run();
  });
  return true;
}

void HostSampler::stop() noexcept {
  if (!state_) return;
  state_->running.store(false, std::memory_order_release);
  if (state_->worker.joinable()) state_->worker.join();
}

bool HostSampler::running() const noexcept { return state_->running.load(); }

std::uint64_t HostSampler::samples() const noexcept { return state_->total.load(); }

std::string HostSampler::symbolize(std::uintptr_t address) {
  char text[512];
#if defined(XENON_HOST_SAMPLER_WINDOWS)
  static std::mutex dbghelp_mutex;  // DbgHelp is single-threaded
  static bool initialized = false;
  std::scoped_lock lock(dbghelp_mutex);
  const HANDLE process = GetCurrentProcess();
  if (!initialized) {
    SymSetOptions(SymGetOptions() | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    // Another diagnostic may have initialized DbgHelp already; then only its
    // module list needs to catch up with modules loaded since.
    (void)SymInitialize(process, nullptr, TRUE);
    initialized = true;
  }
  static auto last_refresh = std::chrono::steady_clock::time_point{};
  if (std::chrono::steady_clock::now() - last_refresh > std::chrono::seconds(1)) {
    (void)SymRefreshModuleList(process);
    last_refresh = std::chrono::steady_clock::now();
  }
  alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 400];
  auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
  std::memset(buffer, 0, sizeof(buffer));
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = 400;
  DWORD64 displacement = 0;
  if (SymFromAddr(process, address, &displacement, symbol)) return symbol->Name;
  HMODULE module = nullptr;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(address), &module) &&
      module) {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(module, path, MAX_PATH);
    const char* base = std::strrchr(path, '\\');
    std::snprintf(text, sizeof(text), "%s+0x%llX", base ? base + 1 : path,
                  static_cast<unsigned long long>(address - reinterpret_cast<std::uintptr_t>(module)));
    return text;
  }
#elif defined(XENON_HOST_SAMPLER_LINUX)
  Dl_info info{};
  if (dladdr(reinterpret_cast<void*>(address), &info)) {
    if (info.dli_sname) {
      int status = 0;
      char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
      std::string name = status == 0 && demangled ? demangled : info.dli_sname;
      std::free(demangled);
      return name;
    }
    if (info.dli_fname) {
      const char* base = std::strrchr(info.dli_fname, '/');
      std::snprintf(text, sizeof(text), "%s+0x%llX", base ? base + 1 : info.dli_fname,
                    static_cast<unsigned long long>(
                        address - reinterpret_cast<std::uintptr_t>(info.dli_fbase)));
      return text;
    }
  }
#endif
  std::snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(address));
  return text;
}

std::vector<HostThreadProfile> HostSampler::profiles() const {
  std::vector<HostThreadProfile> result;
  std::unordered_map<std::uintptr_t, std::string> names;
  const auto name_of = [&](std::uintptr_t address) -> const std::string& {
    auto [it, inserted] = names.try_emplace(address);
    if (inserted) it->second = symbolize(address);
    return it->second;
  };
  std::scoped_lock lock(state_->mutex);
  for (const auto& [id, thread] : state_->threads) {
    if (!thread.samples) continue;
    HostThreadProfile profile{};
    profile.thread_id = id;
    profile.name = thread.name;
    profile.samples = thread.samples;
    profile.running_samples = thread.running;
    profile.cpu_ns = thread.cpu_ns;
    std::unordered_map<std::string, HostSampleEntry> functions;
    for (const auto& [address, count] : thread.self) {
      auto& entry = functions[name_of(address)];
      entry.self += count;
    }
    for (const auto& [address, count] : thread.inclusive) {
      auto& entry = functions[name_of(address)];
      entry.inclusive += count;
    }
    for (auto& [symbol, entry] : functions) {
      entry.symbol = symbol;
      // Recursion through different return addresses can repeat a function
      // within one sample; it still cannot exceed every sample.
      entry.inclusive = (std::min)(entry.inclusive, thread.samples);
      profile.functions.push_back(std::move(entry));
    }
    std::sort(profile.functions.begin(), profile.functions.end(),
              [](const HostSampleEntry& a, const HostSampleEntry& b) {
                return a.self != b.self ? a.self > b.self : a.inclusive > b.inclusive;
              });
    result.push_back(std::move(profile));
  }
  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
    return a.running_samples != b.running_samples ? a.running_samples > b.running_samples
                                                  : a.samples > b.samples;
  });
  return result;
}

std::string HostSampler::report() const {
  const auto threads = profiles();
  std::string out;
  char line[768];
  std::snprintf(line, sizeof(line),
                "host profile: interval %lld us, %llu sampling ticks, %zu threads sampled\n"
                "  (self = sampled PC in the function; incl = the function anywhere on the "
                "stack; run = samples in which the thread's CPU counter advanced)\n",
                static_cast<long long>(state_->options.interval.count()),
                static_cast<unsigned long long>(samples()), threads.size());
  out += line;
  std::size_t quiet = 0;
  for (const auto& thread : threads) {
    if (thread.samples < state_->options.min_thread_samples) {
      ++quiet;
      continue;
    }
    const double samples = static_cast<double>(thread.samples);
    std::snprintf(line, sizeof(line),
                  "\nthread %llu \"%s\": %llu samples, running in %.1f%%, cpu %.2f s\n"
                  "   self%%   incl%%  function\n",
                  static_cast<unsigned long long>(thread.thread_id), thread.name.c_str(),
                  static_cast<unsigned long long>(thread.samples),
                  100.0 * static_cast<double>(thread.running_samples) / samples,
                  static_cast<double>(thread.cpu_ns) / 1e9);
    out += line;
    // By self time, then the heaviest inclusive frames the self list missed.
    std::vector<const HostSampleEntry*> listed;
    for (const auto& entry : thread.functions) {
      if (listed.size() >= state_->options.top_functions || !entry.self) break;
      listed.push_back(&entry);
    }
    std::vector<const HostSampleEntry*> by_inclusive;
    for (const auto& entry : thread.functions) by_inclusive.push_back(&entry);
    std::sort(by_inclusive.begin(), by_inclusive.end(),
              [](const auto* a, const auto* b) { return a->inclusive > b->inclusive; });
    std::size_t added = 0;
    for (const auto* entry : by_inclusive) {
      if (added >= state_->options.top_functions || !entry->inclusive) break;
      if (std::find(listed.begin(), listed.end(), entry) != listed.end()) continue;
      listed.push_back(entry);
      ++added;
    }
    for (const auto* entry : listed) {
      std::snprintf(line, sizeof(line), "  %6.2f  %6.2f  %.600s\n",
                    100.0 * static_cast<double>(entry->self) / samples,
                    100.0 * static_cast<double>(entry->inclusive) / samples,
                    entry->symbol.c_str());
      out += line;
    }
  }
  if (quiet) {
    std::snprintf(line, sizeof(line), "\n%zu threads with fewer than %llu samples not listed\n",
                  quiet, static_cast<unsigned long long>(state_->options.min_thread_samples));
    out += line;
  }
  return out;
}

void set_current_thread_name(std::string_view name) {
#if defined(XENON_HOST_SAMPLER_WINDOWS)
  const int length = MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()),
                                         nullptr, 0);
  if (length <= 0) return;
  std::wstring wide(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), wide.data(), length);
  (void)SetThreadDescription(GetCurrentThread(), wide.c_str());
#elif defined(__linux__)
  char truncated[16] = {};
  std::memcpy(truncated, name.data(), (std::min)(name.size(), sizeof(truncated) - 1u));
  (void)pthread_setname_np(pthread_self(), truncated);
#else
  (void)name;
#endif
}

std::optional<HostSamplerOptions> host_sampler_options_from_environment() {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)  // read once, before any thread could set it
#endif
  const char* value = std::getenv("XENON_HOST_PROFILE");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
  if (!value || !*value || std::string_view(value) == "0") return std::nullopt;
  HostSamplerOptions options{};
  if (std::string_view(value) != "1") {
    const auto micros = std::strtoll(value, nullptr, 10);
    if (micros <= 0) return std::nullopt;
    options.interval = std::chrono::microseconds((std::max)(100ll, static_cast<long long>(micros)));
  }
  return options;
}

namespace {
std::mutex g_host_profile_mutex;
std::unique_ptr<HostSampler> g_host_profile;
}  // namespace

void start_host_profile_from_environment() {
  const auto options = host_sampler_options_from_environment();
  if (!options || !HostSampler::supported()) return;
  std::scoped_lock lock(g_host_profile_mutex);
  if (g_host_profile && g_host_profile->running()) return;
  g_host_profile = std::make_unique<HostSampler>();
  if (!g_host_profile->start(*options)) g_host_profile.reset();
}

void stop_host_profile_and_report() {
  std::unique_ptr<HostSampler> sampler;
  {
    std::scoped_lock lock(g_host_profile_mutex);
    sampler = std::move(g_host_profile);
  }
  if (!sampler) return;
  sampler->stop();
  try {
    const auto directory = probe_log_directory();
    const auto path = directory.empty() ? std::filesystem::path("host_profile.log")
                                        : std::filesystem::path(directory) / "host_profile.log";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << sampler->report();
  } catch (...) {
    // A diagnostic must not take the session down while it stops.
  }
}

}  // namespace xenon::logging

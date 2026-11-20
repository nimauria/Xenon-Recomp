#pragma once

// Host sampling profiler: where the host's own threads spend wall time.
//
// perf_frame.log says how long the GPU worker spent decoding the ring or
// playing IR; it cannot say which function inside a draw took that time, nor
// what the other threads were doing. The sampler answers both. Every
// `interval` it stops each thread of the process for a moment, records where
// it was (and, on Windows x64, the return addresses above it), and lets it
// go. A thread's CPU cycle counter, read at each sample, says whether it ran
// since the previous one or sat blocked, so each thread's report separates
// work from waiting.
//
// The report (report(), or host_profile.log in the run's probe directory when
// started from XENON_HOST_PROFILE) lists, per thread with its name and CPU
// time: the functions taking the most samples by self time (the sampled PC)
// and, where stacks were walked, inclusive time (any frame of the sample).
// Symbols come from DbgHelp (PDBs beside the binaries) on Windows and from
// dladdr on Linux; an address without a symbol is shown as module+offset.
//
// Platforms:
//   Windows x64/ARM64  SuspendThread + GetThreadContext; x64 stacks unwound
//                      with RtlVirtualUnwind (no allocation while a thread is
//                      suspended: it may hold the heap lock).
//   Linux              a real-time signal delivered with tgkill; the handler
//                      stores the interrupted PC (leaf only). SA_RESTART is
//                      set, but a syscall the kernel does not restart may see
//                      EINTR, so this is a development tool, not left on.
// Elsewhere supported() is false and start() refuses.
//
// It is a diagnostic: nothing it measures feeds back into execution.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xenon::logging {

struct HostSamplerOptions {
  std::chrono::microseconds interval{1000};
  // Frames recorded per sample, the sampled PC included (Windows x64 only;
  // other hosts record the PC alone).
  std::size_t max_frames{48};
  // Functions listed per thread in the report.
  std::size_t top_functions{30};
  // Threads with fewer samples than this are summarised, not listed.
  std::uint64_t min_thread_samples{50};
};

// One function's share of a thread's samples, for report consumers and tests.
struct HostSampleEntry {
  std::string symbol;
  std::uint64_t self{};
  std::uint64_t inclusive{};
};

struct HostThreadProfile {
  std::uint64_t thread_id{};
  std::string name;
  std::uint64_t samples{};
  // Samples in which the thread's cycle counter had advanced since the
  // previous one (it ran); the rest found it blocked.
  std::uint64_t running_samples{};
  std::uint64_t cpu_ns{};
  std::vector<HostSampleEntry> functions;  // most self samples first
};

class HostSampler {
 public:
  HostSampler();
  ~HostSampler();
  HostSampler(const HostSampler&) = delete;
  HostSampler& operator=(const HostSampler&) = delete;

  [[nodiscard]] static bool supported() noexcept;

  // Starts sampling every thread of the process but the sampler's own. False
  // when unsupported or already running.
  bool start(const HostSamplerOptions& options = {});
  // Stops and joins the sampling thread; samples stay for report().
  void stop() noexcept;
  [[nodiscard]] bool running() const noexcept;
  [[nodiscard]] std::uint64_t samples() const noexcept;

  // Symbolised per-thread profiles of what was sampled, busiest first: all of
  // it after stop(), or everything so far while sampling continues.
  [[nodiscard]] std::vector<HostThreadProfile> profiles() const;
  // The text report of profiles().
  [[nodiscard]] std::string report() const;

  // Resolves one host code address to "function" or "module+0xoffset"
  // (exposed for tests).
  [[nodiscard]] static std::string symbolize(std::uintptr_t address);

  struct State;

 private:
  std::unique_ptr<State> state_;
};

// Names code the platform's symbols cannot: generated game code, whose
// module carries no debug symbols. Consulted before them by symbolize(); an
// empty result falls through. Replaced by each call; an empty function
// clears it.
using CodeSymbolizer = std::function<std::optional<std::string>(std::uintptr_t)>;
void set_code_symbolizer(CodeSymbolizer symbolizer);

// XENON_HOST_PROFILE: unset/"0" off; "1" samples every millisecond; another
// number is the interval in microseconds (at least 100).
[[nodiscard]] std::optional<HostSamplerOptions> host_sampler_options_from_environment();

// The process-wide sampler XENON_HOST_PROFILE drives: started by the runtime
// session when the variable is set, stopped (writing host_profile.log to the
// probe directory) when the session stops. Both are no-ops otherwise.
void start_host_profile_from_environment();
void stop_host_profile_and_report();
// Writes host_profile.log as it stands while sampling continues, so a run
// that crashes or is killed keeps its profile (the guest profiler calls this
// every 30 s). A no-op when the sampler is not running.
void write_host_profile_snapshot();

// Names the calling thread for host tools: profiles (host_profile.log),
// debuggers and crash dumps (SetThreadDescription; pthread_setname_np, which
// keeps the first 15 bytes, on Linux).
void set_current_thread_name(std::string_view name);

}  // namespace xenon::logging

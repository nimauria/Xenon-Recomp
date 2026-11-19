#pragma once

// Background shader compiles. A draw whose shader is not compiled yet asks
// the queue for it; the first request queues the compile on a worker thread
// and every request returns at once - the finished shader, or nothing while
// it is still compiling - so the GPU worker never waits on DXC unless a
// caller chooses to (wait()). Each worker owns its own DxcShaderCompiler:
// DXC compiler objects must not be used from two threads at once.
//
// Jobs are keyed by the caller (a shader hash, with variant bits). A key is
// compiled once; its result - success or failure - stays until clear().
// Results also go through the ShaderCache given, so they are stored on disk
// and later sessions load them without compiling.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "xenon/gpu/dxc_shader_compiler.hpp"

namespace xenon::gpu {

class ShaderCompileQueue {
 public:
  using Clock = std::chrono::steady_clock;

  // `workers` of 0 picks three quarters of the hardware threads (at least
  // one), as xenia sizes its pipeline creation threads.
  ShaderCompileQueue(ShaderCache& cache, unsigned workers = 0);
  ~ShaderCompileQueue();
  ShaderCompileQueue(const ShaderCompileQueue&) = delete;
  ShaderCompileQueue& operator=(const ShaderCompileQueue&) = delete;

  // The finished compile of `key`, or null while it is pending. The first
  // request for a key queues `shader` (with `options`); later ones only
  // look. A key already in the cache (memory or disk) is returned at once.
  [[nodiscard]] std::shared_ptr<const CompiledShader> request(
      std::uint64_t key, const LoweredShader& shader, const ShaderCompileOptions& options);

  // The finished compile of a key already requested, waiting for it until
  // `deadline`; null if it is still compiling then, or was never requested.
  [[nodiscard]] std::shared_ptr<const CompiledShader> wait(std::uint64_t key,
                                                           Clock::time_point deadline);

  // GPU V3 prefetch: a shader the frontend has decoded, lowered and compiled
  // on a worker before the backend reaches it. `lower` runs on the worker
  // (lowering is a pure function of the decoded program); the first prefetch
  // of a key queues it, later ones do nothing. Safe from any thread.
  struct Prepared {
    std::shared_ptr<const LoweredShader> lowered{};
    // Null when the lowering is incomplete (nothing is compiled then).
    std::shared_ptr<const CompiledShader> compiled{};
    std::string error{};  // the lowering threw
  };
  using Lowering = std::function<LoweredShader()>;
  void prefetch(std::uint64_t key, Lowering lower, const ShaderCompileOptions& options);
  // The prepared shader for a prefetched key, waiting while it is lowered or
  // compiled; nullopt when the key was never prefetched.
  [[nodiscard]] std::optional<Prepared> prepared(std::uint64_t key);
  // Whether `key` was prefetched (queued, running or done).
  [[nodiscard]] bool prefetched(std::uint64_t key) const;

  // Compiles queued or running.
  [[nodiscard]] std::size_t pending() const;
  [[nodiscard]] unsigned worker_count() const noexcept {
    return static_cast<unsigned>(workers_.size());
  }

  // Waits for every queued compile, then forgets all results.
  void clear();

 private:
  struct Job {
    std::uint64_t key;
    LoweredShader shader;
    ShaderCompileOptions options;
    Lowering lower{};  // set: a prefetch job, lowered on the worker
  };
  struct PreparedEntry {
    bool done{};
    Prepared prepared{};
  };
  std::unordered_map<std::uint64_t, PreparedEntry> prepared_;
  void run_worker();

  ShaderCache& cache_;
  mutable std::mutex mutex_;
  std::condition_variable work_ready_;
  std::condition_variable work_done_;
  std::deque<Job> queue_;
  std::size_t running_ = 0;
  bool stopping_ = false;
  // A key's entry exists from its first request; the pointer is set when it
  // finishes.
  std::unordered_map<std::uint64_t, std::shared_ptr<const CompiledShader>> results_;
  std::vector<std::thread> workers_;
};

}  // namespace xenon::gpu

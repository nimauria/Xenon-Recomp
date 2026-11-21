#pragma once

// Asynchronous pipeline creation (GPU Stage 3.1).
//
// A graphics pipeline takes a driver tens of milliseconds to build (more with
// no persistent cache), and a draw used to wait for it where it was prepared.
// Here the build is queued on the shader worker pool the moment a draw (or a
// prefetch) asks for it, the draw goes on preparing - textures, constants,
// uploads, the draws after it - and waits only where it needs the pipeline
// object, when its run of draws is handed to command recording.
//
// Each pipeline is a slot with a state:
//   Unknown    never requested
//   Queued     handed to the pool, not started
//   Compiling  a worker is building it
//   Ready      built; the pipeline object may be read from any thread
//   Failed     the build failed (error() says why)
// A draw is never skipped for a pipeline that is merely not ready: it waits.
// A failed build is reported to the draw that needed it; the next draw that
// asks for the same pipeline builds it again, up to kMaxAttempts builds in
// all, after which the failure stands (an unbuildable state is not retried on
// every draw forever).
//
// Queued builds run in priority order: a draw's own pipeline (urgent) before
// anything prefetched, and a prefetched pipeline a draw then waits for jumps
// the queue (promote).
//
// With no executor (XENON_ASYNC_PIPELINES=0, or no worker pool) request()
// builds on the calling thread before it returns - the synchronous behaviour.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "xenon/gpu/perf_stats.hpp"

namespace xenon::gpu {

enum class PipelineState : std::uint8_t { Unknown, Queued, Compiling, Ready, Failed };

[[nodiscard]] constexpr const char* to_string(PipelineState state) noexcept {
  switch (state) {
    case PipelineState::Unknown: return "unknown";
    case PipelineState::Queued: return "queued";
    case PipelineState::Compiling: return "compiling";
    case PipelineState::Ready: return "ready";
    case PipelineState::Failed: return "failed";
  }
  return "unknown";
}

// A pool that runs tasks on worker threads in priority order.
class PipelineExecutor {
 public:
  virtual ~PipelineExecutor() = default;
  // Queues `task` under `id`: urgent tasks run before every non-urgent one,
  // each group in submission order.
  virtual void submit(std::uint64_t id, std::function<void()> task, bool urgent) = 0;
  // Moves a still-queued task to the urgent group; false when `id` is not
  // waiting in the queue (running, finished, or unknown).
  virtual bool promote(std::uint64_t id) = 0;
};

// Whether pipeline creation runs on the worker pool: XENON_ASYNC_PIPELINES=0
// restores synchronous creation.
[[nodiscard]] inline bool async_pipelines_enabled() noexcept {
  const char* value = std::getenv("XENON_ASYNC_PIPELINES");
  return !(value && value[0] == '0' && value[1] == '\0');
}

template <class Pipeline>
class PipelineBuilds {
 public:
  // Builds the pipeline into `pipeline`; false with `error` set on failure.
  // Runs on a worker (or the requesting thread without an executor).
  using Build = std::function<bool(Pipeline& pipeline, std::string& error)>;
  static constexpr unsigned kMaxAttempts = 3;

  class Slot {
   public:
    // The pipeline; valid to read once state() is Ready.
    Pipeline pipeline{};
    [[nodiscard]] PipelineState state() const noexcept {
      return state_.load(std::memory_order_acquire);
    }
    // Why the build failed; valid once state() is Failed.
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] unsigned attempts() const noexcept { return attempts_; }

   private:
    friend class PipelineBuilds;
    std::atomic<PipelineState> state_{PipelineState::Unknown};
    std::string error_{};
    Build build_{};
    std::uint64_t key_{};
    unsigned attempts_{};
  };

  PipelineBuilds() = default;
  ~PipelineBuilds() { drain(); }
  PipelineBuilds(const PipelineBuilds&) = delete;
  PipelineBuilds& operator=(const PipelineBuilds&) = delete;

  // The pool builds run on (null: build synchronously). Set before the first
  // request, from the thread that requests.
  void set_executor(PipelineExecutor* executor) noexcept { executor_ = executor; }
  [[nodiscard]] bool asynchronous() const noexcept { return executor_ != nullptr; }

  [[nodiscard]] Slot* find(std::uint64_t key) const {
    const auto found = slots_.find(key);
    return found == slots_.end() ? nullptr : found->second.get();
  }
  [[nodiscard]] bool contains(std::uint64_t key) const { return slots_.contains(key); }
  [[nodiscard]] std::size_t size() const noexcept { return slots_.size(); }
  // Whether a request for `key` has anything to do: it was never requested, or
  // its last build failed and it has attempts left. (A caller builds its
  // recipe only then.)
  [[nodiscard]] bool needs_build(std::uint64_t key) const {
    const auto* slot = find(key);
    return !slot || (slot->state() == PipelineState::Failed && slot->attempts_ < kMaxAttempts);
  }

  // Queues the build of `key` (urgent: ahead of every non-urgent build) and
  // returns its slot at once - or after building it, without an executor. A
  // key already queued, building or ready is left alone (a queued one is
  // promoted when urgent); a failed one with attempts left builds again.
  Slot& request(std::uint64_t key, Build build, bool urgent) {
    auto& owned = slots_[key];
    if (!owned) {
      owned = std::make_unique<Slot>();
      owned->key_ = key;
    }
    Slot& slot = *owned;
    const auto state = slot.state();
    if (state == PipelineState::Queued) {
      if (urgent && executor_) executor_->promote(key);
      return slot;
    }
    if (state == PipelineState::Compiling || state == PipelineState::Ready) return slot;
    if (state == PipelineState::Failed) {
      if (slot.attempts_ >= kMaxAttempts) return slot;
      slot.pipeline.reset();
    }
    slot.error_.clear();
    slot.build_ = std::move(build);
    ++slot.attempts_;
    slot.state_.store(PipelineState::Queued, std::memory_order_release);
    {
      std::scoped_lock lock(mutex_);
      ++outstanding_;
    }
    perf::add(perf::Counter::PipelinesQueued, 1u);
    if (!executor_) {
      run(slot);
    } else {
      executor_->submit(key, [this, &slot] { run(slot); }, urgent);
    }
    return slot;
  }

  // The state `slot` ends in: waits while it is queued or building (jumping a
  // queued build ahead of the rest), counting the time in PipelineWaitNs.
  PipelineState wait(const Slot& slot) {
    auto state = slot.state();
    if (state == PipelineState::Ready || state == PipelineState::Failed) return state;
    if (state == PipelineState::Queued && executor_) executor_->promote(slot.key_);
    const auto start = std::chrono::steady_clock::now();
    {
      std::unique_lock lock(mutex_);
      done_.wait(lock, [&] {
        state = slot.state();
        return state == PipelineState::Ready || state == PipelineState::Failed;
      });
    }
    perf::add(perf::Counter::PipelineWaitNs,
              static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - start)
                                             .count()));
    perf::add(perf::Counter::PipelineWaits, 1u);
    return state;
  }

  // Waits for every queued or running build (before what the builds use is
  // destroyed).
  void drain() {
    std::unique_lock lock(mutex_);
    done_.wait(lock, [&] { return outstanding_ == 0; });
  }
  [[nodiscard]] std::size_t outstanding() const {
    std::scoped_lock lock(mutex_);
    return outstanding_;
  }

 private:
  void run(Slot& slot) {
    slot.state_.store(PipelineState::Compiling, std::memory_order_release);
    bool ok = false;
    std::string error;
    {
      const perf::ScopedTime time(perf::Counter::PipelineCreateNs, perf::Counter::PipelineCreates);
      try {
        ok = slot.build_(slot.pipeline, error);
      } catch (const std::exception& exception) {
        error = exception.what();
      } catch (...) {
        error = "pipeline creation threw";
      }
    }
    slot.build_ = nullptr;  // the build's captured shaders are no longer needed
    if (!ok) {
      slot.error_ = error.empty() ? std::string("pipeline creation failed") : std::move(error);
      perf::add(perf::Counter::PipelineFailures, 1u);
    }
    {
      std::scoped_lock lock(mutex_);
      slot.state_.store(ok ? PipelineState::Ready : PipelineState::Failed,
                        std::memory_order_release);
      --outstanding_;
    }
    done_.notify_all();
  }

  PipelineExecutor* executor_{};
  // Slots are only added and found from the requesting thread; workers reach
  // theirs through the pointer their task captured (addresses are stable).
  std::unordered_map<std::uint64_t, std::unique_ptr<Slot>> slots_{};
  mutable std::mutex mutex_;
  std::condition_variable done_;
  std::size_t outstanding_{};
};

}  // namespace xenon::gpu

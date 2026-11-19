#pragma once

// GPU V3 backend execution thread.
//
// The command processor (the ordered frontend) decodes PM4 into graphics IR
// on the GPU pump thread. Consuming that IR - native state translation,
// uploads, command recording and submission - and presenting run here, on a
// thread of their own, in exactly the order they were handed over, so the
// frontend decodes the next commands while the backend records the ones
// before them.
//
// gpu::Backend has no internal synchronisation, so the executor guarantees
// that no two backend operations ever overlap: a task runs either on the
// worker thread, or - through run() when the worker is idle with nothing
// queued - inline on the calling thread while the worker is held off. The
// mutex handing each task over orders every task's memory effects before the
// next one's, whichever thread runs it.
//
// Guest-visible effects stay in order: before the frontend writes a fence,
// raises an interrupt or evaluates a wait, it calls run() for the work that
// makes the GPU's results visible, which returns only after every task handed
// over before it has finished.
//
// Not started, the executor runs every task inline (tools, tests, captures).

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace xenon::gpu {

class BackendExecutor {
 public:
  using Task = std::function<void()>;

  struct Statistics {
    std::uint64_t posted{};        // tasks queued for the worker
    std::uint64_t ran_on_worker{}; // tasks the worker ran
    std::uint64_t ran_inline{};    // run() tasks executed on the caller (worker idle)
    std::uint64_t waits{};         // run()/wait_idle() calls that had to wait for the worker
    std::uint64_t wait_ns{};       // time those calls waited
    std::uint64_t worker_busy_ns{};
    std::uint64_t task_failures{}; // tasks that threw
  };

  BackendExecutor() = default;
  ~BackendExecutor() { stop(); }
  BackendExecutor(const BackendExecutor&) = delete;
  BackendExecutor& operator=(const BackendExecutor&) = delete;

  // Starts the worker thread. Idempotent.
  void start();
  // Runs every task still queued, then joins the worker. Afterwards tasks run
  // inline again. Must not be called from a task.
  void stop() noexcept;
  [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }

  // Queues `task` after everything handed over before it. Inline when the
  // executor is not running or the caller is the worker itself.
  void post(Task task) { static_cast<void>(post_sequenced(std::move(task))); }
  // post(), returning the task's sequence for wait_until_done() (0 when it
  // already ran inline).
  [[nodiscard]] std::uint64_t post_sequenced(Task task);
  // Returns once the task posted with `sequence`, and all before it, has run.
  void wait_until_done(std::uint64_t sequence) {
    if (sequence && running() && !inside_task()) wait_for(sequence);
  }
  // Whether the task posted with `sequence` (and all before it) has run.
  [[nodiscard]] bool done(std::uint64_t sequence) const noexcept {
    return completed_sequence_.load(std::memory_order_acquire) >= sequence;
  }
  // Runs `task` after everything handed over before it and returns once it
  // has run. When the worker is idle with nothing queued, `task` runs on the
  // calling thread with no hand-off. Inline when not running or on the worker.
  void run(const Task& task);
  // Returns once everything handed over so far has run.
  void wait_idle();
  // Whether the worker has nothing to do (a hint for batching hand-offs).
  [[nodiscard]] bool idle() const noexcept {
    return pending_.load(std::memory_order_acquire) == 0u;
  }
  // Whether the caller is executing one of this executor's tasks - on the
  // worker, or inline through run(). Calls made from inside a task (a present
  // task presenting through the graphics system) run at once: queueing them
  // behind the task that makes them would wait for itself.
  [[nodiscard]] bool inside_task() const noexcept;

  [[nodiscard]] Statistics statistics() const;
  // The message of the most recent task that threw, cleared by the call.
  [[nodiscard]] std::string take_error();

 private:
  struct Item {
    Task task{};
    std::uint64_t sequence{};
  };

  void run_worker();
  void execute(const Task& task, bool on_worker);
  // Blocks until every task with a sequence <= `sequence` has run.
  void wait_for(std::uint64_t sequence);

  mutable std::mutex mutex_{};
  std::condition_variable work_{};
  std::condition_variable done_{};
  std::deque<Item> queue_{};
  // Tasks handed over and not finished (queued + running on the worker).
  std::atomic<std::uint64_t> pending_{0};
  std::uint64_t next_sequence_{0};
  std::atomic<std::uint64_t> completed_sequence_{0};
  bool worker_running_task_{false};
  bool inline_running_{false};
  bool stopping_{false};
  std::atomic<bool> running_{false};
  std::atomic<std::thread::id> worker_id_{};
  std::thread thread_{};

  Statistics stats_{};
  std::string error_{};
};

}  // namespace xenon::gpu

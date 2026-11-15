#pragma once

// GPU V3 parallel command recording: worker threads that record runs of
// prepared draws into command lists of their own.
//
// Each task records one run into a command list (or buffer) nothing else
// touches until the batch is submitted, from immutable prepared draws, so the
// tasks are independent. The submission coordinator (each backend's
// CommandQueue) waits for every task of a batch before submitting it in
// order.
//
// Sized to the machine: a few workers on a desktop, one on a handheld; more
// would only compete with the guest's own threads (each run is a few dozen
// draws, recorded in well under a millisecond).

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xenon::gpu {

class RecordingWorkers {
 public:
  using Task = std::function<void()>;

  // `threads` == 0 runs every task on the submitting thread.
  explicit RecordingWorkers(unsigned threads);
  ~RecordingWorkers();
  RecordingWorkers(const RecordingWorkers&) = delete;
  RecordingWorkers& operator=(const RecordingWorkers&) = delete;

  // XENON_GPU_RECORD_THREADS when set (0 disables parallel recording);
  // otherwise one worker per eight hardware threads, from one to four.
  [[nodiscard]] static unsigned default_thread_count();

  [[nodiscard]] unsigned thread_count() const noexcept {
    return static_cast<unsigned>(threads_.size());
  }
  void submit(Task task);
  // Returns once every task submitted so far has finished.
  void wait_all();
  [[nodiscard]] std::uint64_t tasks_run() const;
  // The message of the most recent task that threw, cleared by the call.
  [[nodiscard]] std::string take_error();

 private:
  void run();

  mutable std::mutex mutex_{};
  std::condition_variable work_{};
  std::condition_variable done_{};
  std::deque<Task> queue_{};
  std::uint64_t pending_{};
  std::uint64_t tasks_run_{};
  bool stopping_{};
  std::string error_{};
  std::vector<std::thread> threads_{};
};

}  // namespace xenon::gpu

#include "xenon/gpu/recording_workers.hpp"

#include <algorithm>
#include <cstdlib>
#include <exception>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace xenon::gpu {

unsigned RecordingWorkers::default_thread_count() {
  if (const char* value = std::getenv("XENON_GPU_RECORD_THREADS"); value && *value) {
    char* end = nullptr;
    const auto parsed = std::strtoul(value, &end, 10);
    if (end != value) return static_cast<unsigned>((std::min)(parsed, 16ul));
  }
  const unsigned hardware = std::thread::hardware_concurrency();
  return std::clamp(hardware / 8u, 1u, 4u);
}

RecordingWorkers::RecordingWorkers(unsigned threads) {
  threads_.reserve(threads);
  for (unsigned i = 0; i < threads; ++i) threads_.emplace_back([this] { run(); });
}

RecordingWorkers::~RecordingWorkers() {
  {
    std::scoped_lock lock(mutex_);
    stopping_ = true;
  }
  work_.notify_all();
  for (auto& thread : threads_) thread.join();
}

void RecordingWorkers::submit(Task task) {
  if (threads_.empty()) {
    try {
      task();
    } catch (const std::exception& ex) {
      std::scoped_lock lock(mutex_);
      error_ = ex.what();
    }
    std::scoped_lock lock(mutex_);
    ++tasks_run_;
    return;
  }
  {
    std::scoped_lock lock(mutex_);
    queue_.push_back(std::move(task));
    ++pending_;
  }
  work_.notify_one();
}

void RecordingWorkers::wait_all() {
  std::unique_lock lock(mutex_);
  done_.wait(lock, [this] { return pending_ == 0; });
}

std::uint64_t RecordingWorkers::tasks_run() const {
  std::scoped_lock lock(mutex_);
  return tasks_run_;
}

std::string RecordingWorkers::take_error() {
  std::scoped_lock lock(mutex_);
  std::string error;
  error.swap(error_);
  return error;
}

void RecordingWorkers::run() {
#if defined(_WIN32)
  (void)SetThreadDescription(GetCurrentThread(), L"Xenon GPU recording");
#endif
  std::unique_lock lock(mutex_);
  for (;;) {
    work_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
    if (queue_.empty()) return;  // stopping, nothing left
    Task task = std::move(queue_.front());
    queue_.pop_front();
    lock.unlock();
    std::string failure;
    try {
      task();
    } catch (const std::exception& ex) {
      failure = ex.what();
    } catch (...) {
      failure = "recording task threw a non-standard exception";
    }
    task = nullptr;
    lock.lock();
    if (!failure.empty()) error_ = std::move(failure);
    ++tasks_run_;
    if (--pending_ == 0) done_.notify_all();
  }
}

}  // namespace xenon::gpu

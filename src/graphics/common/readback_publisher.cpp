#include "xenon/gpu/readback_publisher.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include "xenon/gpu/perf_stats.hpp"

namespace xenon::gpu {

namespace {
constexpr std::uint64_t kNone = (std::numeric_limits<std::uint64_t>::max)();
// How long one wait for the GPU lasts before the thread rechecks for a stop
// request; the value it waits for may still be in an unsubmitted batch.
constexpr std::uint32_t kCompletionPollMs = 2u;
}  // namespace

void ReadbackPublisher::start(memory::AddressSpace& memory,
                              memory::GuestMemoryGpuCoherency& coherency,
                              std::span<const std::byte> readback,
                              std::uint32_t max_range_bytes,
                              CompletionWait completion_wait) {
  stop();
  memory_ = &memory;
  coherency_ = &coherency;
  readback_ = readback;
  max_range_bytes_ = max_range_bytes;
  completion_wait_ = std::move(completion_wait);
  {
    std::scoped_lock lock(mutex_);
    queue_.clear();
    in_flight_min_value_ = kNone;
    pushed_sequence_ = taken_sequence_ = done_sequence_ = 0u;
    last_value_ = 0u;
    published_.clear();
    error_.clear();
    running_ = true;
  }
  thread_ = std::thread(&ReadbackPublisher::run, this);
}

void ReadbackPublisher::stop() noexcept {
  {
    std::scoped_lock lock(mutex_);
    running_ = false;
  }
  work_.notify_all();
  done_.notify_all();
  if (thread_.joinable()) thread_.join();
  std::scoped_lock lock(mutex_);
  queue_.clear();
  in_flight_min_value_ = kNone;
  done_sequence_ = taken_sequence_ = pushed_sequence_;
}

void ReadbackPublisher::push(const memory::GpuReadbackRange& range, std::uint64_t offset,
                             std::uint64_t value) {
  {
    std::scoped_lock lock(mutex_);
    queue_.push_back({range, offset, value, ++pushed_sequence_});
    last_value_ = (std::max)(last_value_, value);
  }
  work_.notify_one();
}

std::uint64_t ReadbackPublisher::published_through() const {
  std::scoped_lock lock(mutex_);
  auto oldest = in_flight_min_value_;
  if (!queue_.empty()) oldest = (std::min)(oldest, queue_.front().value);
  return oldest == kNone ? kNone : oldest - 1u;
}

bool ReadbackPublisher::wait_sequence(std::unique_lock<std::mutex>& lock,
                                      std::uint64_t sequence) {
  done_.wait(lock, [&] {
    return done_sequence_ >= sequence || !error_.empty() || !running_;
  });
  return error_.empty() && done_sequence_ >= sequence;
}

bool ReadbackPublisher::wait_published(std::uint64_t value) {
  const perf::ScopedTime waited(perf::Counter::ReadbackWaitNs, perf::Counter::ReadbackWaits);
  std::unique_lock lock(mutex_);
  std::uint64_t target = 0u;
  for (auto it = queue_.rbegin(); it != queue_.rend(); ++it) {
    if (it->value <= value) {
      target = it->sequence;
      break;
    }
  }
  if (!target && in_flight_min_value_ <= value) target = taken_sequence_;
  if (!target) return error_.empty();
  return wait_sequence(lock, target);
}

bool ReadbackPublisher::wait_all() {
  const perf::ScopedTime waited(perf::Counter::ReadbackWaitNs, perf::Counter::ReadbackWaits);
  std::unique_lock lock(mutex_);
  return wait_sequence(lock, pushed_sequence_);
}

bool ReadbackPublisher::pending() const {
  std::scoped_lock lock(mutex_);
  return done_sequence_ < pushed_sequence_;
}

std::uint64_t ReadbackPublisher::last_value() const {
  std::scoped_lock lock(mutex_);
  return last_value_;
}

std::vector<memory::GpuPublishedWrite> ReadbackPublisher::take_published_writes() {
  std::scoped_lock lock(mutex_);
  return std::exchange(published_, {});
}

std::string ReadbackPublisher::error() const {
  std::scoped_lock lock(mutex_);
  return error_;
}

void ReadbackPublisher::run() {
  std::unique_lock lock(mutex_);
  for (;;) {
    work_.wait(lock, [&] { return !running_ || !queue_.empty(); });
    if (!running_) return;
    const auto value = queue_.front().value;
    lock.unlock();
    const bool complete = completion_wait_(value, kCompletionPollMs);
    lock.lock();
    if (!running_) return;
    if (!complete) continue;

    // Readbacks are queued in GPU order, so everything up to this value is
    // finished too.
    std::vector<Entry> batch;
    while (!queue_.empty() && queue_.front().value <= value) {
      batch.push_back(queue_.front());
      queue_.pop_front();
    }
    taken_sequence_ = batch.back().sequence;
    in_flight_min_value_ = value;
    lock.unlock();
    std::vector<memory::GpuPublishedWrite> published;
    std::string failure;
    std::uint64_t written = 0u;
    {
      // Guest CPU writes wait while the window is held.
      const perf::ScopedTime held(perf::Counter::ReadbackWindowNs);
      auto window = memory_->physical_write_window();
      if (!window) {
        failure = "failed to acquire the guest-memory readback window";
      } else {
        for (const auto& entry : batch) {
          // CPU threads may have written the same Xbox bytes while the copy
          // was in flight; only bytes still owned by this readback's GPU
          // generation are written.
          const auto safe_ranges = coherency_->prepare_gpu_download(
              memory_->coherency(), entry.range, max_range_bytes_);
          for (const auto& safe : safe_ranges) {
            const auto offset = entry.offset + (safe.address - entry.range.address);
            if (offset + safe.size > readback_.size()) {
              failure = "guest-memory readback lies outside the readback buffer";
              break;
            }
            const auto previous_epoch =
                memory_->coherency().newest_page_epoch(safe.address, safe.size);
            std::uint64_t publication_epoch = 0u;
            if (!window.write(safe.address, readback_.subspan(offset, safe.size),
                              &publication_epoch)) {
              failure = "guest-memory readback destination is outside physical RAM";
              break;
            }
            written += safe.size;
            (void)coherency_->commit_gpu_download(memory_->coherency(), safe, safe.address,
                                                  safe.size, publication_epoch);
            if (publication_epoch)
              published.push_back({publication_epoch, previous_epoch, safe.address, safe.size});
          }
          if (!failure.empty()) break;
        }
      }
    }
    perf::add(perf::Counter::ReadbackBytes, written);
    lock.lock();
    published_.insert(published_.end(), published.begin(), published.end());
    in_flight_min_value_ = kNone;
    done_sequence_ = taken_sequence_;
    if (!failure.empty()) {
      error_ = failure;
      queue_.clear();
      taken_sequence_ = done_sequence_ = pushed_sequence_;
    }
    done_.notify_all();
  }
}

}  // namespace xenon::gpu

#include "xenon/gpu/shader_compile_queue.hpp"

#include <algorithm>

namespace xenon::gpu {

ShaderCompileQueue::ShaderCompileQueue(ShaderCache& cache, unsigned workers) : cache_(cache) {
  if (workers == 0) workers = (std::max)(1u, std::thread::hardware_concurrency() * 3u / 4u);
  workers_.reserve(workers);
  for (unsigned i = 0; i < workers; ++i) workers_.emplace_back([this] { run_worker(); });
}

ShaderCompileQueue::~ShaderCompileQueue() {
  {
    std::scoped_lock lock(mutex_);
    stopping_ = true;
    queue_.clear();
  }
  work_ready_.notify_all();
  for (auto& worker : workers_) worker.join();
}

std::shared_ptr<const CompiledShader> ShaderCompileQueue::request(
    std::uint64_t key, const LoweredShader& shader, const ShaderCompileOptions& options) {
  {
    std::scoped_lock lock(mutex_);
    if (const auto found = results_.find(key); found != results_.end()) return found->second;
  }
  // Already compiled in this process or a previous one: no worker needed.
  if (auto cached = cache_.find(shader, options)) {
    std::scoped_lock lock(mutex_);
    return results_.emplace(key, std::move(cached)).first->second;
  }
  {
    std::scoped_lock lock(mutex_);
    const auto [entry, inserted] = results_.try_emplace(key);
    if (!inserted) return entry->second;
    queue_.push_back(Job{key, shader, options});
  }
  work_ready_.notify_one();
  return nullptr;
}

std::shared_ptr<const CompiledShader> ShaderCompileQueue::wait(std::uint64_t key,
                                                              Clock::time_point deadline) {
  std::unique_lock lock(mutex_);
  const auto found = results_.find(key);
  if (found == results_.end()) return nullptr;
  // A reference to the mapped value stays valid when other keys are added
  // (an iterator does not survive a rehash); results are only erased by
  // clear(), on the thread that also waits.
  const auto& result = found->second;
  work_done_.wait_until(lock, deadline, [&] { return result != nullptr || stopping_; });
  return result;
}

std::size_t ShaderCompileQueue::pending() const {
  std::scoped_lock lock(mutex_);
  return queue_.size() + running_;
}

void ShaderCompileQueue::clear() {
  std::unique_lock lock(mutex_);
  work_done_.wait(lock, [&] { return queue_.empty() && running_ == 0; });
  results_.clear();
}

void ShaderCompileQueue::run_worker() {
  // DXC compiler objects are not shared between threads.
  const DxcShaderCompiler compiler;
  std::unique_lock lock(mutex_);
  for (;;) {
    work_ready_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
    if (stopping_) return;
    auto job = std::move(queue_.front());
    queue_.pop_front();
    ++running_;
    lock.unlock();
    auto compiled = cache_.get_or_compile(job.shader, job.options, compiler);
    lock.lock();
    --running_;
    if (const auto entry = results_.find(job.key); entry != results_.end())
      entry->second = std::move(compiled);
    work_done_.notify_all();
  }
}

}  // namespace xenon::gpu

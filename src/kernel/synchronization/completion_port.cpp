#include "xenon/kernel/completion_port.hpp"

namespace xenon::kernel {

IoCompletionPort::IoCompletionPort() : KernelObject(ObjectType::IoCompletionPort) {}

void IoCompletionPort::post(CompletionPacket packet) {
  {
    std::scoped_lock lock(mutex_);
    queue_.push_back(std::move(packet));
  }
  condition_.notify_one();
}

bool IoCompletionPort::try_remove(CompletionPacket& out_packet) {
  std::scoped_lock lock(mutex_);
  if (queue_.empty()) return false;
  out_packet = std::move(queue_.front());
  queue_.pop_front();
  return true;
}

bool IoCompletionPort::remove_for(std::chrono::milliseconds timeout,
                                  CompletionPacket& out_packet) {
  std::unique_lock lock(mutex_);
  if (!condition_.wait_for(lock, timeout, [&] { return !queue_.empty(); })) {
    return false;
  }
  out_packet = std::move(queue_.front());
  queue_.pop_front();
  return true;
}

std::size_t IoCompletionPort::pending_count() const {
  std::scoped_lock lock(mutex_);
  return queue_.size();
}

}  // namespace xenon::kernel

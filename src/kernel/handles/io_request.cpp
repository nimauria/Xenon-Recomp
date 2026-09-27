#include "xenon/kernel/io_request.hpp"

namespace xenon::kernel {

IoRequest::IoRequest(std::uint64_t id, IoOperation operation,
                     std::uint64_t context)
    : id_(id), operation_(operation), context_(context) {
  result_.request_id = id_;
}

IoRequestSnapshot IoRequest::snapshot() const {
  std::scoped_lock lock(mutex_);
  return {id_, operation_, state_, context_, result_};
}

bool IoRequest::complete(IoStatus result) {
  std::scoped_lock lock(mutex_);
  if (state_ != IoRequestState::Pending) return false;
  result.request_id = id_;
  result_ = result;
  state_ = IoRequestState::Completed;
  condition_.notify_all();
  return true;
}

bool IoRequest::cancel() {
  std::scoped_lock lock(mutex_);
  if (state_ != IoRequestState::Pending) return false;
  result_ = {KernelIoCode::Cancelled, filesystem::FsError::None, 0, id_};
  state_ = IoRequestState::Cancelled;
  condition_.notify_all();
  return true;
}

IoStatus IoRequest::wait() const {
  std::unique_lock lock(mutex_);
  condition_.wait(lock, [&] { return state_ != IoRequestState::Pending; });
  return result_;
}

}  // namespace xenon::kernel

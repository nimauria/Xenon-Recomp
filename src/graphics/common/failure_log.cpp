#include "xenon/gpu/failure_log.hpp"

namespace xenon::gpu {

std::uint64_t GpuFailureLog::record(std::string_view message, bool resolve) {
  std::scoped_lock lock(mutex_);
  for (auto& reason : reasons_) {
    if (reason.resolve == resolve && reason.message == message) return ++reason.count;
  }
  if (reasons_.size() >= kMaxReasons) {
    ++unlisted_;
    return 0u;
  }
  reasons_.push_back(GpuFailureReason{std::string(message), resolve, 1u});
  return 1u;
}

std::vector<GpuFailureReason> GpuFailureLog::reasons() const {
  std::scoped_lock lock(mutex_);
  return reasons_;
}

std::uint64_t GpuFailureLog::unlisted() const {
  std::scoped_lock lock(mutex_);
  return unlisted_;
}

}  // namespace xenon::gpu

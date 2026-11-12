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

bool GpuResolveCensus::record(bool depth, std::uint8_t source_format,
                              std::uint8_t destination_format, std::uint32_t width,
                              std::uint32_t height, bool all_zero,
                              std::uint32_t destination, bool single_lane) {
  std::scoped_lock lock(mutex_);
  GpuResolveKind* kind = nullptr;
  for (auto& existing : kinds_) {
    if (existing.depth == depth && existing.source_format == source_format &&
        existing.destination_format == destination_format && existing.width == width &&
        existing.height == height) {
      kind = &existing;
      break;
    }
  }
  if (!kind) {
    if (kinds_.size() >= kMaxKinds) {
      ++unlisted_;
      return false;
    }
    kind = &kinds_.emplace_back(GpuResolveKind{depth, source_format, destination_format,
                                               width, height, 0u, 0u, 0u, 0u});
  }
  ++kind->count;
  kind->last_destination = destination;
  if (single_lane) ++kind->single_lane;
  if (!all_zero) return false;
  return ++kind->all_zero == 1u;
}

std::vector<GpuResolveKind> GpuResolveCensus::kinds() const {
  std::scoped_lock lock(mutex_);
  return kinds_;
}

std::uint64_t GpuResolveCensus::unlisted() const {
  std::scoped_lock lock(mutex_);
  return unlisted_;
}

}  // namespace xenon::gpu

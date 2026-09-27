#include "xenon/core/boot_checkpoints.hpp"

namespace xenon::core {

std::string_view to_string(BootCheckpoint checkpoint) noexcept {
  switch (checkpoint) {
    case BootCheckpoint::XexLoaded: return "XEX_LOADED";
    case BootCheckpoint::EntryStarted: return "ENTRY_STARTED";
    case BootCheckpoint::FirstGuestThread: return "FIRST_GUEST_THREAD";
    case BootCheckpoint::FirstFileOpen: return "FIRST_FILE_OPEN";
    case BootCheckpoint::FirstInputPoll: return "FIRST_INPUT_POLL";
    case BootCheckpoint::FirstAudioClient: return "FIRST_AUDIO_CLIENT";
    case BootCheckpoint::FirstGpuSubmission: return "FIRST_GPU_SUBMISSION";
    case BootCheckpoint::FirstShader: return "FIRST_SHADER";
    case BootCheckpoint::FirstResolve: return "FIRST_RESOLVE";
    case BootCheckpoint::FirstPresent: return "FIRST_PRESENT";
    case BootCheckpoint::FirstVblank: return "FIRST_VBLANK";
    case BootCheckpoint::ProfileReady: return "PROFILE_READY";
    case BootCheckpoint::SaveEnumeration: return "SAVE_ENUMERATION";
    case BootCheckpoint::Count: break;
  }
  return "UNKNOWN";
}

bool BootCheckpointTracker::reach(BootCheckpoint checkpoint) {
  const auto index = static_cast<std::size_t>(checkpoint);
  if (index >= reached_.size()) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (reached_[index]) return false;
  reached_[index] = true;
  order_.push_back(checkpoint);
  return true;
}

bool BootCheckpointTracker::reached(BootCheckpoint checkpoint) const noexcept {
  const auto index = static_cast<std::size_t>(checkpoint);
  if (index >= reached_.size()) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  return reached_[index];
}

std::vector<BootCheckpoint> BootCheckpointTracker::reached_in_order() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return order_;
}

void BootCheckpointTracker::reset() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  reached_.fill(false);
  order_.clear();
}

}  // namespace xenon::core

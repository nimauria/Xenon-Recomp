#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

#include "xenon/kernel/object.hpp"

namespace xenon::kernel {

class KernelEvent final : public KernelObject {
 public:
  explicit KernelEvent(bool manual_reset = false, bool initial_state = false);

  void set();
  void reset();
  [[nodiscard]] bool signaled() const;
  [[nodiscard]] bool wait_for(std::chrono::milliseconds timeout);

 private:
  bool manual_reset_{};
  mutable std::mutex mutex_{};
  std::condition_variable condition_{};
  bool signaled_{};
};

}  // namespace xenon::kernel

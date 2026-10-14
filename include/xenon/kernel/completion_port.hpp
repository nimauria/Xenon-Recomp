#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

#include "xenon/kernel/event.hpp"
#include "xenon/kernel/object.hpp"

namespace xenon::kernel {

struct CompletionPacket {
  std::uint64_t key{};
  std::uint64_t context{};
  IoStatus status{};
};

class IoCompletionPort final : public KernelObject {
 public:
  IoCompletionPort();

  void post(CompletionPacket packet);
  [[nodiscard]] bool try_remove(CompletionPacket& out_packet);
  [[nodiscard]] bool remove_for(std::chrono::milliseconds timeout,
                                CompletionPacket& out_packet);
  [[nodiscard]] std::size_t pending_count() const;

  // A manual-reset event signalled while packets are queued, so a guest
  // waiting for a packet (NtRemoveIoCompletion) waits through the kernel
  // wait machinery and can be interrupted like any other wait.
  [[nodiscard]] const std::shared_ptr<KernelEvent>& ready_event() const noexcept { return ready_; }

 private:
  mutable std::mutex mutex_{};
  std::condition_variable condition_{};
  std::deque<CompletionPacket> queue_{};
  std::shared_ptr<KernelEvent> ready_{std::make_shared<KernelEvent>(/*manual_reset=*/true, false)};
};

}  // namespace xenon::kernel

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>

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

 private:
  mutable std::mutex mutex_{};
  std::condition_variable condition_{};
  std::deque<CompletionPacket> queue_{};
};

}  // namespace xenon::kernel

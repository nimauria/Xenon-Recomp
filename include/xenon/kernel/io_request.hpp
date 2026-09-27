#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>

#include "xenon/kernel/io_types.hpp"

namespace xenon::kernel {

struct IoRequestSnapshot {
  std::uint64_t id{};
  IoOperation operation{IoOperation::Other};
  IoRequestState state{IoRequestState::Pending};
  std::uint64_t context{};
  IoStatus result{KernelIoCode::Pending};
};

class IoRequest {
 public:
  IoRequest(std::uint64_t id, IoOperation operation, std::uint64_t context);

  IoRequest(const IoRequest&) = delete;
  IoRequest& operator=(const IoRequest&) = delete;

  [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
  [[nodiscard]] IoOperation operation() const noexcept { return operation_; }
  [[nodiscard]] std::uint64_t context() const noexcept { return context_; }

  [[nodiscard]] IoRequestSnapshot snapshot() const;
  [[nodiscard]] bool complete(IoStatus result);
  [[nodiscard]] bool cancel();
  [[nodiscard]] IoStatus wait() const;

 private:
  const std::uint64_t id_{};
  const IoOperation operation_{IoOperation::Other};
  const std::uint64_t context_{};
  mutable std::mutex mutex_{};
  mutable std::condition_variable condition_{};
  IoRequestState state_{IoRequestState::Pending};
  IoStatus result_{KernelIoCode::Pending};
};

}  // namespace xenon::kernel

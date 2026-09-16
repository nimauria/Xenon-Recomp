#pragma once

#include <stdexcept>
#include <string>

#include "xenon/memory/types.hpp"

namespace xenon::memory {

class MemoryFault final : public std::runtime_error {
 public:
  MemoryFault(GuestAddress address, std::size_t width, AccessKind access,
              FaultReason reason, std::string message)
      : std::runtime_error(std::move(message)),
        address_(address),
        width_(width),
        access_(access),
        reason_(reason) {}

  [[nodiscard]] GuestAddress address() const noexcept { return address_; }
  [[nodiscard]] std::size_t width() const noexcept { return width_; }
  [[nodiscard]] AccessKind access() const noexcept { return access_; }
  [[nodiscard]] FaultReason reason() const noexcept { return reason_; }

 private:
  GuestAddress address_{};
  std::size_t width_{};
  AccessKind access_{};
  FaultReason reason_{};
};

}  // namespace xenon::memory

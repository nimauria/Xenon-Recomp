#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "xenon/memory/types.hpp"

namespace xenon::memory {

// Structured Xbox-visible memory fault state. This record deliberately contains
// only canonical Xenon memory semantics; host SIGSEGV/SEH details belong in the
// host-VM layer and must never leak into guest exception policy.
struct MemoryFaultInfo {
  // The original guest access requested by the CPU/runtime.
  GuestAddress request_address{};
  std::size_t width{};
  AccessKind access{AccessKind::Read};

  // The exact guest address/page that caused the access to fail. For a
  // cross-page access this may differ from request_address.
  GuestAddress fault_address{};
  FaultReason reason{FaultReason::Unmapped};

  // Address-space and page-table state captured at fault time.
  bool region_present{};
  bool mapped{};
  bool committed{};
  bool mmio{};
  RegionKind region_kind{RegionKind::Virtual};
  PageState page_state{PageState::Free};
  Protect allocation_protect{Protect::None};
  Protect current_protect{Protect::None};
  std::uint32_t page_size{};
  std::uint32_t physical_address{0xFFFFFFFFu};

  // Canonical cache/device classification used by ordering and coherency.
  MemoryType memory_type{MemoryType::NormalCached};
  HostMappingPolicy host_policy{HostMappingPolicy::DefaultCachedShared};

  [[nodiscard]] bool is_read() const noexcept {
    return access == AccessKind::Read;
  }
  [[nodiscard]] bool is_write() const noexcept {
    return access == AccessKind::Write;
  }
  [[nodiscard]] bool is_execute() const noexcept {
    return access == AccessKind::Execute;
  }
};

class MemoryFault final : public std::runtime_error {
 public:
  MemoryFault(MemoryFaultInfo info, std::string message)
      : std::runtime_error(std::move(message)), info_(info) {}

  // Backward-compatible accessors retained for tests/tooling that used the V1
  // exception shape. address() remains the address that actually faulted.
  [[nodiscard]] GuestAddress address() const noexcept {
    return info_.fault_address;
  }
  [[nodiscard]] GuestAddress request_address() const noexcept {
    return info_.request_address;
  }
  [[nodiscard]] GuestAddress fault_address() const noexcept {
    return info_.fault_address;
  }
  [[nodiscard]] std::size_t width() const noexcept { return info_.width; }
  [[nodiscard]] AccessKind access() const noexcept { return info_.access; }
  [[nodiscard]] FaultReason reason() const noexcept { return info_.reason; }
  [[nodiscard]] const MemoryFaultInfo& info() const noexcept { return info_; }

 private:
  MemoryFaultInfo info_{};
};

}  // namespace xenon::memory

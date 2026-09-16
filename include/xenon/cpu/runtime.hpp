#pragma once

#include <cstdint>

#include "xenon/cpu/memory_port.hpp"
#include "xenon/cpu/state.hpp"

namespace xenon::cpu {

enum class FlowReason : std::uint8_t {
  Fallthrough,
  Branch,
  Return,
  Trap,
  Syscall,
  Halt,
};

struct ExecutionResult {
  FlowReason reason{FlowReason::Fallthrough};
  GuestAddress next_address{};
  std::uint32_t detail{};

  [[nodiscard]] constexpr bool terminal() const noexcept {
    return reason == FlowReason::Trap || reason == FlowReason::Halt;
  }
};

// Host/runtime services reachable from compiled CPU code. This is deliberately
// not a kernel implementation: it is the narrow boundary used by generated
// code for control transfers and architecturally-visible privileged events.
class RuntimeServices {
 public:
  virtual ~RuntimeServices() = default;

  // Invoke already-compiled guest code at target and return after the guest
  // call returns. Implementations must not interpret PPC instructions.
  virtual ExecutionResult call(GuestAddress target, CpuState& state,
                               MemoryPort& memory) = 0;

  virtual ExecutionResult syscall(std::uint32_t level, CpuState& state,
                                  MemoryPort& memory) = 0;
  virtual ExecutionResult trap(std::uint32_t trap_code, CpuState& state,
                               MemoryPort& memory) = 0;

  // Rare/uncommon SPRs stay out of CpuState and are delegated explicitly.
  virtual std::uint64_t read_spr(std::uint32_t spr, const CpuState& state) = 0;
  virtual void write_spr(std::uint32_t spr, std::uint64_t value,
                         CpuState& state) = 0;

  // Read the architected guest time base. The platform/runtime owns how guest
  // time advances; compiled CPU code must not substitute the host TSC or wall
  // clock directly. Keeping this at the runtime boundary also makes tests and
  // deterministic replay possible without changing CPU IR.
  virtual std::uint64_t read_time_base(const CpuState& state) = 0;
};

class NullRuntimeServices final : public RuntimeServices {
 public:
  ExecutionResult call(GuestAddress target, CpuState&, MemoryPort&) override {
    return {FlowReason::Branch, target, 0};
  }
  ExecutionResult syscall(std::uint32_t level, CpuState& state,
                          MemoryPort&) override {
    return {FlowReason::Syscall, state.cia + 4u, level};
  }
  ExecutionResult trap(std::uint32_t trap_code, CpuState& state,
                       MemoryPort&) override {
    return {FlowReason::Trap, state.cia, trap_code};
  }
  std::uint64_t read_spr(std::uint32_t, const CpuState&) override { return 0; }
  void write_spr(std::uint32_t, std::uint64_t, CpuState&) override {}
  std::uint64_t read_time_base(const CpuState& state) override {
    return state.time_base;
  }
};

}  // namespace xenon::cpu

#pragma once

#include <cstdint>
#include <string_view>

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

  // CPU-v1 compatibility hook for imported platform calls. CPU v2 may replace
  // how imports reach this boundary, but subsystem handlers remain unchanged.
  virtual bool external_call(std::string_view module, std::uint32_t ordinal,
                             CpuState& state, MemoryPort& memory) {
    static_cast<void>(module); static_cast<void>(ordinal);
    static_cast<void>(state); static_cast<void>(memory); return false;
  }
};

// CPU V2 native-entry ABI. A caller creates one context for a guest execution
// chain and compiled functions reuse its Memory V2 access view rather than
// reacquiring the legacy polymorphic MemoryPort boundary on every guest call.
struct ExecutionContext;
using NativeCompiledEntry = ExecutionResult (*)(ExecutionContext&);
enum class CompiledLookupKind : std::uint8_t {
  Branch,
  Call,
};
using CompiledLookupCallback = NativeCompiledEntry (*)(
    void* registry, ExecutionContext& context, GuestAddress target,
    CompiledLookupKind kind);

struct ExecutionContext {
  CpuState& state;
  MemoryPort& memory;
  RuntimeServices& runtime;
  MemoryAccessContext memory_access;
  void* compiled_registry{};
  CompiledLookupCallback compiled_lookup{};

  [[nodiscard]] NativeCompiledEntry lookup_compiled(
      GuestAddress target,
      CompiledLookupKind kind = CompiledLookupKind::Branch) {
    return compiled_lookup ? compiled_lookup(compiled_registry, *this, target,
                                               kind)
                           : nullptr;
  }

  ExecutionContext(CpuState& state_in, MemoryPort& memory_in,
                   RuntimeServices& runtime_in) noexcept
      : state(state_in),
        memory(memory_in),
        runtime(runtime_in),
        memory_access(memory_in.access_context()) {}

  ExecutionContext(const ExecutionContext&) = delete;
  ExecutionContext& operator=(const ExecutionContext&) = delete;
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

#pragma once

#include "xenon/cpu/external_calls.hpp"
#include "xenon/cpu/runtime.hpp"

namespace xenon::cpu {

// CPU-v1 convenience runtime. Generated/recompiled import thunks can call
// external_call today; CPU v2 can later provide its own RuntimeServices while
// reusing ExternalCallRegistry and all registered subsystem handlers.
class RegistryRuntimeServices : public RuntimeServices {
 public:
  explicit RegistryRuntimeServices(ExternalCallRegistry& registry) : registry_(registry) {}
  ExecutionResult call(GuestAddress target, CpuState&, MemoryPort&) override { return {FlowReason::Branch,target,0}; }
  ExecutionResult syscall(std::uint32_t level, CpuState& state, MemoryPort&) override { return {FlowReason::Syscall,state.cia+4u,level}; }
  ExecutionResult trap(std::uint32_t code, CpuState& state, MemoryPort&) override { return {FlowReason::Trap,state.cia,code}; }
  std::uint64_t read_spr(std::uint32_t, const CpuState&) override { return 0; }
  void write_spr(std::uint32_t, std::uint64_t, CpuState&) override {}
  std::uint64_t read_time_base(const CpuState& state) override { return state.time_base; }
  bool external_call(std::string_view module, std::uint32_t ordinal, CpuState& state, MemoryPort& memory) override {
    return registry_.dispatch(module,ordinal,state,memory);
  }
 private:
  ExternalCallRegistry& registry_;
};

}  // namespace xenon::cpu

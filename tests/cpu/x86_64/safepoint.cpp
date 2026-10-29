// Runs generated guest loops with the thread's safepoint flag set and clear.
//
// Regression (AC6): a guest thread spinning inside one compiled function (the
// audio threads' per-processor rendezvous) could not be suspended or
// terminated, because only the dispatcher between functions checked. Session
// shutdown then unloaded the game module while the thread still ran its code.
// Every loop back-edge now polls the flag.

#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>

#include "xenon/cpu/aot_semantics.hpp"
#include "xenon/cpu/flat_memory.hpp"

using namespace xenon::cpu;

ExecutionResult safepoint_spin_v2(ExecutionContext&);
ExecutionResult safepoint_count_v2(ExecutionContext&);

namespace {

struct Stopped {};

// Counts safepoint entries; either unwinds (terminate) or clears the flag and
// returns (a suspended thread being resumed).
class SafepointRuntime final : public RuntimeServices {
 public:
  std::atomic<bool> flag{false};
  int entries = 0;
  bool unwind = true;

  void enter_safepoint(CpuState&) override {
    ++entries;
    if (unwind) throw Stopped{};
    flag.store(false);
  }
  ExecutionResult call(GuestAddress, CpuState&, MemoryPort&) override { return {FlowReason::Trap, 0, 0}; }
  ExecutionResult syscall(std::uint32_t, CpuState&, MemoryPort&) override { return {FlowReason::Trap, 0, 0}; }
  ExecutionResult trap(std::uint32_t, CpuState&, MemoryPort&) override { return {FlowReason::Trap, 0, 0}; }
  std::uint64_t read_spr(std::uint32_t, const CpuState&) override { return 0; }
  void write_spr(std::uint32_t, std::uint64_t, CpuState&) override {}
  std::uint64_t read_time_base(const CpuState&) override { return 0; }
};

}  // namespace

int main() {
  FlatMemory memory(0x100u, 0x1000u);

  // Flag clear: a loop runs to completion without entering the runtime.
  {
    SafepointRuntime runtime;
    CpuState state{};
    state.lr = 0x9000u;
    ExecutionContext context(state, memory, runtime);
    context.safepoint_flag = &runtime.flag;
    const auto result = safepoint_count_v2(context);
    assert(result.reason == FlowReason::Return && result.next_address == 0x9000u);
    assert(state.gpr[3] == 3u && runtime.entries == 0);
  }

  // Flag set on an endless loop: the thread leaves compiled code at the
  // back-edge, with nia at the loop head.
  {
    SafepointRuntime runtime;
    runtime.flag.store(true);
    CpuState state{};
    ExecutionContext context(state, memory, runtime);
    context.safepoint_flag = &runtime.flag;
    bool stopped = false;
    try {
      static_cast<void>(safepoint_spin_v2(context));
    } catch (const Stopped&) {
      stopped = true;
    }
    assert(stopped && runtime.entries == 1);
    assert(state.nia == 0x3000u);
  }

  // A thread resumed at the safepoint continues its loop where it was.
  {
    SafepointRuntime runtime;
    runtime.unwind = false;
    runtime.flag.store(true);
    CpuState state{};
    state.lr = 0x9000u;
    ExecutionContext context(state, memory, runtime);
    context.safepoint_flag = &runtime.flag;
    const auto result = safepoint_count_v2(context);
    assert(result.reason == FlowReason::Return && state.gpr[3] == 3u);
    assert(runtime.entries == 1);
  }

  // Code without a flag (a runtime that never preempts) never polls.
  {
    SafepointRuntime runtime;
    CpuState state{};
    state.lr = 0x9000u;
    ExecutionContext context(state, memory, runtime);
    static_cast<void>(safepoint_count_v2(context));
    assert(state.gpr[3] == 3u && runtime.entries == 0);
  }

  std::cout << "Safepoint codegen tests passed\n";
  return 0;
}

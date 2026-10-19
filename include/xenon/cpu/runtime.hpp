#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

#include "xenon/cpu/memory_port.hpp"
#include "xenon/cpu/state.hpp"

namespace xenon::kernel { class KernelProcess; }

namespace xenon::cpu {

enum class FlowReason : std::uint8_t {
  Fallthrough,
  Branch,
  Return,
  Trap,
  Syscall,
  Halt,
  // Explicit guest non-local transfer used by the Xenon setjmp/longjmp
  // helpers. This propagates through generated guest-call frames until the
  // compiled function that contains next_address resumes at that local block.
  LongJump,
};

[[nodiscard]] constexpr std::string_view flow_reason_name(
    FlowReason reason) noexcept {
  switch (reason) {
    case FlowReason::Fallthrough: return "Fallthrough";
    case FlowReason::Branch: return "Branch";
    case FlowReason::Return: return "Return";
    case FlowReason::Trap: return "Trap";
    case FlowReason::Syscall: return "Syscall";
    case FlowReason::Halt: return "Halt";
    case FlowReason::LongJump: return "LongJump";
  }
  return "Unknown";
}

struct ExecutionResult {
  FlowReason reason{FlowReason::Fallthrough};
  GuestAddress next_address{};
  std::uint32_t detail{};

  [[nodiscard]] constexpr bool terminal() const noexcept {
    return reason == FlowReason::Trap || reason == FlowReason::Halt;
  }
};

// Thrown by RuntimeServices::enter_safepoint() to take a terminated guest
// thread out of compiled code; the dispatcher that runs the thread catches it.
// Deliberately not a std::exception, so no generic handler mistakes it for a
// fault.
struct GuestThreadTerminated {};

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

  // Returns the process that owns the currently executing guest thread when
  // the runtime has one. CPU code only observes the opaque pointer; kernel-
  // owned native helpers use it for process-lifetime services such as the
  // guest heap. Test/minimal runtimes may leave this null.
  virtual xenon::kernel::KernelProcess* current_process() noexcept { return nullptr; }

  // Called by compiled code at a loop back-edge once the executing thread's
  // safepoint flag (ExecutionContext::safepoint_flag) is set: a suspended
  // thread parks here, and a terminated one leaves compiled code by throwing
  // GuestThreadTerminated. A runtime that never sets the flag is never
  // called, which is why this default does nothing.
  virtual void enter_safepoint(CpuState& state) { static_cast<void>(state); }

  // CPU-v1 compatibility hook for imported platform calls. CPU v2 may replace
  // how imports reach this boundary, but subsystem handlers remain unchanged.
  virtual bool external_call(std::string_view module, std::uint32_t ordinal,
                             CpuState& state, MemoryPort& memory) {
    static_cast<void>(module); static_cast<void>(ordinal);
    static_cast<void>(state); static_cast<void>(memory); return false;
  }

  // True iff `target` is a callable XEX-native import thunk address this
  // runtime already resolves via call() (matching an import's guest_thunk).
  // A recognized import thunk's bytes are loader-owned placeholder metadata
  // (ordinal/attributes/record-type), never real guest PPC - a caller that
  // might otherwise try to decode/interpret bytes at an arbitrary executable
  // address (e.g. the Gen 7 dynamic-fallback interpreter) must check this
  // FIRST and route straight to call() instead, the same way the Recomp
  // Driver's static analysis already excludes import-thunk addresses from
  // decode (see driver.cpp's find_callable_import_thunk()). Deliberately
  // kept out of the hot compiled-call path (call() itself does not consult
  // this - it already checks imports directly, in the right order for that
  // path's own performance characteristics); this exists only for callers
  // that would otherwise attempt to interpret raw guest bytes. Default false
  // for minimal/test runtimes with no XEX import table at all.
  virtual bool is_recognized_import_thunk(GuestAddress target) {
    static_cast<void>(target);
    return false;
  }
};

// Runs the import thunk `target` reached by an UNLINKED branch, such as the
// `b thunk` at the end of a title's argument-shuffling wrapper. call()
// reports a completed import as Fallthrough at state.cia, which only a linked
// call site may use: it knows its own return address. Here state.cia is the
// branch itself, so forwarding Fallthrough re-executes the branch and runs the
// export a second time (AC6 created two sockets and bound one twice). A tail
// branch never gets control back: the import's completion is the branching
// function's own return to the untouched LR, as the console thunk's bctr is.
[[nodiscard]] inline ExecutionResult call_import_from_branch(RuntimeServices& runtime,
                                                             GuestAddress target,
                                                             CpuState& state,
                                                             MemoryPort& memory) {
  auto result = runtime.call(target, state, memory);
  if (result.reason == FlowReason::Fallthrough)
    result = {FlowReason::Return, static_cast<GuestAddress>(state.lr & ~3ull), 0u};
  return result;
}

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

// Optional Gen 7 safety-net path for executable guest targets that were not
// present in the static compiled registry. The callback is deliberately
// separate from compiled_lookup: AOT remains authoritative and pays only one
// extra null check on a genuine miss.
struct DynamicFallbackResult {
  bool handled{};
  ExecutionResult result{};
};
using DynamicFallbackCallback = DynamicFallbackResult (*)(
    void* executor, ExecutionContext& context, GuestAddress target,
    CompiledLookupKind kind);
// Optional observation hook for adaptive static recompilation. It fires only
// when a compiled lookup misses, so the hot path for already-known compiled
// edges remains a single null check and projects can persist only genuinely
// new runtime-discovered targets for the next analysis pass.
using CompiledLookupMissCallback = void (*)(
    void* observer, ExecutionContext& context, GuestAddress target,
    CompiledLookupKind kind);

struct ExecutionContext {
  CpuState& state;
  MemoryPort& memory;
  RuntimeServices& runtime;
  MemoryAccessContext memory_access;
  void* compiled_registry{};
  CompiledLookupCallback compiled_lookup{};
  void* compiled_lookup_observer{};
  CompiledLookupMissCallback compiled_lookup_miss{};
  void* dynamic_fallback_executor{};
  DynamicFallbackCallback dynamic_fallback{};
  // Set while the executing guest thread has been asked to suspend or
  // terminate. Compiled loops poll it at every backward branch, so a thread
  // spinning inside one compiled function stays preemptible, as on hardware.
  // Appended last so modules built against the earlier layout still work.
  const std::atomic<bool>* safepoint_flag{};

  // A loop back-edge: hand the thread to the runtime if it must stop.
  void poll_safepoint() {
    if (safepoint_flag && safepoint_flag->load(std::memory_order_relaxed)) [[unlikely]]
      runtime.enter_safepoint(state);
  }

  [[nodiscard]] NativeCompiledEntry lookup_compiled(
      GuestAddress target,
      CompiledLookupKind kind = CompiledLookupKind::Branch) {
    auto* entry = compiled_lookup
                      ? compiled_lookup(compiled_registry, *this, target, kind)
                      : nullptr;
    if (!entry && compiled_lookup_miss)
      compiled_lookup_miss(compiled_lookup_observer, *this, target, kind);
    return entry;
  }

  [[nodiscard]] DynamicFallbackResult try_dynamic_fallback(
      GuestAddress target,
      CompiledLookupKind kind = CompiledLookupKind::Branch) {
    return dynamic_fallback
               ? dynamic_fallback(dynamic_fallback_executor, *this, target, kind)
               : DynamicFallbackResult{};
  }

  ExecutionContext(CpuState& state_in, MemoryPort& memory_in,
                   RuntimeServices& runtime_in) noexcept
      : state(state_in),
        memory(memory_in),
        runtime(runtime_in),
        memory_access(memory_in.access_context()) {
    // This context, and memory_access with it, lives for an entire guest
    // thread's run - not one memory access. Drop the constructor-acquired
    // standing read guard immediately: MemoryAccessContext's own accessors
    // each take their own short-lived guard around just their resolve-and-
    // touch, which is what physical-page reclamation actually needs to see
    // reach zero. Holding this guard for the thread's whole lifetime would
    // starve reclaim of every page the guest frees for as long as any guest
    // thread is alive - i.e. permanently during normal play.
    memory_access.detach_standing_guard();
  }

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

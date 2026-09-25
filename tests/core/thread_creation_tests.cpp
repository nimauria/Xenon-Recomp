// Phase 1/2 (AC6 Runtime Readiness pass): ExCreateThread and the shared
// dispatch_guest_thread()/run_created_guest_thread() primitive it needs.
//
// Also regression-covers a real bug found while building this: XenonSession
// ::external_call() (the real production dispatch path for every guest
// export call) hardcoded ExportCallContext::thread_id to 0 regardless of
// which guest thread was actually calling - meaning NtCreateMutant/
// NtWaitForSingleObjectEx's real thread-identity semantics (fixed earlier
// this pass) worked in isolated export tests but were never actually
// exercised with a real calling thread id in production until this fix.

#include "xenon/core/session.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

using xenon::cpu::CompiledLookupKind;
using xenon::cpu::ExecutionContext;
using xenon::cpu::ExecutionResult;
using xenon::cpu::FlowReason;
using xenon::cpu::GuestAddress;

namespace xenon::core {

// Same pattern as tests/core/session_execution_tests.cpp's
// SessionExecutionTestAccess, defined independently in this translation
// unit (each test executable is a separate program, so there is no ODR
// conflict) with the accessors this file specifically needs: a
// kernel_process_ (ExCreateThread requires one) and direct calls to the
// private export_ex_create_thread()/external_call() methods, bypassing the
// full init_exports()/audio/input/xam wiring this test has no need for.
struct SessionExecutionTestAccess {
  static void configure(XenonSession& session,
                        std::shared_ptr<xenon::memory::AddressSpace> memory,
                        std::function<void(ExecutionContext&)> binder) {
    // The preemption tests below deliberately run a tight guest loop for
    // ~1000+ dispatch iterations; per-dispatch logging (SessionConfig's
    // default) would otherwise produce megabytes of test output for no
    // diagnostic value.
    session.config_.enable_logging = false;
    session.memory_ = memory;
    session.main_cpu_state_ = std::make_unique<xenon::cpu::CpuState>();
    session.loaded_xex_.emplace();
    session.loaded_xex_->image.entry_point = 0x1000u;
    session.compiled_registry_binder_ = std::move(binder);

    // Real guest process/thread model, mirroring create_guest_process()'s
    // own setup (minus actually parsing a XEX) - ExCreateThread needs
    // kernel_process_ to exist.
    session.kernel_memory_ = std::make_shared<kernel::KernelMemory>(memory);
    session.kernel_process_ = std::make_shared<kernel::KernelProcess>(session.kernel_memory_);
  }

  static bool ex_create_thread(XenonSession& session, ExportCallContext& context) {
    return session.export_ex_create_thread(context);
  }

  static bool external_call(XenonSession& session, std::string_view module,
                            std::uint32_t ordinal, cpu::CpuState& state,
                            cpu::MemoryPort& memory) {
    return session.external_call(module, ordinal, state, memory);
  }
};

}  // namespace xenon::core

using namespace xenon;

namespace {

struct Registry {
  std::unordered_map<GuestAddress, xenon::cpu::NativeCompiledEntry> entries;
};

xenon::cpu::NativeCompiledEntry lookup(void* opaque, ExecutionContext&, GuestAddress target,
                                       CompiledLookupKind) {
  const auto& registry = *static_cast<const Registry*>(opaque);
  const auto it = registry.entries.find(target);
  return it == registry.entries.end() ? nullptr : it->second;
}

std::atomic<bool> g_entry_ran{false};
std::atomic<std::uint64_t> g_observed_start_context{0};

// A well-behaved thread function: observes start_context (gpr[3]) and
// returns it incremented by one, matching real PPC ABI (gpr[3] is both the
// single argument ExCreateThread's StartContext passes and the function's
// return value).
ExecutionResult echo_increment_entry(ExecutionContext& context) {
  g_entry_ran.store(true);
  g_observed_start_context.store(context.state.gpr[3]);
  context.state.gpr[3] = context.state.gpr[3] + 1u;
  context.state.lr = 0u;
  return {FlowReason::Return, 0u, 0u};
}

ExecutionResult trapping_entry(ExecutionContext&) {
  g_entry_ran.store(true);
  return {FlowReason::Trap, 0u, 0xBADu};
}

std::atomic<std::uint64_t> g_loop_iterations{0};

// Branches to itself forever - a synthetic tight guest loop, so the
// preemptive safepoint (dispatch_guest_thread()'s per-block-boundary
// suspend/terminate check) has something real to interrupt.
//
// Throttled to roughly 20,000 dispatches/second: dispatch_guest_thread()'s
// top-level loop has a real, pre-existing kMaxTopLevelDispatches (1,000,000)
// safety cap intended for genuinely runaway compiled guest code. A trivial
// native lambda dispatched with no throttling reaches that cap in well under
// a second, which raced with (and intermittently pre-empted) this test's own
// explicit terminate() call - the thread would already be Terminated by the
// time the test called it, since hitting the cap falls through
// dispatch_guest_thread() as an ordinary "natural" exit. Real compiled guest
// code returns to the top-level dispatch loop far less often than every
// single instruction (only at actual function-call/return boundaries), so
// this cap is not reachable in practice at anywhere near this rate - only
// this synthetic microbenchmark-speed test loop can hit it artificially fast.
ExecutionResult looping_entry(ExecutionContext&) {
  g_loop_iterations.fetch_add(1, std::memory_order_relaxed);
  std::this_thread::sleep_for(std::chrono::microseconds(50));
  return {FlowReason::Branch, 0x3000u, 0u};
}

struct ThreadCreationHarness {
  xenon::core::XenonSession session;
  Registry registry;

  explicit ThreadCreationHarness(xenon::cpu::NativeCompiledEntry entry) {
    auto memory = std::make_shared<xenon::memory::AddressSpace>(
        xenon::memory::GuestTranslationMode::Compact);
    assert(memory->initialize());
    registry.entries.emplace(0x3000u, entry);
    xenon::core::SessionExecutionTestAccess::configure(
        session, std::move(memory), [this](ExecutionContext& context) {
      context.compiled_registry = &registry;
      context.compiled_lookup = &lookup;
    });
  }
};

xenon::core::ExportCallContext make_call(xenon::cpu::CpuState& cpu,
                                         xenon::memory::AddressSpace& memory) {
  return xenon::core::ExportCallContext{cpu, memory, 0, 0};
}

void test_ex_create_thread_runs_and_propagates_start_context_and_exit_code() {
  g_entry_ran.store(false);
  g_observed_start_context.store(0);
  ThreadCreationHarness harness(&echo_increment_entry);

  xenon::memory::GuestAddress handle_out{};
  xenon::memory::GuestAddress thread_id_out{};
  assert(harness.session.memory()->allocate(4, 4, xenon::memory::kReadWrite, false, handle_out));
  assert(harness.session.memory()->allocate(4, 4, xenon::memory::kReadWrite, false, thread_id_out));

  xenon::cpu::CpuState cpu{};
  cpu.gpr[3] = handle_out;
  cpu.gpr[4] = 0;             // default stack size
  cpu.gpr[5] = thread_id_out;
  cpu.gpr[7] = 0x3000u;       // start address
  cpu.gpr[8] = 0x41u;         // start context
  cpu.gpr[9] = 0;             // no creation flags
  auto call = make_call(cpu, *harness.session.memory());

  const bool handled = xenon::core::SessionExecutionTestAccess::ex_create_thread(
      harness.session, call);
  assert(handled);
  assert(cpu.gpr[3] == 0u);  // STATUS_SUCCESS

  const auto handle = harness.session.memory()->read32_be(handle_out);
  const auto reported_thread_id = harness.session.memory()->read32_be(thread_id_out);
  assert(handle != 0u);
  assert(reported_thread_id != 0u);

  kernel::HandleView view{};
  assert(harness.session.kernel_process()->handle_table().lookup(handle, view) ==
         kernel::KernelIoCode::Success);
  assert(view.object->type() == kernel::ObjectType::Thread);
  auto& thread = static_cast<kernel::KernelThread&>(*view.object);
  assert(thread.thread_id() == reported_thread_id);

  assert(thread.join(2000) && "created thread must actually finish and be joinable");
  assert(g_entry_ran.load());
  assert(g_observed_start_context.load() == 0x41u &&
         "start_context (gpr[8]) must arrive in the thread function as gpr[3]");
  assert(thread.exit_code() == 0x42u &&
         "the thread function's gpr[3] return value must become the exit code");
  assert(thread.state() == kernel::ThreadState::Terminated);
}

void test_ex_create_thread_honors_create_suspended() {
  g_entry_ran.store(false);
  ThreadCreationHarness harness(&echo_increment_entry);

  xenon::memory::GuestAddress handle_out{};
  assert(harness.session.memory()->allocate(4, 4, xenon::memory::kReadWrite, false, handle_out));

  xenon::cpu::CpuState cpu{};
  cpu.gpr[3] = handle_out;
  cpu.gpr[7] = 0x3000u;
  cpu.gpr[8] = 0;
  cpu.gpr[9] = 0x4u;  // CREATE_SUSPENDED
  auto call = make_call(cpu, *harness.session.memory());

  assert(xenon::core::SessionExecutionTestAccess::ex_create_thread(harness.session, call));
  assert(cpu.gpr[3] == 0u);
  const auto handle = harness.session.memory()->read32_be(handle_out);

  kernel::HandleView view{};
  assert(harness.session.kernel_process()->handle_table().lookup(handle, view) ==
         kernel::KernelIoCode::Success);
  auto& thread = static_cast<kernel::KernelThread&>(*view.object);

  assert(thread.state() == kernel::ThreadState::Suspended);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  assert(!g_entry_ran.load() && "CREATE_SUSPENDED must not run the thread function yet");

  assert(thread.resume());
  assert(thread.join(2000));
  assert(g_entry_ran.load());
}

void test_preemptive_safepoint_terminates_a_running_created_thread() {
  // Phase 2 of the AC6 Runtime Readiness pass: terminate() must interrupt a
  // thread actually executing guest code (not just one blocked in a wait),
  // via dispatch_guest_thread()'s per-block-boundary safepoint.
  g_loop_iterations.store(0);
  ThreadCreationHarness harness(&looping_entry);

  xenon::memory::GuestAddress handle_out{};
  assert(harness.session.memory()->allocate(4, 4, xenon::memory::kReadWrite, false, handle_out));
  xenon::cpu::CpuState cpu{};
  cpu.gpr[3] = handle_out;
  cpu.gpr[7] = 0x3000u;
  auto call = make_call(cpu, *harness.session.memory());
  assert(xenon::core::SessionExecutionTestAccess::ex_create_thread(harness.session, call));
  const auto handle = harness.session.memory()->read32_be(handle_out);

  kernel::HandleView view{};
  assert(harness.session.kernel_process()->handle_table().lookup(handle, view) ==
         kernel::KernelIoCode::Success);
  auto& thread = static_cast<kernel::KernelThread&>(*view.object);

  // Let it actually loop for a while before terminating it.
  while (g_loop_iterations.load(std::memory_order_relaxed) < 1000) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  assert(thread.terminate(0x7777u));

  const auto start = std::chrono::steady_clock::now();
  assert(thread.join(2000) &&
         "terminate() must stop a running dispatch loop via the safepoint, not hang forever");
  const auto elapsed = std::chrono::steady_clock::now() - start;
  assert(elapsed < std::chrono::milliseconds(500) &&
         "the safepoint must be observed within roughly one block's latency, not eventually");

  assert(thread.state() == kernel::ThreadState::Terminated);
  assert(thread.exit_code() == 0x7777u &&
         "terminate()'s exit code must be authoritative, not an in-flight register value");

  // Prove the guest dispatch loop actually stopped, not merely that join()
  // returned while a detached background loop kept running.
  const auto count_at_join = g_loop_iterations.load(std::memory_order_relaxed);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  assert(g_loop_iterations.load(std::memory_order_relaxed) == count_at_join &&
         "guest dispatch must have genuinely stopped after terminate(), not kept looping");
}

void test_preemptive_safepoint_suspends_and_resumes_a_running_created_thread() {
  g_loop_iterations.store(0);
  ThreadCreationHarness harness(&looping_entry);

  xenon::memory::GuestAddress handle_out{};
  assert(harness.session.memory()->allocate(4, 4, xenon::memory::kReadWrite, false, handle_out));
  xenon::cpu::CpuState cpu{};
  cpu.gpr[3] = handle_out;
  cpu.gpr[7] = 0x3000u;
  auto call = make_call(cpu, *harness.session.memory());
  assert(xenon::core::SessionExecutionTestAccess::ex_create_thread(harness.session, call));
  const auto handle = harness.session.memory()->read32_be(handle_out);

  kernel::HandleView view{};
  assert(harness.session.kernel_process()->handle_table().lookup(handle, view) ==
         kernel::KernelIoCode::Success);
  auto& thread = static_cast<kernel::KernelThread&>(*view.object);

  while (g_loop_iterations.load(std::memory_order_relaxed) < 1000) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  assert(thread.suspend());
  // Give the safepoint time to actually park the loop.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto count_after_suspend_settles = g_loop_iterations.load(std::memory_order_relaxed);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  assert(g_loop_iterations.load(std::memory_order_relaxed) == count_after_suspend_settles &&
         "a suspended running thread must stop dispatching, not keep looping in the background");

  assert(thread.resume());
  bool resumed_and_progressed = false;
  for (int i = 0; i < 200; ++i) {
    if (g_loop_iterations.load(std::memory_order_relaxed) > count_after_suspend_settles) {
      resumed_and_progressed = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  assert(resumed_and_progressed && "resume() must let the safepoint-parked thread continue dispatching");

  assert(thread.terminate(1u));
  assert(thread.join(2000));
}

void test_ex_create_thread_crash_produces_terminal_state_and_nonzero_exit() {
  g_entry_ran.store(false);
  ThreadCreationHarness harness(&trapping_entry);

  xenon::memory::GuestAddress handle_out{};
  assert(harness.session.memory()->allocate(4, 4, xenon::memory::kReadWrite, false, handle_out));

  xenon::cpu::CpuState cpu{};
  cpu.gpr[3] = handle_out;
  cpu.gpr[7] = 0x3000u;
  auto call = make_call(cpu, *harness.session.memory());

  assert(xenon::core::SessionExecutionTestAccess::ex_create_thread(harness.session, call));
  const auto handle = harness.session.memory()->read32_be(handle_out);

  kernel::HandleView view{};
  assert(harness.session.kernel_process()->handle_table().lookup(handle, view) ==
         kernel::KernelIoCode::Success);
  auto& thread = static_cast<kernel::KernelThread&>(*view.object);

  assert(thread.join(2000));
  assert(g_entry_ran.load());
  assert(thread.state() == kernel::ThreadState::Terminated);
  assert(thread.exit_code() != 0u &&
         "a crashing created thread must not silently report success");
}

void test_ex_create_thread_rejects_null_start_address() {
  ThreadCreationHarness harness(&echo_increment_entry);
  xenon::memory::GuestAddress handle_out{};
  assert(harness.session.memory()->allocate(4, 4, xenon::memory::kReadWrite, false, handle_out));

  xenon::cpu::CpuState cpu{};
  cpu.gpr[3] = handle_out;
  cpu.gpr[7] = 0;  // no start address
  auto call = make_call(cpu, *harness.session.memory());

  assert(xenon::core::SessionExecutionTestAccess::ex_create_thread(harness.session, call));
  assert(cpu.gpr[3] == 0xC000000Du);  // STATUS_INVALID_PARAMETER
}

void test_external_call_resolves_real_calling_thread_identity() {
  // Regression for external_call() previously hardcoding thread_id to 0.
  ThreadCreationHarness harness(&echo_increment_entry);

  std::uint32_t observed_thread_id = 0xFFFFFFFFu;
  core::ExportDescriptor descriptor{};
  descriptor.library = "test";
  descriptor.name = "Probe";
  descriptor.ordinal = 1;
  descriptor.requirement = core::ExportRequirement::Required;
  descriptor.handler = [&](core::ExportCallContext& ctx) -> bool {
    observed_thread_id = ctx.thread_id;
    return true;
  };
  assert(harness.session.exports()->register_export(std::move(descriptor)));

  kernel::ThreadCreationParams params{};
  auto probe_thread = harness.session.kernel_process()->thread_manager().create_thread(
      []() -> std::uint32_t { return 0; }, params);
  assert(probe_thread);
  harness.session.kernel_process()->thread_manager().set_current_thread(probe_thread);

  xenon::cpu::CpuState cpu{};
  const bool handled = xenon::core::SessionExecutionTestAccess::external_call(
      harness.session, "test", 1, cpu, *harness.session.memory());
  assert(handled);
  assert(observed_thread_id == probe_thread->thread_id() &&
         "external_call() must resolve the real calling thread's id, not hardcode 0");
}

}  // namespace

int main() {
  std::cout << "Testing ExCreateThread / guest thread creation...\n";

  test_ex_create_thread_runs_and_propagates_start_context_and_exit_code();
  test_ex_create_thread_honors_create_suspended();
  test_preemptive_safepoint_terminates_a_running_created_thread();
  test_preemptive_safepoint_suspends_and_resumes_a_running_created_thread();
  test_ex_create_thread_crash_produces_terminal_state_and_nonzero_exit();
  test_ex_create_thread_rejects_null_start_address();
  test_external_call_resolves_real_calling_thread_identity();

  std::cout << "All ExCreateThread tests passed!\n";
  return 0;
}

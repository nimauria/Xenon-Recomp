// GPU pump thread: the single combined host thread that drains a guest's
// Xenos PM4 command ring buffer into GraphicsSystem/Backend and, at ~60Hz,
// presents the current front buffer and fires the guest's registered vsync
// interrupt callback (XenonSession::start_gpu_pump_thread()/
// run_gpu_pump_thread()/invoke_gpu_interrupt_callback()/
// stop_gpu_pump_thread()).
//
// Before this pass, nothing in the live XenonSession ever called into the
// (fully implemented, isolation-tested) Xenos frontend/backend at all - a
// real game would boot with a black screen and no video, not because the
// GPU frontend was broken, but because nothing ever drove it. This file
// proves the missing wiring:
//
//   1. A session with graphics enabled starts the pump thread as part of the
//      real create_guest_process() production path, and shutdown() stops/
//      joins it cleanly with no hang - proving the lifecycle ordering
//      (start after init_gpu()+kernel_process_ exist; stop before
//      gpu_/graphics_system_/kernel_process_ are torn down) is correct.
//   2. Driving KernelProcess::configure_gpu_ring_buffer()/
//      set_gpu_ring_buffer_write_index() directly (simulating what the
//      guest's VdInitializeRingBuffer/VdSwap calls would do) is actually
//      observed and drained by the pump thread: gpu_ring_buffer().read_index
//      advances via GraphicsSystem::submit_ring()/execute_ir(), matching
//      what the pump thread committed.
//   3. Driving KernelProcess::set_gpu_front_buffer()/
//      set_gpu_interrupt_callback() causes the pump thread to (a) call
//      GraphicsSystem::present() against a NullBackend without crashing and
//      (b) actually invoke the registered guest interrupt callback with the
//      real VdSetGraphicsInterruptCallback ABI (r3 = 0 "normal interrupt",
//      r4 = the registered context) - via a synthetic "compiled" guest
//      function, mirroring tests/core/thread_creation_tests.cpp's
//      echo_increment_entry pattern rather than building/compiling a real
//      XEX (this test needs no recomp-driver/codegen pipeline).
//
// Every wait on the pump thread below is bounded - an unbounded wait here
// would turn a real lifecycle/shutdown-ordering bug into a test hang instead
// of a failure, which is exactly the failure mode this pass is most at risk
// of (see the audio callback thread's own shutdown-ordering history).

#include "xenon/core/session.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

using xenon::cpu::CompiledLookupKind;
using xenon::cpu::ExecutionContext;
using xenon::cpu::ExecutionResult;
using xenon::cpu::FlowReason;
using xenon::cpu::GuestAddress;

namespace xenon::core {

// Same pattern as tests/core/thread_creation_tests.cpp's
// SessionExecutionTestAccess, defined independently in this translation unit
// (each test executable is a separate program, so there is no ODR conflict)
// with the accessors this file specifically needs: enough private state to
// reach create_guest_process() without parsing a real XEX, plus direct
// access to the GPU pump thread's own bookkeeping so tests can assert on its
// lifecycle without relying only on indirect timing.
struct SessionExecutionTestAccess {
  static void configure(XenonSession& session,
                        std::shared_ptr<xenon::memory::AddressSpace> memory,
                        std::function<void(ExecutionContext&)> binder) {
    session.config_.enable_logging = false;
    session.config_.enable_graphics = true;
    session.config_.graphics_backend = "null";
    // This harness bypasses initialize()/load_game() entirely (see the class
    // doc comment), so state_ must be moved off Uninitialized by hand -
    // shutdown() no-ops immediately while state() reads Uninitialized (see
    // its own top-of-function guard), which would otherwise make every
    // "shutdown must stop/join cleanly" assertion below vacuously true.
    session.state_ = SessionState::Running;
    session.memory_ = memory;
    session.main_cpu_state_ = std::make_unique<xenon::cpu::CpuState>();
    session.loaded_xex_.emplace();
    session.loaded_xex_->image.entry_point = 0x1000u;
    session.loaded_xex_->image_base = 0x80000000u;
    session.compiled_registry_binder_ = std::move(binder);
    // Mirrors initialize()'s own ordering: init_gpu() must run before
    // create_guest_process() (it constructs gpu_/graphics_system_ and
    // allocates the pump thread's callback stack).
    assert(session.init_gpu() && "init_gpu() should succeed with the null backend");
  }

  static bool create_guest_process(XenonSession& session) {
    return session.create_guest_process();
  }

  [[nodiscard]] static bool gpu_pump_running(const XenonSession& session) noexcept {
    return session.gpu_pump_running_.load();
  }
  [[nodiscard]] static bool gpu_pump_thread_alive(const XenonSession& session) noexcept {
    return session.gpu_pump_thread_ != nullptr;
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

std::atomic<bool> g_gpu_interrupt_ran{false};
std::atomic<std::uint64_t> g_gpu_interrupt_r3{0xFFFFFFFFFFFFFFFFull};
std::atomic<std::uint64_t> g_gpu_interrupt_r4{0};

// A synthetic "compiled" guest interrupt callback: records the registers it
// was actually invoked with (proving the real VdSetGraphicsInterruptCallback
// ABI - r3 = 0/1 flag, r4 = context) and returns normally.
ExecutionResult gpu_interrupt_probe_entry(ExecutionContext& context) {
  g_gpu_interrupt_r3.store(context.state.gpr[3]);
  g_gpu_interrupt_r4.store(context.state.gpr[4]);
  g_gpu_interrupt_ran.store(true);
  context.state.gpr[3] = 0;
  context.state.lr = 0u;
  return {FlowReason::Return, 0u, 0u};
}

struct GpuPumpHarness {
  xenon::core::XenonSession session;
  Registry registry;

  explicit GpuPumpHarness(GuestAddress callback_addr = 0,
                          xenon::cpu::NativeCompiledEntry entry = nullptr) {
    auto memory = std::make_shared<xenon::memory::AddressSpace>(
        xenon::memory::GuestTranslationMode::Compact);
    assert(memory->initialize());
    if (entry) registry.entries.emplace(callback_addr, entry);
    xenon::core::SessionExecutionTestAccess::configure(
        session, std::move(memory), [this](ExecutionContext& context) {
      context.compiled_registry = &registry;
      context.compiled_lookup = &lookup;
    });
  }
};

// Runs `fn` on a background thread and waits up to `timeout` for it to
// finish, returning true if it completed in time. Used anywhere this file
// waits on the pump thread's shutdown/join path so a real lifecycle bug
// manifests as a failed assertion, not a hung test binary.
template <typename Fn>
bool run_with_timeout(Fn&& fn, std::chrono::milliseconds timeout) {
  auto promise = std::make_shared<std::promise<void>>();
  auto future = promise->get_future();
  std::thread worker([fn = std::forward<Fn>(fn), promise]() mutable {
    fn();
    promise->set_value();
  });
  const auto status = future.wait_for(timeout);
  if (status == std::future_status::ready) {
    worker.join();
    return true;
  }
  // Something is genuinely stuck - detach rather than block the test binary
  // forever on join(); the assertion failure below is what matters.
  worker.detach();
  return false;
}

// Test 1: a session with graphics enabled starts the GPU pump thread via the
// real create_guest_process() production path, and shutdown() stops/joins it
// cleanly with no hang.
void test_gpu_pump_thread_starts_and_stops_cleanly() {
  GpuPumpHarness harness;
  assert(xenon::core::SessionExecutionTestAccess::create_guest_process(harness.session) &&
         "create_guest_process() should succeed and start the GPU pump thread");
  assert(xenon::core::SessionExecutionTestAccess::gpu_pump_thread_alive(harness.session) &&
         "the GPU pump KernelThread should have been created");
  assert(xenon::core::SessionExecutionTestAccess::gpu_pump_running(harness.session) &&
         "the GPU pump loop should be marked running after a successful start");

  // Let it actually run for a little while before tearing down, so shutdown
  // has to interrupt a live loop, not just join a thread that never started.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const bool shutdown_completed = run_with_timeout(
      [&] { harness.session.shutdown(); }, std::chrono::milliseconds(5000));
  assert(shutdown_completed &&
         "shutdown() must stop and join the GPU pump thread without hanging");
  assert(!harness.session.is_initialized() && "session should be cleanly shut down");
  assert(!xenon::core::SessionExecutionTestAccess::gpu_pump_thread_alive(harness.session) &&
         "shutdown() must release the GPU pump KernelThread");
}

// Test 2: driving the ring-buffer state directly (simulating what the other
// workstream's VdInitializeRingBuffer/guest PM4 writes would do) is actually
// observed and drained by the pump thread.
void test_gpu_pump_thread_drains_ring_buffer() {
  GpuPumpHarness harness;
  assert(xenon::core::SessionExecutionTestAccess::create_guest_process(harness.session));

  auto* memory = harness.session.memory();
  assert(memory != nullptr);

  // A ring of 16 all-zero dwords: each zero dword decodes as a Xenos Type-0
  // packet (top 2 bits 00) writing register 0 with a zero payload dword -
  // completely harmless (no draws, no resource state that matters) but a
  // real, successfully-decoded PM4 stream, not garbage the decoder rejects.
  constexpr std::uint32_t kRingBase = 0x2000u;
  constexpr std::uint32_t kRingCapacityDwords = 16u;
  assert(memory->fill_physical(kRingBase, kRingCapacityDwords * 4u, std::byte{0}));

  auto* kernel_process = harness.session.kernel_process();
  assert(kernel_process != nullptr);
  kernel_process->configure_gpu_ring_buffer(kRingBase, kRingCapacityDwords);
  constexpr std::uint32_t kWriteIndex = 8u;  // 8 dwords available to drain.
  kernel_process->set_gpu_ring_buffer_write_index(kWriteIndex);

  bool drained = false;
  for (int i = 0; i < 400 && !drained; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    drained = kernel_process->gpu_ring_buffer().read_index == kWriteIndex;
  }
  assert(drained &&
         "the GPU pump thread should advance read_index to match write_index "
         "within a bounded timeout");

  const bool shutdown_completed = run_with_timeout(
      [&] { harness.session.shutdown(); }, std::chrono::milliseconds(5000));
  assert(shutdown_completed);
}

// Test 3: driving the front-buffer/interrupt-callback state directly proves
// (a) GraphicsSystem::present() against a NullBackend is exercised every
// vsync without crashing, and (b) the guest interrupt callback actually runs
// with the real ABI (r3 = 0, r4 = context).
void test_gpu_pump_thread_presents_and_fires_interrupt_callback() {
  g_gpu_interrupt_ran.store(false);
  g_gpu_interrupt_r3.store(0xFFFFFFFFFFFFFFFFull);
  g_gpu_interrupt_r4.store(0);

  constexpr GuestAddress kCallbackAddr = 0x5000u;
  constexpr std::uint32_t kContext = 0xCAFEBABEu;
  GpuPumpHarness harness(kCallbackAddr, &gpu_interrupt_probe_entry);
  assert(xenon::core::SessionExecutionTestAccess::create_guest_process(harness.session));

  auto* kernel_process = harness.session.kernel_process();
  assert(kernel_process != nullptr);
  kernel_process->set_gpu_front_buffer(/*base_address=*/0x3000u, /*width=*/64u,
                                       /*height=*/64u, /*pitch=*/256u,
                                       /*format=*/0u);
  kernel_process->set_gpu_interrupt_callback(kCallbackAddr, kContext);

  bool fired = false;
  for (int i = 0; i < 400 && !fired; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    fired = g_gpu_interrupt_ran.load();
  }
  assert(fired &&
         "the GPU pump thread should fire the registered vsync interrupt "
         "callback within a bounded timeout once a front buffer and "
         "callback are both registered");
  assert(g_gpu_interrupt_r3.load() == 0u &&
         "r3 must be the real VdSetGraphicsInterruptCallback 'normal "
         "interrupt' flag (0), not an arbitrary value");
  assert(g_gpu_interrupt_r4.load() == kContext &&
         "r4 must be the guest-registered context, matching the real ABI");

  const bool shutdown_completed = run_with_timeout(
      [&] { harness.session.shutdown(); }, std::chrono::milliseconds(5000));
  assert(shutdown_completed);
}

}  // namespace

int main() {
  std::cout << "Testing GPU pump thread (ring drain + vsync present/interrupt)...\n";

  test_gpu_pump_thread_starts_and_stops_cleanly();
  test_gpu_pump_thread_drains_ring_buffer();
  test_gpu_pump_thread_presents_and_fires_interrupt_callback();

  std::cout << "All GPU pump thread tests passed!\n";
  return 0;
}

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
#include "xenon/kernel/event.hpp"
#include "xenon/kernel/wait.hpp"
#include "xenon/logging/diagnostic_events.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
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
  [[nodiscard]] static CallbackWatch::Snapshot gpu_callback(const XenonSession& session) noexcept {
    return session.gpu_callback_watch_.snapshot();
  }
  [[nodiscard]] static CallbackWatch::Snapshot gpu_vsync_callback(
      const XenonSession& session) noexcept {
    return session.gpu_vsync_callback_watch_.snapshot();
  }
  [[nodiscard]] static std::uint64_t gpu_interrupts_delivered(const XenonSession& session) noexcept {
    return session.gpu_interrupts_delivered_.load();
  }
  [[nodiscard]] static std::string present_results(const XenonSession& session) {
    return session.describe_present_results();
  }
  // Replaces the backend init_gpu() chose; before create_guest_process().
  static void set_backend(XenonSession& session, std::unique_ptr<gpu::Backend> backend) {
    session.gpu_ = std::move(backend);
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

std::atomic<int> g_handoff_stage_a{0};
std::atomic<int> g_handoff_stage_b{0};

// First function of a two-function callback: like real compiled code, it ends by
// branching into a second function (a tail call across a function boundary).
ExecutionResult handoff_stage_a_entry(ExecutionContext&) {
  g_handoff_stage_a.fetch_add(1);
  return {FlowReason::Branch, 0x5100u, 0u};
}

// The second function - in Ace Combat 6's vsync callback this is where the
// callback releases the spin lock it took in the first.
ExecutionResult handoff_stage_b_entry(ExecutionContext& context) {
  g_handoff_stage_b.fetch_add(1);
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
  // VdEnableRingBufferRPtrWriteBack supplies a PHYSICAL address; the pump must
  // publish the read pointer there (regression: it used to treat it as a virtual
  // address, fault on a free page each drain, and never update the guest's copy).
  constexpr std::uint32_t kWritebackPhysical = 0x2400u;
  assert(memory->fill_physical(kWritebackPhysical, 4u, std::byte{0xEE}));
  kernel_process->set_gpu_ring_buffer_rptr_writeback(kWritebackPhysical);
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

  std::array<std::byte, 4> written{};
  bool published = false;
  for (int i = 0; i < 200 && !published; ++i) {
    assert(memory->copy_physical_range(kWritebackPhysical, written));
    published = written[3] == static_cast<std::byte>(kWriteIndex) &&
                written[0] == std::byte{0} && written[1] == std::byte{0} &&
                written[2] == std::byte{0};
    if (!published) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  assert(published && "the read pointer must be written, big-endian, to the physical writeback");

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

std::atomic<bool> g_saw_command_stream_interrupt{false};
std::atomic<std::uint64_t> g_command_stream_interrupt_r4{0};
std::atomic<std::uint32_t> g_command_stream_interrupt_cpu{0xFFu};

// Records whether the callback was ever entered with source 1 (a PM4_INTERRUPT), as
// opposed to source 0 (the vsync tick that keeps firing alongside it).
ExecutionResult interrupt_source_probe_entry(ExecutionContext& context) {
  if (context.state.gpr[3] == 1u) {
    g_command_stream_interrupt_r4.store(context.state.gpr[4]);
    // What the title's own handler reads: the KPCR's current-CPU byte (r13+0x10C).
    g_command_stream_interrupt_cpu.store(context.memory.read8(context.state.gpr[13] + 0x10Cu));
    g_saw_command_stream_interrupt.store(true);
  }
  context.state.gpr[3] = 0;
  context.state.lr = 0u;
  return {FlowReason::Return, 0u, 0u};
}

// Regression (Ace Combat 6): PM4_INTERRUPT packets were dropped because nothing
// installed the command processor's interrupt callback, so the title's graphics
// interrupt handler - which acknowledges GPU->CPU handshakes - never ran for them.
void test_pm4_interrupt_reaches_guest_callback_with_source_one() {
  g_saw_command_stream_interrupt.store(false);
  constexpr GuestAddress kCallbackAddr = 0x5000u;
  constexpr std::uint32_t kContext = 0xFEEDF00Du;
  GpuPumpHarness harness(kCallbackAddr, &interrupt_source_probe_entry);
  assert(xenon::core::SessionExecutionTestAccess::create_guest_process(harness.session));
  auto* memory = harness.session.memory();
  auto* kernel_process = harness.session.kernel_process();

  constexpr std::uint32_t kRingBase = 0x2000u;
  constexpr std::uint32_t kRingCapacityDwords = 16u;
  assert(memory->fill_physical(kRingBase, kRingCapacityDwords * 4u, std::byte{0}));
  // Type-3 INTERRUPT (opcode 0x54), one payload dword: CPU mask 0b100 = CPU 2.
  const std::array<std::byte, 8> packet{std::byte{0xC0}, std::byte{0x00}, std::byte{0x54},
                                        std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                                        std::byte{0x00}, std::byte{0x04}};
  assert(memory->write_physical(kRingBase, packet));
  kernel_process->configure_gpu_ring_buffer(kRingBase, kRingCapacityDwords);
  kernel_process->set_gpu_interrupt_callback(kCallbackAddr, kContext);
  kernel_process->set_gpu_ring_buffer_write_index(2u);

  bool seen = false;
  for (int i = 0; i < 400 && !seen; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    seen = g_saw_command_stream_interrupt.load();
  }
  assert(seen && "a PM4_INTERRUPT must invoke the callback with source 1");
  assert(g_command_stream_interrupt_r4.load() == kContext);
  assert(g_command_stream_interrupt_cpu.load() == 2u &&
         "the handler must see the interrupt's target CPU in the KPCR (its ack clears that CPU's bit)");

  const bool shutdown_completed = run_with_timeout(
      [&] { harness.session.shutdown(); }, std::chrono::milliseconds(5000));
  assert(shutdown_completed);
}

// Regression (Ace Combat 6 stalled at boot): a vsync callback that crosses a
// compiled-function boundary must run to completion. The pump used to execute only
// the callback's first function and count the handoff as success, so the tail of
// the callback - which released the spin lock the first part took - never ran and
// the next vsync deadlocked on its own lock.
void test_interrupt_callback_follows_function_handoffs() {
  g_handoff_stage_a.store(0);
  g_handoff_stage_b.store(0);

  constexpr GuestAddress kCallbackAddr = 0x5000u;
  GpuPumpHarness harness(kCallbackAddr, &handoff_stage_a_entry);
  harness.registry.entries.emplace(0x5100u, &handoff_stage_b_entry);
  assert(xenon::core::SessionExecutionTestAccess::create_guest_process(harness.session));

  auto* kernel_process = harness.session.kernel_process();
  assert(kernel_process != nullptr);
  kernel_process->set_gpu_front_buffer(0x3000u, 64u, 64u, 256u, 0u);
  kernel_process->set_gpu_interrupt_callback(kCallbackAddr, 0x1234u);

  bool finished = false;
  for (int i = 0; i < 400 && !finished; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    finished = g_handoff_stage_b.load() > 0;
  }
  assert(finished && "the second function of the callback must run");

  const bool shutdown_completed = run_with_timeout(
      [&] { harness.session.shutdown(); }, std::chrono::milliseconds(5000));
  assert(shutdown_completed);
}

std::atomic<int> g_blocking_calls{0};
std::atomic<bool> g_release_blocking{false};

// A vsync callback whose first call does not return until the test releases
// it, as a guest callback blocked in a wait or spinning on a lock would.
ExecutionResult blocking_callback_entry(ExecutionContext& context) {
  if (g_blocking_calls.fetch_add(1) == 0) {
    while (!g_release_blocking.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  context.state.gpr[3] = 0;
  context.state.lr = 0u;
  return {FlowReason::Return, 0u, 0u};
}

// A pump stuck inside a guest callback is visible while it is stuck: the
// always-on watch names the callback and how long it has run, and the
// diagnostic events show the callback as still open. Once it returns, the
// watch counts it as longer than a frame.
void test_a_blocked_callback_is_reported_while_it_blocks() {
  namespace events = xenon::logging::events;
  g_blocking_calls.store(0);
  g_release_blocking.store(false);
  events::set_enabled(true);
  events::clear();

  constexpr GuestAddress kCallbackAddr = 0x5200u;
  GpuPumpHarness harness(kCallbackAddr, &blocking_callback_entry);
  assert(xenon::core::SessionExecutionTestAccess::create_guest_process(harness.session));
  auto* kernel_process = harness.session.kernel_process();
  kernel_process->set_gpu_front_buffer(0x3000u, 64u, 64u, 256u, 0u);
  kernel_process->set_gpu_interrupt_callback(kCallbackAddr, 0u);

  for (int i = 0; i < 400 && g_blocking_calls.load() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  assert(g_blocking_calls.load() == 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));

  using xenon::core::CallbackWatch;
  const auto stuck = xenon::core::SessionExecutionTestAccess::gpu_vsync_callback(harness.session);
  const auto now = CallbackWatch::now_ns();
  assert(stuck.in_callback && stuck.entry == kCallbackAddr);
  assert(std::string(stuck.kind) == "vsync interrupt");
  assert(now - stuck.since_ns >= 50'000'000);
  assert(CallbackWatch::describe("GPU pump", stuck, now)
             .find("inside vsync interrupt callback 0x00005200") != std::string::npos);
  const auto open = events::open_callbacks(events::snapshot());
  assert(open.size() == 1 && open[0].guest_address == kCallbackAddr);
  assert(std::string(open[0].source) == "vsync interrupt");

  g_release_blocking.store(true);
  for (int i = 0; i < 400 && g_blocking_calls.load() < 2; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const bool shutdown_completed = run_with_timeout(
      [&] { harness.session.shutdown(); }, std::chrono::milliseconds(5000));
  assert(shutdown_completed);
  events::set_enabled(false);

  const auto after = xenon::core::SessionExecutionTestAccess::gpu_vsync_callback(harness.session);
  assert(!after.in_callback && after.calls >= 2);
  assert(after.slow_calls >= 1 && after.longest_ns >= 50'000'000 &&
         after.longest_entry == kCallbackAddr);
  bool ended_slowly = false;
  for (const auto& event : events::snapshot()) {
    if (event.kind == events::EventKind::CallbackEnd && event.value >= 50u) ended_slowly = true;
  }
  assert(ended_slowly && "CallbackEnd carries the callback's duration in ms");
  assert(events::open_callbacks(events::snapshot()).empty());
  events::clear();
}

std::shared_ptr<kernel::KernelEvent> g_never_signalled;
std::atomic<bool> g_callback_waiting{false};
std::atomic<kernel::WaitResult> g_callback_wait_result{kernel::WaitResult::Success};

// A vsync callback that blocks in a kernel wait nothing will ever satisfy, as
// a guest interrupt handler stuck on a lock or event would.
ExecutionResult wait_forever_callback_entry(ExecutionContext& context) {
  if (!g_callback_waiting.exchange(true)) {
    g_callback_wait_result = kernel::wait_for_single_object(
        g_never_signalled, std::chrono::milliseconds(-1), 0u);
  }
  context.state.gpr[3] = 0;
  context.state.lr = 0u;
  return {FlowReason::Return, 0u, 0u};
}

// Shutdown must not hang on, or detach, a pump callback or a guest thread
// blocked in an unbounded kernel wait: terminating each thread ends its wait.
void test_shutdown_ends_blocked_waits_in_callbacks_and_guest_threads() {
  g_never_signalled = std::make_shared<kernel::KernelEvent>(true, false);
  g_callback_waiting.store(false);
  g_callback_wait_result.store(kernel::WaitResult::Success);

  constexpr GuestAddress kCallbackAddr = 0x5300u;
  GpuPumpHarness harness(kCallbackAddr, &wait_forever_callback_entry);
  assert(xenon::core::SessionExecutionTestAccess::create_guest_process(harness.session));
  auto* kernel_process = harness.session.kernel_process();
  kernel_process->set_gpu_front_buffer(0x3000u, 64u, 64u, 256u, 0u);
  kernel_process->set_gpu_interrupt_callback(kCallbackAddr, 0u);

  std::atomic<bool> worker_waiting{false};
  std::atomic<kernel::WaitResult> worker_result{kernel::WaitResult::Success};
  kernel::ThreadCreationParams params{};
  auto worker = kernel_process->thread_manager().create_thread(
      [&]() -> std::uint32_t {
        worker_waiting = true;
        worker_result = kernel::wait_for_single_object(g_never_signalled,
                                                       std::chrono::milliseconds(-1), 0u);
        return 0;
      },
      params);
  assert(worker && worker->start());

  for (int i = 0; i < 400 && !(g_callback_waiting.load() && worker_waiting.load()); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  assert(g_callback_waiting.load() && worker_waiting.load());
  std::this_thread::sleep_for(std::chrono::milliseconds(30));

  const bool shutdown_completed = run_with_timeout(
      [&] { harness.session.shutdown(); }, std::chrono::milliseconds(5000));
  assert(shutdown_completed && "a blocked wait must not hang shutdown");
  assert(g_callback_wait_result.load() == kernel::WaitResult::Terminating);
  assert(worker_result.load() == kernel::WaitResult::Terminating);
  assert(worker->state() == kernel::ThreadState::Terminated);
  g_never_signalled.reset();
}

std::shared_ptr<kernel::KernelEvent> g_frame_event;
std::atomic<int> g_frames_signalled{0};

// A vsync callback that signals an auto-reset event a guest thread waits on,
// the usual way a title paces its render thread to vertical blank.
ExecutionResult signal_frame_callback_entry(ExecutionContext& context) {
  if (g_frame_event) {
    // Counted before the signal: the woken thread may finish and the test
    // check the count before this callback runs another instruction.
    g_frames_signalled.fetch_add(1);
    g_frame_event->set();
  }
  context.state.gpr[3] = 0;
  context.state.lr = 0u;
  return {FlowReason::Return, 0u, 0u};
}

// Ordering between a pump callback and a guest wait: every frame the
// callback signals wakes the waiting guest thread, and the pump keeps
// running while the guest thread waits.
void test_vsync_callback_wakes_a_waiting_guest_thread_each_frame() {
  g_frame_event = std::make_shared<kernel::KernelEvent>(false, false);
  g_frames_signalled.store(0);
  constexpr GuestAddress kCallbackAddr = 0x5400u;
  GpuPumpHarness harness(kCallbackAddr, &signal_frame_callback_entry);
  assert(xenon::core::SessionExecutionTestAccess::create_guest_process(harness.session));
  auto* kernel_process = harness.session.kernel_process();

  std::atomic<int> frames_seen{0};
  kernel::ThreadCreationParams params{};
  auto render = kernel_process->thread_manager().create_thread(
      [&]() -> std::uint32_t {
        while (frames_seen.load() < 10) {
          if (kernel::wait_for_single_object(g_frame_event, std::chrono::milliseconds(1000), 0u) !=
              kernel::WaitResult::Success) {
            return 1;  // a frame never arrived
          }
          frames_seen.fetch_add(1);
        }
        return 0;
      },
      params);
  assert(render && render->start());
  kernel_process->set_gpu_front_buffer(0x3000u, 64u, 64u, 256u, 0u);
  kernel_process->set_gpu_interrupt_callback(kCallbackAddr, 0u);

  assert(render->join(5000) && "ten vsync frames arrive within 5 s");
  assert(render->exit_code() == 0u && "no wait for a frame timed out");
  assert(frames_seen.load() == 10 && g_frames_signalled.load() >= 10);
  const bool shutdown_completed = run_with_timeout(
      [&] { harness.session.shutdown(); }, std::chrono::milliseconds(5000));
  assert(shutdown_completed);
  g_frame_event.reset();
}

// A backend whose draws block until released, as a slow native draw or EDRAM
// transfer blocks the GPU worker.
class BlockingDrawBackend final : public gpu::Backend {
 public:
  std::atomic<bool> release{false};
  std::atomic<int> draws_entered{0};
  void begin_submission(memory::AddressSpace&, gpu::Edram&) override {}
  void consume(const gpu::ir::Command& command) override {
    if (!std::holds_alternative<gpu::ir::DrawPacket>(command)) return;
    draws_entered.fetch_add(1);
    while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  void end_submission() override {}
  bool make_guest_memory_cpu_visible(std::uint32_t, std::uint32_t) override { return true; }
  bool make_edram_canonical() override { return true; }
  bool invalidate_edram_native_state() override { return true; }
  gpu::PresentStatus present(const gpu::PresentationFrame&) override {
    return gpu::PresentStatus::NotConfigured;
  }
  bool resize_presentation(std::uint32_t, std::uint32_t) override { return false; }
  bool presentation_ready() const noexcept override { return false; }
};

// Presents whose scanout fails. The pump used to discard present()'s
// status, so a window left black by failing presents was indistinguishable
// from one showing a black frame.
class FailingPresentBackend final : public gpu::Backend {
 public:
  std::atomic<int> presents{0};
  void begin_submission(memory::AddressSpace&, gpu::Edram&) override {}
  void consume(const gpu::ir::Command&) override {}
  void end_submission() override {}
  bool make_guest_memory_cpu_visible(std::uint32_t, std::uint32_t) override { return true; }
  bool make_edram_canonical() override { return true; }
  bool invalidate_edram_native_state() override { return true; }
  gpu::PresentStatus present(const gpu::PresentationFrame&) override {
    return ++presents % 2 ? gpu::PresentStatus::Error : gpu::PresentStatus::Success;
  }
  bool resize_presentation(std::uint32_t, std::uint32_t) override { return true; }
  bool presentation_ready() const noexcept override { return true; }
};

void test_present_outcomes_are_counted_by_status() {
  GpuPumpHarness harness;
  auto backend = std::make_unique<FailingPresentBackend>();
  auto* failing = backend.get();
  xenon::core::SessionExecutionTestAccess::set_backend(harness.session, std::move(backend));
  assert(xenon::core::SessionExecutionTestAccess::present_results(harness.session).empty());
  assert(xenon::core::SessionExecutionTestAccess::create_guest_process(harness.session));
  harness.session.kernel_process()->set_gpu_front_buffer(0x3000u, 64u, 64u, 256u, 0u);
  for (int i = 0; i < 400 && failing->presents.load() < 4; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const bool shutdown_completed = run_with_timeout(
      [&] { harness.session.shutdown(); }, std::chrono::milliseconds(5000));
  assert(shutdown_completed);
  const auto presents = failing->presents.load();
  assert(presents >= 4);
  const auto expected = " (" + std::to_string(presents / 2) + " ok, " +
                        std::to_string((presents + 1) / 2) + " error)";
  assert(xenon::core::SessionExecutionTestAccess::present_results(harness.session) == expected);
}

// Regression (Ace Combat 6): one thread drained the ring, rendered, presented
// and delivered the vsync interrupt, so a draw that took hundreds of
// milliseconds held back vsync, and with it the title's flip handshakes. On
// hardware vertical blank does not wait for the GPU: vsync interrupts keep
// arriving while the GPU worker is stuck in a draw.
void test_vsync_keeps_arriving_while_the_gpu_worker_is_busy() {
  g_gpu_interrupt_ran.store(false);
  constexpr GuestAddress kCallbackAddr = 0x5000u;
  GpuPumpHarness harness(kCallbackAddr, &gpu_interrupt_probe_entry);
  auto backend = std::make_unique<BlockingDrawBackend>();
  auto* blocking = backend.get();
  xenon::core::SessionExecutionTestAccess::set_backend(harness.session, std::move(backend));
  assert(xenon::core::SessionExecutionTestAccess::create_guest_process(harness.session));
  auto* memory = harness.session.memory();
  auto* kernel_process = harness.session.kernel_process();
  kernel_process->set_gpu_interrupt_callback(kCallbackAddr, 0u);

  // DRAW_INDX_2, auto-index, 3 vertices: big-endian ring words.
  constexpr std::uint32_t kRingBase = 0x2000u;
  const std::uint32_t words[] = {0xC0003600u, 0x00030084u};
  for (std::uint32_t i = 0; i < 2; ++i) {
    const std::array<std::byte, 4> bytes{
        static_cast<std::byte>(words[i] >> 24), static_cast<std::byte>(words[i] >> 16),
        static_cast<std::byte>(words[i] >> 8), static_cast<std::byte>(words[i])};
    assert(memory->write_physical(kRingBase + i * 4u, bytes));
  }
  kernel_process->configure_gpu_ring_buffer(kRingBase, 16u);
  kernel_process->set_gpu_ring_buffer_write_index(2u);

  for (int i = 0; i < 400 && blocking->draws_entered.load() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  assert(blocking->draws_entered.load() == 1 && "the worker reached the draw");
  const auto before = xenon::core::SessionExecutionTestAccess::gpu_interrupts_delivered(harness.session);
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  const auto during = xenon::core::SessionExecutionTestAccess::gpu_interrupts_delivered(harness.session);
  // About 15 at 60 Hz; the old single thread delivered none.
  assert(during - before >= 5 && "vsync interrupts continue while a draw blocks the worker");
  assert(kernel_process->gpu_ring_buffer().read_index == 0u &&
         "the read pointer does not move past a draw still executing");

  blocking->release.store(true);
  for (int i = 0; i < 400 && kernel_process->gpu_ring_buffer().read_index != 2u; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  assert(kernel_process->gpu_ring_buffer().read_index == 2u);
  const bool shutdown_completed = run_with_timeout(
      [&] { harness.session.shutdown(); }, std::chrono::milliseconds(5000));
  assert(shutdown_completed);
}

// The worker drains ring work when it is published, not on a polling tick:
// with no vsync callback and nothing else to do, a write-pointer move is
// drained promptly and counted as a ring-work wake.
void test_published_ring_work_wakes_the_worker() {
  GpuPumpHarness harness;
  assert(xenon::core::SessionExecutionTestAccess::create_guest_process(harness.session));
  auto* memory = harness.session.memory();
  auto* kernel_process = harness.session.kernel_process();
  constexpr std::uint32_t kRingBase = 0x2000u;
  assert(memory->fill_physical(kRingBase, 64u * 4u, std::byte{0}));
  kernel_process->configure_gpu_ring_buffer(kRingBase, 64u);
  for (std::uint32_t write = 4u; write <= 32u; write += 4u) {
    kernel_process->set_gpu_ring_buffer_write_index(write);
    bool drained = false;
    for (int i = 0; i < 2000 && !drained; ++i) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      drained = kernel_process->gpu_ring_buffer().read_index == write;
    }
    assert(drained && "published ring work is drained within 200 ms");
  }
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
  test_interrupt_callback_follows_function_handoffs();
  test_pm4_interrupt_reaches_guest_callback_with_source_one();
  test_a_blocked_callback_is_reported_while_it_blocks();
  test_shutdown_ends_blocked_waits_in_callbacks_and_guest_threads();
  test_vsync_callback_wakes_a_waiting_guest_thread_each_frame();
  test_vsync_keeps_arriving_while_the_gpu_worker_is_busy();
  test_published_ring_work_wakes_the_worker();
  test_present_outcomes_are_counted_by_status();

  std::cout << "All GPU pump thread tests passed!\n";
  return 0;
}

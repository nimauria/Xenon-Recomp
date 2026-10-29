#include <array>
#include <atomic>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "xenon/core/session.hpp"

namespace {

using xenon::cpu::CompiledLookupKind;
using xenon::cpu::ExecutionContext;
using xenon::cpu::ExecutionResult;
using xenon::cpu::FlowReason;
using xenon::cpu::GuestAddress;

}  // namespace

namespace xenon::core {

struct SessionExecutionTestAccess {
  static std::uint32_t run(XenonSession& session) {
    return session.run_execution();
  }

  static void configure(
      XenonSession& session, std::shared_ptr<xenon::memory::AddressSpace> memory,
      std::function<void(ExecutionContext&)> binder) {
    session.memory_ = std::move(memory);
    session.main_cpu_state_ = std::make_unique<xenon::cpu::CpuState>();
    session.loaded_xex_.emplace();
    session.loaded_xex_->image.entry_point = 0x1000u;
    session.compiled_registry_binder_ = std::move(binder);
  }

  // Declares a callable import thunk at `thunk` for (library, ordinal) and
  // registers `handler` for it, as load_game() and init_exports() would.
  static void add_import(XenonSession& session, std::string library, std::uint32_t ordinal,
                         GuestAddress thunk, ExportHandler handler) {
    if (!session.code_cache_) session.code_cache_ = std::make_unique<xenon::cpu::ExecutableCodeCache>();
    xenon::xbox::XexImport import{};
    import.module = library;
    import.ordinal = static_cast<std::uint16_t>(ordinal);
    import.guest_thunk = thunk;
    import.kind = xenon::xbox::XexImportKind::FunctionThunk;
    session.loaded_xex_->image.imports.push_back(import);
    session.index_import_thunks();
    ExportDescriptor descriptor{};
    descriptor.library = std::move(library);
    descriptor.ordinal = ordinal;
    descriptor.handler = std::move(handler);
    assert(session.export_registry_.register_export(std::move(descriptor)));
  }

  static void request_stop(XenonSession& session) {
    session.stop_requested_.store(true);
  }

  static const std::string& last_error(const XenonSession& session) {
    return session.last_error_;
  }

  // Publishes a running thread's registers and a thread blocked in a Ke
  // wait, as dispatch_guest_thread() and the export bridge do, then logs
  // the stop report.
  static void report_stop_with(XenonSession& session, std::uint32_t running_id,
                               const xenon::cpu::CpuState& running, std::uint32_t waiting_id,
                               const ExportDescriptor& wait, std::uint32_t objects,
                               std::uint32_t count) {
    session.export_trace_.set_enabled(true);
    {
      std::scoped_lock lock(session.in_flight_exports_mutex_);
      session.guest_thread_states_[running_id] = &running;
      XenonSession::InFlightExport call{};
      call.descriptor = &wait;
      call.arguments = {count, objects, 1u, 3u};
      call.since = std::chrono::steady_clock::now();
      session.in_flight_exports_[waiting_id] = call;
    }
    session.report_stop_diagnostics();
    std::scoped_lock lock(session.in_flight_exports_mutex_);
    session.guest_thread_states_.erase(running_id);
    session.in_flight_exports_.erase(waiting_id);
  }

  static xenon::memory::AddressSpace& memory(XenonSession& session) { return *session.memory_; }

  static void record_pump_activity(XenonSession& session) {
    session.gpu_interrupts_delivered_.store(37u);
    session.gpu_callback_watch_.begin(0x821E63F0u, "vsync interrupt", CallbackWatch::now_ns());
    session.audio_callback_watch_.begin(0x82450000u, "audio render", 0);
    static_cast<void>(session.audio_callback_watch_.end(1'000'000));
  }
};

}  // namespace xenon::core

namespace {

struct Registry {
  std::unordered_map<GuestAddress, xenon::cpu::NativeCompiledEntry> entries;
};

xenon::cpu::NativeCompiledEntry lookup(
    void* opaque, ExecutionContext&, GuestAddress target, CompiledLookupKind) {
  const auto& registry = *static_cast<const Registry*>(opaque);
  const auto it = registry.entries.find(target);
  return it == registry.entries.end() ? nullptr : it->second;
}

ExecutionResult branch_to_second(ExecutionContext&) {
  return {FlowReason::Branch, 0x2000u, 0u};
}

ExecutionResult halt(ExecutionContext&) {
  return {FlowReason::Halt, 0u, 0u};
}

ExecutionResult trap(ExecutionContext&) {
  return {FlowReason::Trap, 0u, 0x1234u};
}

ExecutionResult invalid_branch(ExecutionContext&) {
  return {FlowReason::Branch, 0xDEAD0000u, 0u};
}

ExecutionResult terminal_return(ExecutionContext& context) {
  context.state.lr = 0u;
  return {FlowReason::Return, 0u, 0u};
}

ExecutionResult entry_fn(ExecutionContext& context) {
  return branch_to_second(context);
}

struct SessionHarness {
  xenon::core::XenonSession session;
  Registry registry;

  SessionHarness(xenon::cpu::NativeCompiledEntry entry,
                 xenon::cpu::NativeCompiledEntry target = nullptr) {
    auto memory = std::make_shared<xenon::memory::AddressSpace>(
        xenon::memory::GuestTranslationMode::Compact);
    assert(memory->initialize());
    registry.entries.emplace(0x1000u, entry);
    if (target) registry.entries.emplace(0x2000u, target);
    xenon::core::SessionExecutionTestAccess::configure(
        session, std::move(memory), [this](ExecutionContext& context) {
      context.compiled_registry = &registry;
      context.compiled_lookup = &lookup;
    });
  }

  std::uint32_t run() { return xenon::core::SessionExecutionTestAccess::run(session); }
};

void test_branch_dispatches_to_compiled_target() {
  SessionHarness harness(&entry_fn, &halt);
  assert(harness.run() == 0u);
}

void test_trap_remains_failure() {
  SessionHarness harness(&trap);
  assert(harness.run() == 0x1234u);
}

void test_halt_is_clean_terminal() {
  SessionHarness harness(&halt);
  assert(harness.run() == 0u);
}

void test_invalid_branch_is_failure() {
  SessionHarness harness(&invalid_branch);
  assert(harness.run() != 0u);
  assert(!xenon::core::SessionExecutionTestAccess::last_error(harness.session).empty());
}

void test_return_with_zero_link_register_is_clean() {
  SessionHarness harness(&terminal_return);
  assert(harness.run() == 0u);
}

// Regression (AC6 wrote 2.7 million "Guest dispatch returned" lines in 45 s,
// each under the console lock, throttling its main thread while loading):
// per-dispatch tracing is verbose-only.
void test_dispatch_trace_is_verbose_only() {
  SessionHarness harness(&entry_fn, &halt);
  std::ostringstream captured;
  auto* const previous = std::cout.rdbuf(captured.rdbuf());
  assert(harness.run() == 0u);
  std::cout.rdbuf(previous);
  assert(captured.str().find("Guest dispatch returned") == std::string::npos);
  assert(captured.str().find("Guest entry returned") == std::string::npos);
}

void test_stop_request_is_clean() {
  SessionHarness harness(&entry_fn, &halt);
  xenon::core::SessionExecutionTestAccess::request_stop(harness.session);
  assert(harness.run() == 0u);
}

constexpr GuestAddress kImportThunk = 0x3000u;
int g_import_calls = 0;

// A title's argument-shuffling wrapper: sets the caller id, puts the return
// address (the compiled function at 0x2000) in LR, then `b thunk`.
ExecutionResult tail_branch_to_import(ExecutionContext& context) {
  context.state.lr = 0x2000u;
  context.state.gpr[3] = 1u;
  context.state.cia = 0x1010u;  // the wrapper's own `b`
  return {FlowReason::Branch, kImportThunk, 0u};
}

// Regression (AC6 NetDll_socket/NetDll_bind ran twice, the second time with
// the first call's result as its first argument): an import reached by a
// tail branch runs exactly once and continues at LR. It must also not need
// the Gen 7 fallback, which strict AOT will not have; this harness binds none.
void test_tail_branch_to_import_runs_once_and_returns_to_lr() {
  SessionHarness harness(&tail_branch_to_import, &halt);
  g_import_calls = 0;
  xenon::core::SessionExecutionTestAccess::add_import(
      harness.session, "testlib", 7u, kImportThunk, [](xenon::core::ExportCallContext& call) {
        ++g_import_calls;
        assert(call.cpu.gpr[3] == 1u && "the export must see the wrapper's arguments");
        call.cpu.gpr[3] = 0xABCDu;
        return true;
      });
  assert(harness.run() == 0u);
  assert(g_import_calls == 1 && "the export must run exactly once");
}

std::uint32_t g_outer_dispatches = 0;

// Each call is one outer-loop dispatch, like a return through a shared
// register-restore helper; after 1,500,000 of them the guest finishes.
ExecutionResult long_running_loop(ExecutionContext&) {
  if (++g_outer_dispatches == 1'500'000u) return {FlowReason::Halt, 0u, 0u};
  return {FlowReason::Branch, 0x2000u, 0u};
}

// Regression (AC6, about 30 s in): the outer dispatch loop had a lifetime cap
// of 1,000,000 dispatches and failed the thread when correct, still-running
// code reached it. A guest thread runs until it finishes, however long.
void test_long_running_thread_is_not_capped() {
  SessionHarness harness(&long_running_loop, &long_running_loop);
  g_outer_dispatches = 0;
  assert(harness.run() == 0u);
  assert(g_outer_dispatches == 1'500'000u);
  assert(xenon::core::SessionExecutionTestAccess::last_error(harness.session).empty());
}

// The stop report must say where a thread spinning in guest code was called
// from, and what a thread blocked in a Ke wait is waiting on. AC6's audio
// callback spun at 0x823AC8D0 for 85 s with no caller information, while
// two threads waited in KeWaitForMultipleObjects on objects not shown.
void test_stop_report_shows_spinning_callers_and_wait_objects() {
  SessionHarness harness(&halt);
  auto& memory = xenon::core::SessionExecutionTestAccess::memory(harness.session);
  xenon::memory::GuestAddress stack{};
  assert(memory.allocate(0x4000u, 0x1000u, xenon::memory::kReadWrite, false, stack));
  // Frames: inner -> middle -> outer, LR saved at -8 from each caller's r1.
  const std::uint32_t top = stack + 0x3000u, outer = top - 0x100u, inner = outer - 0x90u;
  memory.write32_be(top, 0u);
  memory.write32_be(top - 8u, 0x82000010u);
  memory.write32_be(outer, top);
  memory.write32_be(outer - 8u, 0x82000020u);
  memory.write32_be(inner, outer);
  xenon::cpu::CpuState running{};
  running.cia = 0x823AC8D0u;
  running.gpr[1] = inner;

  // Two dispatcher objects: an unsignalled event (type 1) and a signalled one.
  const std::uint32_t objects = stack + 0x100u, event_a = stack + 0x200u, event_b = stack + 0x220u;
  memory.write32_be(objects, event_a);
  memory.write32_be(objects + 4u, event_b);
  memory.write8(event_a, 1u);
  memory.write32_be(event_a + 4u, 0u);
  memory.write8(event_b, 1u);
  memory.write32_be(event_b + 4u, 1u);
  xenon::core::ExportDescriptor wait{};
  wait.library = "xboxkrnl.exe";
  wait.name = "KeWaitForMultipleObjects";

  std::ostringstream captured;
  auto* const previous = std::cout.rdbuf(captured.rdbuf());
  xenon::core::SessionExecutionTestAccess::report_stop_with(harness.session, 21u, running, 23u,
                                                            wait, objects, 2u);
  std::cout.rdbuf(previous);
  // Hex case depends on stream flags earlier output left set.
  auto log = captured.str();
  for (auto& c : log) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  assert(log.find("t21 cia=0x823ac8d0") != std::string::npos);
  assert(log.find("stack: 0x82000020 0x82000010") != std::string::npos);
  std::ostringstream expected;
  expected << "objects: 0x" << std::hex << event_a << "{type=1 signal=0x0} 0x" << event_b
           << "{type=1 signal=0x1}";
  assert(log.find(expected.str()) != std::string::npos);

  // A thread blocked entering a critical section: the report names the
  // section's counts and its owner (AC6: t22 waited 39 s on 0x829147F4).
  const std::uint32_t cs = stack + 0x300u;
  memory.write32_be(cs + 0x10u, 1u);            // held, one waiter
  memory.write32_be(cs + 0x14u, 1u);
  memory.write32_be(cs + 0x18u, 0x40001234u);   // owner KTHREAD
  xenon::core::ExportDescriptor enter{};
  enter.library = "xboxkrnl.exe";
  enter.name = "RtlEnterCriticalSection";
  std::ostringstream cs_captured;
  auto* const cs_previous = std::cout.rdbuf(cs_captured.rdbuf());
  xenon::core::SessionExecutionTestAccess::report_stop_with(harness.session, 21u, running, 22u,
                                                            enter, 0u, cs);  // r3 = cs
  std::cout.rdbuf(cs_previous);
  auto cs_log = cs_captured.str();
  for (auto& c : cs_log) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  std::ostringstream cs_expected;
  cs_expected << "critical section 0x" << std::hex << cs
              << ": lock_count=1 recursion=1 owner_kthread=0x40001234";
  assert(cs_log.find(cs_expected.str()) != std::string::npos);
}

ExecutionResult read_unmapped(ExecutionContext& context) {
  static_cast<void>(context.memory.read32_be(0x10u));
  return {FlowReason::Halt, 0u, 0u};
}

// Regression (AC6): the GPU/audio pump state was only logged by a clean
// stop, so a run that ended in a guest fault gave no evidence of whether
// vsync interrupts and audio callbacks were still being delivered.
void test_crash_reports_pump_state() {
  SessionHarness harness(&read_unmapped);
  xenon::core::SessionExecutionTestAccess::record_pump_activity(harness.session);
  std::ostringstream captured;
  auto* const previous = std::cout.rdbuf(captured.rdbuf());
  const auto exit_code = harness.run();
  std::cout.rdbuf(previous);
  const auto log = captured.str();
  assert(exit_code != 0u && "an unmapped read is a crash");
  assert(log.find("Pump state (main thread crashed)") != std::string::npos);
  assert(log.find("GPU pump: inside vsync interrupt callback 0x821E63F0") != std::string::npos);
  assert(log.find("Audio pump: idle; 1 callbacks") != std::string::npos);
  assert(log.find("GPU vsync interrupt: callback=0x0 context=0x0 delivered=37 failed=0") !=
         std::string::npos);
}

}  // namespace

int main() {
  assert(xenon::cpu::flow_reason_name(FlowReason::Branch) == "Branch");
  test_branch_dispatches_to_compiled_target();
  test_trap_remains_failure();
  test_halt_is_clean_terminal();
  test_invalid_branch_is_failure();
  test_return_with_zero_link_register_is_clean();
  test_stop_request_is_clean();
  test_tail_branch_to_import_runs_once_and_returns_to_lr();
  test_dispatch_trace_is_verbose_only();
  test_crash_reports_pump_state();
  test_long_running_thread_is_not_capped();
  test_stop_report_shows_spinning_callers_and_wait_objects();
  std::cout << "Session execution tests passed\n";
  return 0;
}

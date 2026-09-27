#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
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

  static void request_stop(XenonSession& session) {
    session.stop_requested_.store(true);
  }

  static const std::string& last_error(const XenonSession& session) {
    return session.last_error_;
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

void test_stop_request_is_clean() {
  SessionHarness harness(&entry_fn, &halt);
  xenon::core::SessionExecutionTestAccess::request_stop(harness.session);
  assert(harness.run() == 0u);
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
  std::cout << "Session execution tests passed\n";
  return 0;
}

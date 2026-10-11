// XNotify listeners (xam::NotificationManager and XamNotifyCreateListener,
// XamNotifyCreateListenerInternal, XNotifyGetNext), driven through
// core::ExportRegistry::invoke(). Behaviour as in xenia's xam_notify.cc,
// xnotifylistener.cc and KernelState::RegisterNotifyListener.

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#include "xenon/core/export_registry.hpp"
#include "xenon/core/session.hpp"
#include "xenon/kernel/event.hpp"
#include "xenon/kernel/handle_table.hpp"
#include "xenon/kernel/memory.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/kernel/wait.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xam/notification_manager.hpp"

namespace xenon::xam {
bool register_notification_exports(core::ExportRegistry& registry,
                                   NotificationManager& notification_manager,
                                   core::XenonSession& session);
}  // namespace xenon::xam

namespace xenon::core {
struct SessionExecutionTestAccess {
  static void configure(XenonSession& session,
                        std::shared_ptr<xenon::memory::AddressSpace> memory) {
    session.config_.enable_logging = false;
    session.memory_ = memory;
    session.kernel_memory_ = std::make_shared<kernel::KernelMemory>(memory);
    session.kernel_process_ = std::make_shared<kernel::KernelProcess>(session.kernel_memory_);
  }
};
}  // namespace xenon::core

using namespace xenon;
using namespace std::chrono_literals;

namespace {

constexpr std::uint32_t kCreateListener = 0x028Au;
constexpr std::uint32_t kGetNext = 0x028Bu;
constexpr std::uint32_t kCreateListenerInternal = 0x0291u;
constexpr std::uint64_t kSystemArea = 1u;
constexpr std::uint64_t kLiveArea = 2u;

struct Fixture {
  std::shared_ptr<memory::AddressSpace> address_space;
  core::XenonSession session;
  xam::NotificationManager notifications;
  core::ExportRegistry registry;
  memory::GuestAddress id_ptr{};
  memory::GuestAddress param_ptr{};

  Fixture() {
    address_space =
        std::make_shared<memory::AddressSpace>(memory::GuestTranslationMode::Compact);
    assert(address_space->initialize());
    core::SessionExecutionTestAccess::configure(session, address_space);
    notifications.initialize();
    assert(xam::register_notification_exports(registry, notifications, session));
    assert(address_space->allocate(8, 8, memory::kReadWrite, false, id_ptr));
    param_ptr = id_ptr + 4u;
  }

  std::uint32_t call(std::uint32_t ordinal, cpu::CpuState& cpu) {
    core::ExportCallContext context{cpu, *address_space, 0, 0};
    assert(registry.invoke("xam", ordinal, context).handled);
    return static_cast<std::uint32_t>(cpu.gpr[3]);
  }

  kernel::Handle listen(std::uint64_t mask, std::uint32_t max_version = 0u) {
    cpu::CpuState cpu{};
    cpu.gpr[3] = mask;
    cpu.gpr[4] = max_version;
    return call(kCreateListener, cpu);
  }

  // XNotifyGetNext(handle, match_id, id_ptr, param_ptr): the dequeued
  // (id, param), or (0, 0) when nothing was dequeued.
  std::pair<std::uint32_t, std::uint32_t> next(kernel::Handle handle, std::uint32_t match_id = 0u) {
    address_space->write32_be(id_ptr, 0xAAAAAAAAu);
    address_space->write32_be(param_ptr, 0xAAAAAAAAu);
    cpu::CpuState cpu{};
    cpu.gpr[3] = handle;
    cpu.gpr[4] = match_id;
    cpu.gpr[5] = id_ptr;
    cpu.gpr[6] = param_ptr;
    const auto dequeued = call(kGetNext, cpu);
    const std::pair<std::uint32_t, std::uint32_t> got{address_space->read32_be(id_ptr),
                                                      address_space->read32_be(param_ptr)};
    assert((dequeued == 1u) == (got.first != 0u));
    return got;
  }

  std::shared_ptr<kernel::KernelObject> object(kernel::Handle handle) {
    kernel::HandleView view{};
    assert(session.kernel_process()->handle_table().lookup(handle, view) ==
           kernel::KernelIoCode::Success);
    return view.object;
  }
};

void test_the_first_system_listener_gets_the_startup_notifications() {
  Fixture f;
  const auto handle = f.listen(kSystemArea | kLiveArea);
  assert(handle != 0u);
  const std::vector<std::pair<std::uint32_t, std::uint32_t>> expected{
      {0x09u, 1u}, {0x09u, 0u}, {0x0Au, 1u}, {0x0Au, 1u},
      {0x12u, 0u}, {0x12u, 0u}, {0x13u, 0u}, {0x13u, 0u}};
  for (const auto& notification : expected) assert(f.next(handle) == notification);
  assert(f.next(handle) == std::make_pair(0u, 0u));
  assert(f.next(f.listen(kSystemArea)) == std::make_pair(0u, 0u) && "only the first listener");
  std::cout << "test_the_first_system_listener_gets_the_startup_notifications passed\n";
}

void test_listeners_filter_by_area_version_and_match_id() {
  Fixture f;
  const auto live = f.listen(kLiveArea);  // no system area: no startup notifications
  const auto system = f.listen(kSystemArea);
  while (f.next(system).first != 0u) {}

  f.notifications.broadcast(xam::notification::kStorageDevicesChanged, 0u);
  f.notifications.broadcast(xam::notification::kLiveConnectionChanged, 0x8015190Bu);
  f.notifications.broadcast(0x0001000Au, 7u);  // version 1: newer than max_version 0
  assert(f.next(live) == std::make_pair(xam::notification::kLiveConnectionChanged, 0x8015190Bu));
  assert(f.next(live) == std::make_pair(0u, 0u));

  f.notifications.broadcast(xam::notification::kSigninChanged, 1u);
  assert(f.next(system, xam::notification::kSigninChanged) ==
         std::make_pair(xam::notification::kSigninChanged, 1u) && "match_id takes out of order");
  assert(f.next(system, xam::notification::kSigninChanged) == std::make_pair(0u, 0u));
  assert(f.next(system) == std::make_pair(xam::notification::kStorageDevicesChanged, 0u));
  assert(f.next(system) == std::make_pair(0u, 0u) && "the version 1 notification was dropped");

  const auto newer = f.listen(kSystemArea, 1u);
  f.notifications.broadcast(0x0001000Au, 7u);
  assert(f.next(newer) == std::make_pair(0x0001000Au, 7u));
  std::cout << "test_listeners_filter_by_area_version_and_match_id passed\n";
}

// The listener handle is a waitable kernel object, signalled while it has
// notifications queued, and closes like any handle.
void test_the_listener_handle_is_waitable_and_closable() {
  Fixture f;
  const auto handle = f.listen(kLiveArea);
  auto object = f.object(handle);
  assert(kernel::wait_for_single_object(object, 0ms, 1) == kernel::WaitResult::Timeout);
  f.notifications.broadcast(xam::notification::kLiveConnectionChanged, 0u);
  assert(kernel::wait_for_single_object(object, 0ms, 1) == kernel::WaitResult::Success);
  assert(kernel::wait_for_single_object(object, 0ms, 1) == kernel::WaitResult::Success &&
         "manual reset: still signalled until emptied");
  assert(f.next(handle).first != 0u);
  assert(kernel::wait_for_single_object(object, 0ms, 1) == kernel::WaitResult::Timeout);

  assert(f.session.kernel_process()->handle_table().close(handle) == kernel::KernelIoCode::Success);
  object.reset();
  f.notifications.broadcast(xam::notification::kLiveConnectionChanged, 0u);  // no listener left
  assert(f.next(handle) == std::make_pair(0u, 0u) && "a closed handle dequeues nothing");
  assert(f.next(0x1234u) == std::make_pair(0u, 0u));
  std::cout << "test_the_listener_handle_is_waitable_and_closable passed\n";
}

void test_get_next_pointer_rules_and_the_internal_variant() {
  Fixture f;
  cpu::CpuState cpu{};
  cpu.gpr[3] = kSystemArea;
  cpu.gpr[4] = 1;  // is_system
  cpu.gpr[5] = 0;  // max_version
  const auto handle = f.call(kCreateListenerInternal, cpu);
  assert(handle != 0u);

  // No id_ptr: nothing is dequeued, and *param_ptr is zeroed.
  f.address_space->write32_be(f.param_ptr, 0xAAAAAAAAu);
  cpu = cpu::CpuState{};
  cpu.gpr[3] = handle;
  cpu.gpr[6] = f.param_ptr;
  assert(f.call(kGetNext, cpu) == 0u);
  assert(f.address_space->read32_be(f.param_ptr) == 0u);
  // A null param_ptr is allowed.
  cpu = cpu::CpuState{};
  cpu.gpr[3] = handle;
  cpu.gpr[5] = f.id_ptr;
  assert(f.call(kGetNext, cpu) == 1u);
  assert(f.address_space->read32_be(f.id_ptr) == xam::notification::kSystemUi &&
         "the internal variant receives the startup notifications too");
  std::cout << "test_get_next_pointer_rules_and_the_internal_variant passed\n";
}

}  // namespace

int main() {
  test_the_first_system_listener_gets_the_startup_notifications();
  test_listeners_filter_by_area_version_and_match_id();
  test_the_listener_handle_is_waitable_and_closable();
  test_get_next_pointer_rules_and_the_internal_variant();
  std::cout << "All notification tests passed!\n";
  return 0;
}

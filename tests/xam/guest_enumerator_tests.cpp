// Regression coverage for the real XamContentCreateEnumerator/XamEnumerate/
// XamNotifyCreateListener/XNotifyGetNext implementations added to replace
// their previous fake-handle/wrong-GPR stub bodies (see xam_content_exports.cpp,
// xam_enum_exports.cpp, xam_notification_exports.cpp). Drives each export
// through core::ExportRegistry::invoke() with real guest GPRs/memory, exactly
// as a guest thunk would, rather than calling the handler C++ function
// directly - this is what actually proves the ABI register mapping (the bug
// this pass found and fixed) is correct.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

#include "xenon/core/export_registry.hpp"
#include "xenon/core/session.hpp"
#include "xenon/kernel/handle_table.hpp"
#include "xenon/kernel/memory.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xam/content_manager.hpp"
#include "xenon/xam/guest_enumerator.hpp"
#include "xenon/xam/notification_manager.hpp"
#include "xenon/xam/types.hpp"

namespace xenon::xam {
bool register_content_exports(core::ExportRegistry& registry, ContentManager& content_manager,
                              core::XenonSession& session);
bool register_enum_exports(core::ExportRegistry& registry, core::XenonSession& session);
bool register_notification_exports(core::ExportRegistry& registry,
                                   NotificationManager& notification_manager);
}  // namespace xenon::xam

// Same pattern as tests/core/thread_creation_tests.cpp's
// SessionExecutionTestAccess, defined independently in this translation unit
// (each test executable is a separate program, so there is no ODR conflict)
// with just the accessor this file needs: a kernel_process_ (the content
// enumerator handle is a real kernel object, inserted/looked up through it),
// without the full init_exports()/audio/input/xam wiring this test has no
// need for.
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

namespace {

void test_guest_enumerator_batches_and_exhausts() {
  std::vector<std::vector<std::byte>> items;
  for (std::uint8_t i = 0; i < 5; ++i) {
    items.push_back({std::byte{i}, std::byte{static_cast<std::uint8_t>(i + 1u)}});
  }
  xam::GuestEnumeratorObject enumerator(/*item_size=*/2u, /*items_per_enumerate=*/2u,
                                        std::move(items));
  assert(enumerator.remaining() == 5u);

  std::vector<std::byte> out;
  assert(enumerator.next(out) == 2u);
  assert(out.size() == 4u);
  assert(out[0] == std::byte{0} && out[1] == std::byte{1});
  assert(enumerator.remaining() == 3u);

  assert(enumerator.next(out) == 2u);
  assert(enumerator.remaining() == 1u);

  assert(enumerator.next(out) == 1u);
  assert(out.size() == 2u);
  assert(enumerator.remaining() == 0u);

  assert(enumerator.next(out) == 0u);
  std::cout << "test_guest_enumerator_batches_and_exhausts passed\n";
}

void test_guest_enumerator_empty() {
  xam::GuestEnumeratorObject enumerator(4u, 10u, {});
  std::vector<std::byte> out;
  assert(enumerator.next(out) == 0u);
  assert(enumerator.remaining() == 0u);
  std::cout << "test_guest_enumerator_empty passed\n";
}

struct ContentFixture {
  std::shared_ptr<memory::AddressSpace> address_space;
  core::XenonSession session;
  xam::ContentManager content_manager;
  core::ExportRegistry registry;

  ContentFixture() {
    address_space =
        std::make_shared<memory::AddressSpace>(memory::GuestTranslationMode::Compact);
    assert(address_space->initialize());
    core::SessionExecutionTestAccess::configure(session, address_space);
    content_manager.initialize();
    assert(xam::register_content_exports(registry, content_manager, session));
    assert(xam::register_enum_exports(registry, session));
  }

  [[nodiscard]] core::ExportCallResult invoke(std::uint32_t ordinal, cpu::CpuState& cpu) {
    core::ExportCallContext call{cpu, *address_space, 0, 0};
    return registry.invoke("xam", ordinal, call);
  }

  [[nodiscard]] memory::GuestAddress alloc32() {
    memory::GuestAddress addr{};
    assert(address_space->allocate(4, 4, memory::kReadWrite, false, addr));
    return addr;
  }
};

// XamContentCreateEnumerator (0x025C) -> XamEnumerate (0x0250) round trip:
// real content created through ContentManager must actually come back out
// through the enumerator, byte for byte, and the handle must behave like a
// real kernel object (NtClose-able, X_ERROR_INVALID_HANDLE afterward).
void test_content_enumerator_round_trip() {
  ContentFixture f;

  std::uint32_t content_id = 0;
  assert(f.content_manager.create_content(xam::storage_device::HardDisk, "save1",
                                           xam::ContentType::SavedGame,
                                           /*title_id=*/0, content_id) == xam::result::Success);

  cpu::CpuState cpu{};
  const auto buffer_size_ptr = f.alloc32();
  const auto handle_ptr = f.alloc32();
  cpu.gpr[3] = 0;                                                     // user_index
  cpu.gpr[4] = xam::storage_device::HardDisk;                         // device_id
  cpu.gpr[5] = static_cast<std::uint32_t>(xam::ContentType::SavedGame);  // content_type
  cpu.gpr[6] = 0;                                                     // content_flags
  cpu.gpr[7] = 10;                                                    // items_per_enumerate
  cpu.gpr[8] = buffer_size_ptr;
  cpu.gpr[9] = handle_ptr;

  auto result = f.invoke(0x025Cu, cpu);  // XamContentCreateEnumerator
  assert(result.handled);
  assert(cpu.gpr[3] == xam::result::Success);
  assert(f.address_space->read32_be(buffer_size_ptr) == 306u * 10u);
  const auto handle = f.address_space->read32_be(handle_ptr);
  assert(handle != 0u);

  // XamEnumerate: one item should come back, matching the real content's
  // device_id/content_type/file_name byte layout.
  memory::GuestAddress buffer_addr{};
  assert(f.address_space->allocate(306, 4, memory::kReadWrite, false, buffer_addr));
  const auto items_returned_ptr = f.alloc32();

  cpu::CpuState enum_cpu{};
  enum_cpu.gpr[3] = handle;
  enum_cpu.gpr[4] = 0;  // flags
  enum_cpu.gpr[5] = buffer_addr;
  enum_cpu.gpr[6] = 306u;  // buffer_length - exactly one item
  enum_cpu.gpr[7] = items_returned_ptr;

  result = f.invoke(0x0250u, enum_cpu);  // XamEnumerate
  assert(result.handled);
  assert(enum_cpu.gpr[3] == xam::result::Success);
  assert(f.address_space->read32_be(items_returned_ptr) == 1u);
  assert(f.address_space->read32_be(buffer_addr + 0u) == xam::storage_device::HardDisk);
  assert(f.address_space->read32_be(buffer_addr + 4u) ==
         static_cast<std::uint32_t>(xam::ContentType::SavedGame));
  assert(f.address_space->read8(buffer_addr + 264u) == static_cast<std::uint8_t>('s'));

  // No more items: the second call must report X_ERROR_NO_MORE_FILES, not
  // silently report success with zero items. invoke() overwrote gpr[3] with
  // the previous call's result code, so the handle must be restored before
  // reusing enum_cpu - this is the guest ABI's actual register-reuse
  // contract (gpr[3] is a plain scratch/return register, not sticky), not
  // something XamEnumerate itself needs to account for.
  enum_cpu.gpr[3] = handle;
  result = f.invoke(0x0250u, enum_cpu);
  assert(result.handled);
  assert(enum_cpu.gpr[3] == 0x00000012u);  // X_ERROR_NO_MORE_FILES
  assert(f.address_space->read32_be(items_returned_ptr) == 0u);

  // The handle is a real kernel object: NtClose-style teardown through the
  // handle table invalidates it for any further XamEnumerate call.
  kernel::HandleView view{};
  assert(f.session.kernel_process()->handle_table().lookup(handle, view) ==
         kernel::KernelIoCode::Success);
  assert(f.session.kernel_process()->handle_table().close(handle) == kernel::KernelIoCode::Success);
  enum_cpu.gpr[3] = handle;
  result = f.invoke(0x0250u, enum_cpu);
  assert(result.handled);
  assert(enum_cpu.gpr[3] == 0x00000006u);  // X_ERROR_INVALID_HANDLE

  std::cout << "test_content_enumerator_round_trip passed\n";
}

void test_enumerate_unknown_handle_is_invalid() {
  ContentFixture f;
  cpu::CpuState cpu{};
  memory::GuestAddress buffer_addr{};
  assert(f.address_space->allocate(306, 4, memory::kReadWrite, false, buffer_addr));
  cpu.gpr[3] = 0xDEADBEEFu;  // never a real handle
  cpu.gpr[5] = buffer_addr;
  cpu.gpr[6] = 306u;
  const auto result = f.invoke(0x0250u, cpu);
  assert(result.handled);
  assert(cpu.gpr[3] == 0x00000006u);  // X_ERROR_INVALID_HANDLE
  std::cout << "test_enumerate_unknown_handle_is_invalid passed\n";
}

struct NotificationFixture {
  std::shared_ptr<memory::AddressSpace> address_space;
  xam::NotificationManager notification_manager;
  core::ExportRegistry registry;

  NotificationFixture() {
    address_space =
        std::make_shared<memory::AddressSpace>(memory::GuestTranslationMode::Compact);
    assert(address_space->initialize());
    notification_manager.initialize();
    assert(xam::register_notification_exports(registry, notification_manager));
  }

  [[nodiscard]] core::ExportCallResult invoke(std::uint32_t ordinal, cpu::CpuState& cpu) {
    core::ExportCallContext call{cpu, *address_space, 0, 0};
    return registry.invoke("xam", ordinal, call);
  }

  [[nodiscard]] memory::GuestAddress alloc32() {
    memory::GuestAddress addr{};
    assert(address_space->allocate(4, 4, memory::kReadWrite, false, addr));
    return addr;
  }
};

// XamNotifyCreateListener (0x028A) real ABI: the handle is the RETURN VALUE
// in r3, not written through an out-pointer - the previous stub read a
// bogus out-pointer from gpr[1] (the guest stack pointer) and always
// returned 0 (an invalid handle) to a real caller.
void test_notify_create_listener_returns_handle_directly() {
  NotificationFixture f;
  cpu::CpuState cpu{};
  cpu.gpr[3] = 0;  // mask
  cpu.gpr[4] = 0;  // max_version
  const auto result = f.invoke(0x028Au, cpu);  // XamNotifyCreateListener
  assert(result.handled);
  assert(cpu.gpr[3] != 0u);
  std::cout << "test_notify_create_listener_returns_handle_directly passed\n";
}

// XNotifyGetNext (0x028B): must actually write the dequeued notification's
// id/param to guest memory (the previous stub never wrote either output),
// and must zero them + return FALSE when the queue is empty.
void test_notify_get_next_writes_real_payload() {
  NotificationFixture f;

  const auto id_ptr = f.alloc32();
  const auto param_ptr = f.alloc32();
  cpu::CpuState cpu{};
  cpu.gpr[3] = 1;  // handle (unused by the global-queue model, but non-zero)
  cpu.gpr[4] = 0;  // match_id: none, just dequeue next
  cpu.gpr[5] = id_ptr;
  cpu.gpr[6] = param_ptr;

  auto result = f.invoke(0x028Bu, cpu);  // XNotifyGetNext
  assert(result.handled);
  assert(cpu.gpr[3] == 0u);  // FALSE: nothing queued yet
  assert(f.address_space->read32_be(id_ptr) == 0u);
  assert(f.address_space->read32_be(param_ptr) == 0u);

  xam::Notification notification{};
  notification.type = xam::NotificationType::FriendOnline;
  notification.param = 0x1234u;
  f.notification_manager.post_notification(notification);

  result = f.invoke(0x028Bu, cpu);
  assert(result.handled);
  assert(cpu.gpr[3] == 1u);  // TRUE: a notification was dequeued
  assert(f.address_space->read32_be(id_ptr) ==
         static_cast<std::uint32_t>(xam::NotificationType::FriendOnline));
  assert(f.address_space->read32_be(param_ptr) == 0x1234u);

  std::cout << "test_notify_get_next_writes_real_payload passed\n";
}

}  // namespace

int main() {
  test_guest_enumerator_batches_and_exhausts();
  test_guest_enumerator_empty();
  test_content_enumerator_round_trip();
  test_enumerate_unknown_handle_is_invalid();
  test_notify_create_listener_returns_handle_directly();
  test_notify_get_next_writes_real_payload();
  std::cout << "All guest_enumerator_tests passed!\n";
  return 0;
}

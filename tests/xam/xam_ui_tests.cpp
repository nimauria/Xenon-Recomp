// System dialogs Xenon answers without UI (XamShowMessageBoxUI,
// XamShowKeyboardUI, XamIsUIActive, as xenia does headless) and
// XamUserGetGamerTag, driven through core::ExportRegistry::invoke().

#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

#include "xenon/core/export_registry.hpp"
#include "xenon/core/session.hpp"
#include "xenon/kernel/memory.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xam/notification_manager.hpp"
#include "xenon/xam/types.hpp"
#include "xenon/xam/user_manager.hpp"

namespace xenon::xam {
bool register_ui_exports(core::ExportRegistry& registry, core::XenonSession& session,
                         NotificationManager& notifications);
bool register_user_exports(core::ExportRegistry& registry, UserManager& user_manager,
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

namespace {

constexpr std::uint32_t kShowKeyboard = 0x02C1u;
constexpr std::uint32_t kShowMessageBox = 0x02CAu;
constexpr std::uint32_t kIsUiActive = 0x02ECu;
constexpr std::uint32_t kGetGamerTag = 0x020Cu;
constexpr std::uint32_t kIoPending = 0x3E5u;

struct Fixture {
  std::shared_ptr<memory::AddressSpace> address_space;
  core::XenonSession session;
  xam::NotificationManager notifications;
  xam::UserManager users;
  core::ExportRegistry registry;
  memory::GuestAddress stack_top{};

  Fixture() {
    address_space =
        std::make_shared<memory::AddressSpace>(memory::GuestTranslationMode::Compact);
    assert(address_space->initialize());
    core::SessionExecutionTestAccess::configure(session, address_space);
    users.initialize();
    assert(xam::register_ui_exports(registry, session, notifications));
    assert(xam::register_user_exports(registry, users, session));
    memory::GuestAddress stack{};
    assert(address_space->allocate(0x10000, 0x1000, memory::kReadWrite, false, stack));
    stack_top = stack + 0x10000u - 0x200u;
  }

  memory::GuestAddress alloc(std::uint32_t bytes) {
    memory::GuestAddress address{};
    assert(address_space->allocate(bytes, 8, memory::kReadWrite, false, address));
    for (std::uint32_t i = 0; i < bytes; ++i) address_space->write8(address + i, 0xEEu);
    return address;
  }

  memory::GuestAddress wide(const std::u16string& text) {
    const auto address = alloc(static_cast<std::uint32_t>((text.size() + 1u) * 2u));
    for (std::size_t i = 0; i <= text.size(); ++i) {
      address_space->write16_be(address + static_cast<std::uint32_t>(i * 2u),
                                i < text.size() ? text[i] : 0u);
    }
    return address;
  }

  std::u16string read_wide(memory::GuestAddress address, std::uint32_t max_units) {
    std::u16string text;
    for (std::uint32_t i = 0; i < max_units; ++i) {
      const auto unit = address_space->read16_be(address + i * 2u);
      if (unit == 0u) break;
      text.push_back(static_cast<char16_t>(unit));
    }
    return text;
  }

  std::uint32_t call(std::uint32_t ordinal, cpu::CpuState& cpu) {
    if (cpu.gpr[1] == 0u) cpu.gpr[1] = stack_top;
    core::ExportCallContext context{cpu, *address_space, 0, 0};
    assert(registry.invoke("xam", ordinal, context).handled);
    return static_cast<std::uint32_t>(cpu.gpr[3]);
  }
};

void test_a_message_box_answers_with_the_focused_button() {
  Fixture f;
  auto listener = f.notifications.create_listener(2u, 0u);  // no system area: nothing queued
  auto system = f.notifications.create_listener(1u, 0u);
  std::uint32_t id = 0, param = 0;
  while (f.notifications.dequeue(system.get(), 0, id, param)) {}

  const auto result_ptr = f.alloc(4);
  cpu::CpuState cpu{};
  cpu.gpr[4] = f.wide(u"Title");
  cpu.gpr[5] = f.wide(u"Overwrite your save?");
  cpu.gpr[6] = 2;
  cpu.gpr[8] = 1;  // focused button
  cpu.gpr[10] = result_ptr;
  f.address_space->write32_be(f.stack_top + 0x54u, 0u);
  assert(f.call(kShowMessageBox, cpu) == xam::result::Success);
  assert(f.address_space->read32_be(result_ptr) == 1u);
  assert(f.notifications.dequeue(system.get(), 0, id, param) && id == 0x09u && param == 1u);
  assert(f.notifications.dequeue(system.get(), 0, id, param) && id == 0x09u && param == 0u &&
         "XN_SYS_UI on, then off");
  assert(!f.notifications.dequeue(listener.get(), 0, id, param));

  // Through an XOVERLAPPED, the ninth argument, on the stack.
  const auto overlapped = f.alloc(28);
  for (std::uint32_t i = 0; i < 28u; i += 4u) f.address_space->write32_be(overlapped + i, 0u);
  cpu = cpu::CpuState{};
  cpu.gpr[8] = 0;
  cpu.gpr[10] = result_ptr;
  f.address_space->write32_be(f.stack_top + 0x54u, overlapped);
  assert(f.call(kShowMessageBox, cpu) == kIoPending);
  assert(f.address_space->read32_be(result_ptr) == 0u && f.address_space->read32_be(overlapped) == 0u);

  cpu = cpu::CpuState{};
  f.address_space->write32_be(f.stack_top + 0x54u, 0u);
  assert(f.call(kShowMessageBox, cpu) == xam::result::InvalidParameter && "no result pointer");

  cpu = cpu::CpuState{};
  assert(f.call(kIsUiActive, cpu) == 0u && "no dialog stays open");
  std::cout << "test_a_message_box_answers_with_the_focused_button passed\n";
}

void test_the_keyboard_returns_the_default_text() {
  Fixture f;
  const auto buffer = f.alloc(16u * 2u);
  const auto keyboard = [&](memory::GuestAddress default_text, std::uint32_t length,
                            memory::GuestAddress overlapped) {
    cpu::CpuState cpu{};
    cpu.gpr[5] = default_text;
    cpu.gpr[8] = buffer;
    cpu.gpr[9] = length;
    cpu.gpr[10] = overlapped;
    return f.call(kShowKeyboard, cpu);
  };
  assert(keyboard(f.wide(u"Pilot"), 16u, 0u) == xam::result::Success);
  assert(f.read_wide(buffer, 16u) == u"Pilot");
  assert(keyboard(f.wide(u"Callsign Mobius"), 6u, 0u) == xam::result::Success);
  assert(f.read_wide(buffer, 16u) == u"Calls" && "cut to buffer_length - 1 and terminated");
  assert(keyboard(0u, 16u, 0u) == xam::result::Success);
  assert(f.read_wide(buffer, 16u).empty());

  const auto overlapped = f.alloc(28);
  for (std::uint32_t i = 0; i < 28u; i += 4u) f.address_space->write32_be(overlapped + i, 0u);
  assert(keyboard(f.wide(u"Ace"), 16u, overlapped) == kIoPending);
  assert(f.read_wide(buffer, 16u) == u"Ace");

  cpu::CpuState cpu{};
  assert(f.call(kShowKeyboard, cpu) == xam::result::InvalidParameter && "no buffer");
  std::cout << "test_the_keyboard_returns_the_default_text passed\n";
}

void test_get_gamertag() {
  Fixture f;
  const auto buffer = f.alloc(16u * 2u);
  const auto gamertag = [&](std::uint32_t user, std::uint32_t length) {
    cpu::CpuState cpu{};
    cpu.gpr[3] = user;
    cpu.gpr[4] = buffer;
    cpu.gpr[5] = length;
    return f.call(kGetGamerTag, cpu);
  };
  assert(gamertag(0, 16u) == 0u);
  const auto expected = f.users.gamertag(0);
  assert(f.read_wide(buffer, 16u) == std::u16string(expected.begin(), expected.end()));
  assert(gamertag(0, 15u) == 0x80070057u && "a buffer under 16 units");
  assert(gamertag(1, 16u) == 0x80070525u && "user 1 is not signed in");
  assert(gamertag(4, 16u) == 0x80070057u);
  std::cout << "test_get_gamertag passed\n";
}

}  // namespace

int main() {
  test_a_message_box_answers_with_the_focused_button();
  test_the_keyboard_returns_the_default_text();
  test_get_gamertag();
  std::cout << "All XAM UI tests passed!\n";
  return 0;
}

// Profile settings: xam::ProfileSettings and the XamUserReadProfileSettings
// (0x0219), XamUserWriteProfileSettings (0x021A) and
// XamUserReadProfileSettingsEx (0x022A) exports, driven through
// core::ExportRegistry::invoke().
//
// Before this, any read of one or more settings failed with
// InvalidParameter (an AC6 call asks for three), nothing could be written,
// and the size query titles make first was not answered.

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "xenon/core/export_registry.hpp"
#include "xenon/core/session.hpp"
#include "xenon/kernel/event.hpp"
#include "xenon/kernel/handle_table.hpp"
#include "xenon/kernel/memory.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xam/notification_manager.hpp"
#include "xenon/xam/profile_settings.hpp"
#include "xenon/xam/types.hpp"
#include "xenon/xam/user_manager.hpp"

namespace xenon::xam {
bool register_profile_settings_exports(core::ExportRegistry& registry, UserManager& users,
                                       ProfileSettings& settings, NotificationManager& notifications,
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

constexpr std::uint32_t kRead = 0x0219u;
constexpr std::uint32_t kWrite = 0x021Au;
constexpr std::uint32_t kReadEx = 0x022Au;
constexpr std::uint32_t kInsufficientBuffer = 0x7Au;
constexpr std::uint32_t kNoSuchUser = 0x525u;
constexpr std::uint32_t kIoPending = 0x3E5u;

constexpr std::uint32_t kVibration = 0x10040003u;     // INT32, default 3
constexpr std::uint32_t kMotto = 0x402C0011u;         // WSTRING, 0x2C bytes, default empty
constexpr std::uint32_t kTitleSpecific1 = 0x63E83FFFu;  // BINARY, 1000 bytes
constexpr std::uint32_t kTitleSpecific2 = 0x63E83FFEu;
constexpr std::uint32_t kTitleSpecific3 = 0x63E83FFDu;
constexpr std::uint32_t kTitle = 0x4E4D07D6u;
constexpr xam::XUID kXuid = xam::UserManager::kDefaultOfflineXuid;

std::filesystem::path scratch_directory(const char* name) {
  auto path = std::filesystem::temp_directory_path() / "xenon_profile_settings_tests" / name;
  std::filesystem::remove_all(path);
  return path;
}

void test_the_store_keeps_defaults_titles_and_saved_values() {
  xam::ProfileSettings settings;
  const auto vibration = settings.read(kXuid, kTitle, kVibration);
  assert(vibration && vibration->type == xam::ProfileSettingType::Int32 && vibration->bits == 3u);
  assert(!settings.read(kXuid, kTitle, kTitleSpecific1) && "title-specific starts with no value");
  assert(!settings.read(kXuid, kTitle, 0x10040FFFu) && "an unknown id has no value");

  const auto directory = scratch_directory("store");
  settings.set_storage_directory(directory);
  assert(settings.write(kXuid, kTitle, kTitleSpecific1,
                        {xam::ProfileSettingType::Binary, 0u, {1, 2, 3}}));
  assert(settings.write(kXuid, kTitle, kVibration, {xam::ProfileSettingType::Int32, 1u, {}}));
  assert(!settings.read(kXuid, kTitle + 1u, kTitleSpecific1) && "title-specific is per title");
  assert(settings.read(kXuid, kTitle + 1u, kVibration)->bits == 1u && "profile settings are not");

  // A new store over the same directory sees the saved title-specific value.
  xam::ProfileSettings reloaded;
  reloaded.set_storage_directory(directory);
  const auto saved = reloaded.read(kXuid, kTitle, kTitleSpecific1);
  assert(saved && saved->type == xam::ProfileSettingType::Binary &&
         saved->bytes == (std::vector<std::uint8_t>{1, 2, 3}));
  assert(reloaded.read(kXuid, kTitle, kVibration)->bits == 3u &&
         "only title-specific settings are saved");
  std::filesystem::remove_all(directory);
  std::cout << "test_the_store_keeps_defaults_titles_and_saved_values passed\n";
}

struct Fixture {
  std::shared_ptr<memory::AddressSpace> address_space;
  core::XenonSession session;
  xam::UserManager users;
  xam::ProfileSettings settings;
  xam::NotificationManager notifications;
  core::ExportRegistry registry;
  memory::GuestAddress stack_top{};

  Fixture() {
    address_space =
        std::make_shared<memory::AddressSpace>(memory::GuestTranslationMode::Compact);
    assert(address_space->initialize());
    core::SessionExecutionTestAccess::configure(session, address_space);
    users.initialize();
    assert(xam::register_profile_settings_exports(registry, users, settings, notifications, session));
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

  memory::GuestAddress ids(const std::vector<std::uint32_t>& values) {
    const auto address = alloc(static_cast<std::uint32_t>(values.size() * 4u));
    for (std::size_t i = 0; i < values.size(); ++i) {
      address_space->write32_be(address + static_cast<std::uint32_t>(i * 4u), values[i]);
    }
    return address;
  }

  // Read(title_id, user_index, xuid_count, xuids, count, ids, size_ptr,
  // buffer, overlapped): the ninth argument is on the stack.
  std::uint32_t read(std::uint32_t user_index, std::uint32_t xuid_count,
                     memory::GuestAddress xuids, std::uint32_t count, memory::GuestAddress id_list,
                     memory::GuestAddress size_ptr, memory::GuestAddress buffer,
                     memory::GuestAddress overlapped = 0u) {
    cpu::CpuState cpu{};
    cpu.gpr[1] = stack_top;
    cpu.gpr[3] = kTitle;
    cpu.gpr[4] = user_index;
    cpu.gpr[5] = xuid_count;
    cpu.gpr[6] = xuids;
    cpu.gpr[7] = count;
    cpu.gpr[8] = id_list;
    cpu.gpr[9] = size_ptr;
    cpu.gpr[10] = buffer;
    address_space->write32_be(stack_top + 0x54u, overlapped);
    return call(kRead, cpu);
  }

  std::uint32_t call(std::uint32_t ordinal, cpu::CpuState& cpu) {
    core::ExportCallContext context{cpu, *address_space, 0, 0};
    assert(registry.invoke("xam", ordinal, context).handled);
    return static_cast<std::uint32_t>(cpu.gpr[3]);
  }

  // Reads `id_list` for user 0 into a buffer of exactly the size the call
  // asks for, and returns the buffer.
  memory::GuestAddress read_all(const std::vector<std::uint32_t>& id_list) {
    const auto list = ids(id_list);
    const auto size_ptr = alloc(4);
    address_space->write32_be(size_ptr, 0u);
    const auto count = static_cast<std::uint32_t>(id_list.size());
    assert(read(0, 0, 0, count, list, size_ptr, 0) == kInsufficientBuffer);
    const auto buffer = alloc(address_space->read32_be(size_ptr));
    assert(read(0, 0, 0, count, list, size_ptr, buffer) == xam::result::Success);
    return buffer;
  }

  memory::GuestAddress entry(memory::GuestAddress buffer, std::uint32_t index) {
    return address_space->read32_be(buffer + 4u) + index * 40u;
  }
};

void test_a_size_query_reports_the_size_needed() {
  Fixture f;
  const auto list = f.ids({kTitleSpecific1, kTitleSpecific2, kTitleSpecific3});
  const auto size_ptr = f.alloc(4);
  f.address_space->write32_be(size_ptr, 0u);
  assert(f.read(0, 0, 0, 3, list, size_ptr, 0) == kInsufficientBuffer);
  assert(f.address_space->read32_be(size_ptr) == 8u + 3u * 40u + 3u * 1000u &&
         "header, three entries, and each binary setting's declared size");

  // A non-zero size that is too small is left alone.
  f.address_space->write32_be(size_ptr, 100u);
  assert(f.read(0, 0, 0, 3, list, size_ptr, f.alloc(100)) == kInsufficientBuffer);
  assert(f.address_space->read32_be(size_ptr) == 100u);

  assert(f.read(0, 0, 0, 0, list, size_ptr, 0) == xam::result::InvalidParameter);
  assert(f.read(0, 0, 0, 33, list, size_ptr, 0) == xam::result::InvalidParameter);
  assert(f.read(0, 0, 0, 3, list, 0, 0) == xam::result::InvalidParameter);
  std::cout << "test_a_size_query_reports_the_size_needed passed\n";
}

void test_reading_defaults_and_unset_settings() {
  Fixture f;
  const auto buffer = f.read_all({kVibration, kTitleSpecific1, kMotto});
  assert(f.address_space->read32_be(buffer) == 3u);
  assert(f.address_space->read32_be(buffer + 4u) == buffer + 8u);

  const auto vibration = f.entry(buffer, 0);
  assert(f.address_space->read32_be(vibration + 0x00) == 1u && "source: default");
  assert(f.address_space->read32_be(vibration + 0x08) == 0u && "user index 0");
  assert(f.address_space->read32_be(vibration + 0x10) == kVibration);
  assert(f.address_space->read8(vibration + 0x18) == 1u && "INT32");
  assert(f.address_space->read32_be(vibration + 0x20) == 3u);

  const auto unset = f.entry(buffer, 1);
  assert(f.address_space->read32_be(unset + 0x00) == 0u && "source: no value");
  assert(f.address_space->read32_be(unset + 0x10) == kTitleSpecific1);
  assert(f.address_space->read8(unset + 0x18) == 0u);
  assert(f.address_space->read32_be(unset + 0x20) == 0u && f.address_space->read32_be(unset + 0x24) == 0u);

  const auto motto = f.entry(buffer, 2);
  assert(f.address_space->read32_be(motto + 0x00) == 1u);
  assert(f.address_space->read8(motto + 0x18) == 4u && "WSTRING");
  assert(f.address_space->read32_be(motto + 0x20) == 0u && "empty string: size 0, no pointer");
  std::cout << "test_reading_defaults_and_unset_settings passed\n";
}

void test_a_written_setting_reads_back() {
  Fixture f;
  const auto data = f.alloc(5);
  const std::string text = "hello";
  for (std::uint32_t i = 0; i < 5u; ++i) f.address_space->write8(data + i, static_cast<std::uint8_t>(text[i]));

  // Two entries: TITLE_SPECIFIC1 as BINARY, and one whose data type is not
  // the type its id declares (skipped).
  const auto entries = f.alloc(80);
  for (std::uint32_t i = 0; i < 80u; i += 4u) f.address_space->write32_be(entries + i, 0u);
  f.address_space->write32_be(entries + 0x10, kTitleSpecific1);
  f.address_space->write8(entries + 0x18, 6u);
  f.address_space->write32_be(entries + 0x20, 5u);
  f.address_space->write32_be(entries + 0x24, data);
  f.address_space->write32_be(entries + 40u + 0x10, kTitleSpecific2);
  f.address_space->write8(entries + 40u + 0x18, 1u);
  f.address_space->write32_be(entries + 40u + 0x20, 7u);

  cpu::CpuState cpu{};
  cpu.gpr[3] = kTitle;
  cpu.gpr[4] = 0;
  cpu.gpr[5] = 2;
  cpu.gpr[6] = entries;
  auto listener = f.notifications.create_listener(1u, 0u);
  std::uint32_t id = 0, param = 0;
  while (f.notifications.dequeue(listener.get(), 0, id, param)) {}  // startup notifications
  assert(f.call(kWrite, cpu) == xam::result::Success);
  assert(f.notifications.dequeue(listener.get(), 0, id, param) && id == 0x0000000Eu &&
         param == 1u && "XN_SYS_PROFILESETTINGCHANGED for user 0");

  const auto buffer = f.read_all({kTitleSpecific1, kTitleSpecific2});
  const auto written = f.entry(buffer, 0);
  assert(f.address_space->read32_be(written + 0x00) == 2u && "source: title");
  assert(f.address_space->read8(written + 0x18) == 6u && "BINARY");
  assert(f.address_space->read32_be(written + 0x20) == 5u);
  const auto pointer = f.address_space->read32_be(written + 0x24);
  assert(pointer == buffer + 8u + 2u * 40u && "data follows the entries");
  for (std::uint32_t i = 0; i < 5u; ++i) {
    assert(f.address_space->read8(pointer + i) == static_cast<std::uint8_t>(text[i]));
  }
  assert(f.address_space->read32_be(f.entry(buffer, 1)) == 0u && "the mistyped entry was skipped");

  cpu = cpu::CpuState{};
  cpu.gpr[3] = kTitle;
  cpu.gpr[5] = 0;
  cpu.gpr[6] = entries;
  assert(f.call(kWrite, cpu) == xam::result::InvalidParameter);
  std::cout << "test_a_written_setting_reads_back passed\n";
}

void test_users_xuids_and_overlapped_completion() {
  Fixture f;
  const auto list = f.ids({kVibration});
  const auto size_ptr = f.alloc(4);
  f.address_space->write32_be(size_ptr, 48u);
  const auto buffer = f.alloc(48);

  assert(f.read(1, 0, 0, 1, list, size_ptr, buffer) == kNoSuchUser && "user 1 is not signed in");

  // By XUID: the entry carries the XUID instead of a user index.
  const auto xuids = f.alloc(8);
  f.address_space->write64_be(xuids, kXuid);
  assert(f.read(0, 1, xuids, 1, list, size_ptr, buffer) == xam::result::Success);
  assert(f.address_space->read64_be(f.entry(buffer, 0) + 0x08) == kXuid);
  f.address_space->write64_be(xuids, kXuid + 0x100u);
  assert(f.read(0, 1, xuids, 1, list, size_ptr, buffer) == kNoSuchUser);

  // Through an XOVERLAPPED (the stack-passed ninth argument).
  auto event = std::make_shared<kernel::KernelEvent>(true, false);
  kernel::Handle handle{};
  assert(f.session.kernel_process()->handle_table().insert(event, 0xFFFFFFFFu,
                                                          kernel::HandleFlags::None, handle) ==
         kernel::KernelIoCode::Success);
  const auto overlapped = f.alloc(28);
  for (std::uint32_t i = 0; i < 28u; i += 4u) f.address_space->write32_be(overlapped + i, 0u);
  f.address_space->write32_be(overlapped + 0x0C, handle);
  assert(f.read(0, 0, 0, 1, list, size_ptr, buffer, overlapped) == kIoPending);
  assert(f.address_space->read32_be(overlapped) == 0u && event->signaled());
  assert(f.address_space->read32_be(f.entry(buffer, 0) + 0x20) == 3u);
  assert(f.read(1, 0, 0, 1, list, size_ptr, buffer, overlapped) == kIoPending);
  assert(f.address_space->read32_be(overlapped) == kNoSuchUser &&
         f.address_space->read32_be(overlapped + 0x18) == 0x80070525u);
  // A size error returns at once, not through the overlapped.
  f.address_space->write32_be(size_ptr, 0u);
  assert(f.read(0, 0, 0, 1, list, size_ptr, 0, overlapped) == kInsufficientBuffer);
  std::cout << "test_users_xuids_and_overlapped_completion passed\n";
}

// ReadEx(..., ids, size_ptr, size_ptr2, buffer, overlapped): the buffer and
// overlapped are the ninth and tenth arguments, and the second size pointer
// is used when the first is null, as in xenia-canary.
void test_read_ex_takes_its_buffer_from_the_stack() {
  Fixture f;
  const auto list = f.ids({kVibration});
  const auto size_ptr = f.alloc(4);
  f.address_space->write32_be(size_ptr, 48u);
  const auto buffer = f.alloc(48);
  cpu::CpuState cpu{};
  cpu.gpr[1] = f.stack_top;
  cpu.gpr[3] = kTitle;
  cpu.gpr[7] = 1;
  cpu.gpr[8] = list;
  cpu.gpr[9] = 0;
  cpu.gpr[10] = size_ptr;
  f.address_space->write32_be(f.stack_top + 0x54u, buffer);
  f.address_space->write32_be(f.stack_top + 0x5Cu, 0u);
  assert(f.call(kReadEx, cpu) == xam::result::Success);
  assert(f.address_space->read32_be(f.entry(buffer, 0) + 0x20) == 3u);
  std::cout << "test_read_ex_takes_its_buffer_from_the_stack passed\n";
}

}  // namespace

int main() {
  test_the_store_keeps_defaults_titles_and_saved_values();
  test_a_size_query_reports_the_size_needed();
  test_reading_defaults_and_unset_settings();
  test_a_written_setting_reads_back();
  test_users_xuids_and_overlapped_completion();
  test_read_ex_takes_its_buffer_from_the_stack();
  std::cout << "All profile settings tests passed!\n";
  return 0;
}

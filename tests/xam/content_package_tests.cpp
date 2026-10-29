// Content packages: XamContentCreate/XamContentCreateEx mount a title's
// content (saves, mostly) as "<root>:" over a host directory, and
// XamContentClose, XamContentDelete and XamContentFlush act on it. Driven
// through core::ExportRegistry::invoke() with a real VirtualFileSystem.
//
// Before this, XamContentCreateEx recorded the content in memory only,
// ignored the creation disposition, always reported "created" and mounted
// nothing, so a title's "save:\..." file I/O had nowhere to go and no save
// survived a restart.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "xenon/core/export_registry.hpp"
#include "xenon/core/session.hpp"
#include "xenon/filesystem/virtual_file_system.hpp"
#include "xenon/kernel/memory.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xam/content_manager.hpp"
#include "xenon/xam/xam_session.hpp"
#include "xenon/xam/types.hpp"

namespace xenon::xam {
bool register_content_exports(core::ExportRegistry& registry, ContentManager& content_manager,
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
    session.filesystem_ = std::make_shared<filesystem::VirtualFileSystem>();
    session.xam_ = std::make_unique<xam::XamSession>();
    static_cast<void>(session.xam_->initialize());
  }
};
}  // namespace xenon::core

using namespace xenon;

namespace {

constexpr std::uint32_t kCreate = 0x0258u;
constexpr std::uint32_t kCreateEx = 0x0259u;
constexpr std::uint32_t kClose = 0x025Au;
constexpr std::uint32_t kDelete = 0x025Bu;
constexpr std::uint32_t kFlush = 0x0267u;
constexpr std::uint32_t kCreateNew = 1u;
constexpr std::uint32_t kCreateAlways = 2u;
constexpr std::uint32_t kOpenExisting = 3u;
constexpr std::uint32_t kOpenAlways = 4u;
constexpr std::uint32_t kFileNotFound = 2u;
constexpr std::uint32_t kPathNotFound = 3u;
constexpr std::uint32_t kAccessDenied = 5u;
constexpr std::uint32_t kAlreadyExists = 0xB7u;
constexpr std::uint32_t kIoPending = 0x3E5u;
constexpr std::uint32_t kFunctionFailed = 0x65Bu;

// Windows cannot remove a file that is still open, so cleanup must not
// throw.
void remove_tree(const std::filesystem::path& path) {
  std::error_code error;
  // A run that stopped mid-test may have left directories read-only.
  if (std::filesystem::is_directory(path, error)) {
    std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add, error);
    for (const auto& entry : std::filesystem::recursive_directory_iterator(path, error)) {
      std::filesystem::permissions(entry.path(), std::filesystem::perms::owner_all,
                                   std::filesystem::perm_options::add, error);
    }
  }
  std::filesystem::remove_all(path, error);
}

std::filesystem::path scratch(const char* name) {
  auto path = std::filesystem::temp_directory_path() / "xenon_content_package_tests" / name;
  remove_tree(path);
  return path;
}

struct Fixture {
  std::shared_ptr<memory::AddressSpace> address_space;
  core::XenonSession session;
  xam::ContentManager content_manager;
  core::ExportRegistry registry;
  memory::GuestAddress stack_top{};

  explicit Fixture(const std::filesystem::path& root) {
    address_space =
        std::make_shared<memory::AddressSpace>(memory::GuestTranslationMode::Compact);
    assert(address_space->initialize());
    core::SessionExecutionTestAccess::configure(session, address_space);
    content_manager.initialize();
    content_manager.set_content_root(root);
    assert(xam::register_content_exports(registry, content_manager, session));
    memory::GuestAddress stack{};
    assert(address_space->allocate(0x10000, 0x1000, memory::kReadWrite, false, stack));
    stack_top = stack + 0x10000u - 0x200u;
  }

  memory::GuestAddress alloc(std::uint32_t bytes) {
    memory::GuestAddress address{};
    assert(address_space->allocate(bytes, 8, memory::kReadWrite, false, address));
    for (std::uint32_t i = 0; i < bytes; ++i) address_space->write8(address + i, 0u);
    return address;
  }

  memory::GuestAddress text(const std::string& value) {
    const auto address = alloc(static_cast<std::uint32_t>(value.size() + 1u));
    for (std::size_t i = 0; i < value.size(); ++i) {
      address_space->write8(address + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(value[i]));
    }
    return address;
  }

  // An XCONTENT_DATA for a saved game.
  memory::GuestAddress content(const std::string& file_name, const std::u16string& display) {
    const auto data = alloc(0x134u);
    address_space->write32_be(data, xam::storage_device::HardDisk);
    address_space->write32_be(data + 4u, 1u);
    for (std::size_t i = 0; i < display.size(); ++i) {
      address_space->write16_be(data + 8u + static_cast<std::uint32_t>(i * 2u), display[i]);
    }
    for (std::size_t i = 0; i < file_name.size(); ++i) {
      address_space->write8(data + 264u + static_cast<std::uint32_t>(i),
                            static_cast<std::uint8_t>(file_name[i]));
    }
    return data;
  }

  std::uint32_t call(std::uint32_t ordinal, cpu::CpuState& cpu) {
    if (cpu.gpr[1] == 0u) cpu.gpr[1] = stack_top;
    core::ExportCallContext context{cpu, *address_space, 0, 0};
    assert(registry.invoke("xam", ordinal, context).handled);
    return static_cast<std::uint32_t>(cpu.gpr[3]);
  }

  // XamContentCreateEx(user, root, data, flags, disposition*, license*, 0, 0, overlapped).
  std::uint32_t create(const std::string& root, memory::GuestAddress data, std::uint32_t flags,
                       std::uint32_t* disposition = nullptr, memory::GuestAddress overlapped = 0u) {
    const auto disposition_ptr = alloc(4);
    const auto license_ptr = alloc(4);
    address_space->write32_be(license_ptr, 0xAAAAAAAAu);
    cpu::CpuState cpu{};
    cpu.gpr[3] = 0;
    cpu.gpr[4] = text(root);
    cpu.gpr[5] = data;
    cpu.gpr[6] = flags;
    cpu.gpr[7] = disposition_ptr;
    cpu.gpr[8] = license_ptr;
    cpu.gpr[1] = stack_top;
    address_space->write32_be(stack_top + 0x54u, overlapped);
    const auto result = call(kCreateEx, cpu);
    if (disposition) *disposition = address_space->read32_be(disposition_ptr);
    if (result == 0u) assert(address_space->read32_be(license_ptr) == 0u);
    return result;
  }

  std::uint32_t root_call(std::uint32_t ordinal, const std::string& root) {
    cpu::CpuState cpu{};
    cpu.gpr[3] = text(root);
    return call(ordinal, cpu);
  }

  std::uint32_t remove(memory::GuestAddress data) {
    cpu::CpuState cpu{};
    cpu.gpr[4] = data;
    return call(kDelete, cpu);
  }

  bool write_file(const std::string& path, const std::string& contents) {
    filesystem::OpenOptions options{};
    options.access = filesystem::FileAccess::Read | filesystem::FileAccess::Write;
    options.disposition = filesystem::CreateDisposition::OverwriteIf;
    std::unique_ptr<filesystem::FileHandle> file;
    if (session.filesystem()->open(path, options, file) != filesystem::FsError::None) return false;
    std::size_t written = 0;
    const auto bytes = std::as_bytes(std::span(contents.data(), contents.size()));
    return file->write(bytes, written) == filesystem::FsError::None && written == contents.size();
  }

  bool exists(const std::string& path) {
    filesystem::FileInfo info{};
    return session.filesystem()->stat(path, info) == filesystem::FsError::None;
  }
};

void test_create_mounts_writes_reach_the_host_and_close_unmounts() {
  const auto root = scratch("mount");
  Fixture f(root);
  const auto data = f.content("slot1", u"Slot 1");
  std::uint32_t disposition = 0;
  assert(f.create("save", data, kOpenAlways, &disposition) == xam::result::Success);
  assert(disposition == 1u && "created");
  assert(f.write_file("save:\\progress.dat", "level 3"));

  // xenia's layout: <root>/<title id>/<content type>/<file name>/.
  const auto package = root / "00000000" / "00000001" / "slot1";
  std::string contents;
  {
    std::ifstream host(package / "progress.dat");
    std::getline(host, contents);
  }
  assert(contents == "level 3");

  assert(f.root_call(kFlush, "save") == xam::result::Success);
  assert(f.root_call(kClose, "save") == xam::result::Success);
  assert(!f.exists("save:\\progress.dat") && "the root is unmounted");
  assert(f.root_call(kClose, "save") == kFileNotFound);

  assert(f.create("SAVE", data, kOpenExisting, &disposition) == xam::result::Success);
  assert(disposition == 2u && "opened");
  assert(f.exists("save:\\progress.dat") && "root names are case-insensitive");
  assert(f.root_call(kClose, "save") == xam::result::Success);
  remove_tree(root);
  std::cout << "test_create_mounts_writes_reach_the_host_and_close_unmounts passed\n";
}

void test_creation_dispositions() {
  const auto root = scratch("dispositions");
  Fixture f(root);
  const auto data = f.content("slot1", u"Slot 1");
  std::uint32_t disposition = 0;
  assert(f.create("save", data, kOpenExisting) == kPathNotFound);
  assert(f.create("save", data, kCreateNew, &disposition) == xam::result::Success && disposition == 1u);
  assert(f.write_file("save:\\a.dat", "a"));
  assert(f.root_call(kClose, "save") == xam::result::Success);

  assert(f.create("save", data, kCreateNew) == kAlreadyExists);
  assert(f.create("save", f.content("slot2", u"Slot 2"), kOpenAlways) == xam::result::Success);
  assert(f.create("save", data, kOpenAlways) == kAlreadyExists && "the root name is in use");
  assert(f.root_call(kClose, "save") == xam::result::Success);

  // CREATE_ALWAYS replaces the package.
  assert(f.create("save", data, kCreateAlways, &disposition) == xam::result::Success && disposition == 1u);
  assert(!f.exists("save:\\a.dat"));
  assert(f.root_call(kClose, "save") == xam::result::Success);

  assert(f.create("save", f.content("..", u"x"), kOpenAlways) == xam::result::InvalidParameter);
  assert(f.create("save", f.content("a\\b", u"x"), kOpenAlways) == xam::result::InvalidParameter);
  assert(f.create("save", data, 0u) == xam::result::InvalidParameter);
  remove_tree(root);
  std::cout << "test_creation_dispositions passed\n";
}

void test_delete_and_persistence_across_sessions() {
  const auto root = scratch("persistence");
  {
    Fixture f(root);
    const auto data = f.content("slot1", u"Pilot \u00E9");
    assert(f.create("save", data, kCreateNew) == xam::result::Success);
    assert(f.write_file("save:\\progress.dat", "kept"));
    assert(f.remove(data) == kAccessDenied && "open content cannot be deleted");
    assert(f.root_call(kClose, "save") == xam::result::Success);

    const auto doomed = f.content("slot2", u"Slot 2");
    assert(f.create("save", doomed, kCreateNew) == xam::result::Success);
    assert(f.root_call(kClose, "save") == xam::result::Success);
    assert(f.remove(doomed) == xam::result::Success);
    assert(f.remove(doomed) == kFileNotFound);
    assert(f.create("save", doomed, kOpenExisting) == kPathNotFound);
  }
  // A new session over the same root lists the saved content, with its
  // display name, and opens it with the data intact.
  Fixture f(root);
  const auto listed =
      f.content_manager.enumerate_content(xam::storage_device::HardDisk, xam::ContentType::SavedGame);
  assert(listed.size() == 1u && listed[0].file_name == "slot1");
  assert(listed[0].display_name == "Pilot \xC3\xA9");
  std::uint32_t disposition = 0;
  assert(f.create("save", f.content("slot1", u"Pilot"), kOpenExisting, &disposition) ==
         xam::result::Success && disposition == 2u);
  assert(f.exists("save:\\progress.dat"));
  remove_tree(root);
  std::cout << "test_delete_and_persistence_across_sessions passed\n";
}

void test_overlapped_results_and_xam_content_create() {
  const auto root = scratch("overlapped");
  Fixture f(root);
  const auto data = f.content("slot1", u"Slot 1");
  const auto overlapped = f.alloc(28);
  assert(f.create("save", data, kCreateNew, nullptr, overlapped) == kIoPending);
  assert(f.address_space->read32_be(overlapped) == 0u);
  assert(f.address_space->read32_be(overlapped + 4u) == 1u && "Length is the disposition");
  assert(f.root_call(kClose, "save") == xam::result::Success);

  assert(f.create("save", data, kCreateNew, nullptr, overlapped) == kIoPending);
  assert(f.address_space->read32_be(overlapped) == kFunctionFailed);
  assert(f.address_space->read32_be(overlapped + 0x18u) == 0x800700B7u);

  // XamContentCreate takes its overlapped in r9.
  cpu::CpuState cpu{};
  cpu.gpr[4] = f.text("save");
  cpu.gpr[5] = data;
  cpu.gpr[6] = kOpenExisting;
  cpu.gpr[7] = f.alloc(4);
  cpu.gpr[9] = overlapped;
  assert(f.call(kCreate, cpu) == kIoPending);
  assert(f.address_space->read32_be(overlapped) == 0u &&
         f.address_space->read32_be(overlapped + 4u) == 2u);
  assert(f.root_call(kClose, "save") == xam::result::Success);
  remove_tree(root);
  std::cout << "test_overlapped_results_and_xam_content_create passed\n";
}

// A package the host fails to remove stays listed and the delete reports
// ERROR_ACCESS_DENIED instead of success. Made unremovable here by a
// read-only parent directory, which only POSIX non-root processes honour
// (on Windows a file left open has the same effect).
void test_a_package_the_host_cannot_remove_is_not_reported_deleted() {
#if !defined(_WIN32)
  if (geteuid() == 0) return;
  const auto root = scratch("undeletable");
  Fixture f(root);
  const auto data = f.content("slot1", u"Slot 1");
  assert(f.create("save", data, kCreateNew) == xam::result::Success);
  assert(f.write_file("save:\\progress.dat", "kept"));
  assert(f.root_call(kClose, "save") == xam::result::Success);
  const auto type_directory = root / "00000000" / "00000001";
  std::filesystem::permissions(type_directory, std::filesystem::perms::owner_read |
                                                   std::filesystem::perms::owner_exec);
  std::filesystem::permissions(type_directory / "slot1",
                               std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
  assert(f.remove(data) == kAccessDenied);
  assert(f.content_manager.enumerate_content(xam::storage_device::HardDisk,
                                             xam::ContentType::SavedGame).size() == 1u);
  std::filesystem::permissions(type_directory / "slot1", std::filesystem::perms::owner_all);
  std::filesystem::permissions(type_directory, std::filesystem::perms::owner_all);
  assert(f.remove(data) == xam::result::Success);
  remove_tree(root);
  std::cout << "test_a_package_the_host_cannot_remove_is_not_reported_deleted passed\n";
#endif
}

// XamContentSetThumbnail/GetThumbnail keep a PNG with the package;
// XamContentGetCreator reports the signed-in user as a saved game's creator.
void test_thumbnails_and_creator() {
  const auto root = scratch("thumbnails");
  Fixture f(root);
  const auto data = f.content("slot1", u"Slot 1");
  assert(f.create("save", data, kCreateNew) == xam::result::Success);
  assert(f.root_call(kClose, "save") == xam::result::Success);

  const std::string png = "\x89PNG\r\n\x1a\n-thumbnail-";
  const auto png_ptr = f.text(png);
  cpu::CpuState cpu{};
  cpu.gpr[4] = data;
  cpu.gpr[5] = png_ptr;
  cpu.gpr[6] = static_cast<std::uint32_t>(png.size());
  assert(f.call(0x0260u, cpu) == xam::result::Success);
  assert(std::filesystem::exists(root / "00000000" / "00000001" / "slot1" / "__thumbnail.png"));

  const auto size_ptr = f.alloc(4);
  const auto get = [&](memory::GuestAddress buffer, std::uint32_t capacity, memory::GuestAddress which) {
    f.address_space->write32_be(size_ptr, capacity);
    cpu::CpuState c{};
    c.gpr[4] = which;
    c.gpr[5] = buffer;
    c.gpr[6] = size_ptr;
    return f.call(0x0261u, c);
  };
  assert(get(0u, 0u, data) == xam::result::Success && "a size query");
  assert(f.address_space->read32_be(size_ptr) == png.size());
  const auto out = f.alloc(64);
  assert(get(out, 4u, data) == 0x7Au && "ERROR_INSUFFICIENT_BUFFER");
  assert(get(out, 64u, data) == xam::result::Success);
  for (std::size_t i = 0; i < png.size(); ++i) {
    assert(f.address_space->read8(out + static_cast<std::uint32_t>(i)) == static_cast<std::uint8_t>(png[i]));
  }
  const auto missing = f.content("nothing", u"x");
  assert(get(out, 64u, missing) == kFileNotFound);
  cpu = cpu::CpuState{};
  cpu.gpr[4] = missing;
  cpu.gpr[5] = png_ptr;
  cpu.gpr[6] = 4u;
  assert(f.call(0x0260u, cpu) == kFileNotFound && "no phantom package is created");
  assert(!std::filesystem::exists(root / "00000000" / "00000001" / "nothing"));

  const auto is_creator = f.alloc(4);
  const auto xuid = f.alloc(8);
  const auto creator = [&](memory::GuestAddress which) {
    cpu::CpuState c{};
    c.gpr[3] = 0;
    c.gpr[4] = which;
    c.gpr[5] = is_creator;
    c.gpr[6] = xuid;
    return f.call(0x0262u, c);
  };
  assert(creator(data) == xam::result::Success);
  assert(f.address_space->read32_be(is_creator) == 1u);
  assert(f.address_space->read64_be(xuid) == xam::UserManager::kDefaultOfflineXuid);
  assert(creator(missing) == kPathNotFound);
  remove_tree(root);
  std::cout << "test_thumbnails_and_creator passed\n";
}

}  // namespace

int main() {
  test_create_mounts_writes_reach_the_host_and_close_unmounts();
  test_creation_dispositions();
  test_delete_and_persistence_across_sessions();
  test_overlapped_results_and_xam_content_create();
  test_a_package_the_host_cannot_remove_is_not_reported_deleted();
  test_thumbnails_and_creator();
  std::cout << "All content package tests passed!\n";
  return 0;
}

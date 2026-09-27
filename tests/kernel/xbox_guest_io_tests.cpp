#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "xenon/filesystem/host_path_device.hpp"
#include "xenon/filesystem/virtual_file_system.hpp"
#include "xenon/kernel/xbox_io_guest.hpp"
#include "xenon/memory/address_space.hpp"

namespace fs = xenon::filesystem;
namespace kernel = xenon::kernel;
namespace xbox = xenon::kernel::xbox;
namespace memory = xenon::memory;

namespace {

constexpr memory::GuestAddress kGuestBase = 0x00100000u;
constexpr memory::GuestAddress kAnsi = kGuestBase + 0x100u;
constexpr memory::GuestAddress kObjectAttributes = kGuestBase + 0x120u;
constexpr memory::GuestAddress kHandleOut = kGuestBase + 0x140u;
constexpr memory::GuestAddress kIosb = kGuestBase + 0x148u;
constexpr memory::GuestAddress kPath = kGuestBase + 0x200u;
constexpr memory::GuestAddress kInfo = kGuestBase + 0x800u;
constexpr memory::GuestAddress kBuffer = kGuestBase + 0x2000u;
constexpr memory::GuestAddress kBuffer2 = kGuestBase + 0x4000u;
constexpr memory::GuestAddress kSegmentArray = kGuestBase + 0x600u;

class TempDirectory {
 public:
  TempDirectory() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("xenon_xbox_guest_io_" + std::to_string(now));
    std::filesystem::create_directories(path_);
  }
  ~TempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_{};
};

void write_file(const std::filesystem::path& path, std::string_view data) {
  std::ofstream stream(path, std::ios::binary);
  assert(stream);
  stream.write(data.data(), static_cast<std::streamsize>(data.size()));
  assert(stream);
}

std::shared_ptr<fs::VirtualFileSystem> make_vfs(const std::filesystem::path& root) {
  auto vfs = std::make_shared<fs::VirtualFileSystem>();
  fs::HostPathDeviceOptions options{};
  options.read_only = false;
  options.create_root = true;
  auto device = std::make_shared<fs::HostPathDevice>(
      "\\Device\\Harddisk0\\Partition1", root, options);
  assert(vfs->register_device(device) == fs::FsError::None);
  assert(vfs->register_symbolic_link("game:", device->mount_point()) ==
         fs::FsError::None);
  return vfs;
}

void write_ansi(memory::AddressSpace& mem, memory::GuestAddress descriptor,
                memory::GuestAddress storage, std::string_view value) {
  mem.write16_be(descriptor + 0, static_cast<std::uint16_t>(value.size()));
  mem.write16_be(descriptor + 2, static_cast<std::uint16_t>(value.size() + 1));
  mem.write32_be(descriptor + 4, storage);
  mem.write_bytes(storage, std::as_bytes(std::span(value.data(), value.size())));
  mem.write8(storage + static_cast<std::uint32_t>(value.size()), 0);
}

void write_object_attributes(memory::AddressSpace& mem, kernel::Handle root,
                             memory::GuestAddress ansi) {
  mem.write32_be(kObjectAttributes + 0, root);
  mem.write32_be(kObjectAttributes + 4, ansi);
  mem.write32_be(kObjectAttributes + 8, 0x40u);  // OBJ_CASE_INSENSITIVE
}

std::string read_guest_string(memory::AddressSpace& mem,
                              memory::GuestAddress address,
                              std::size_t length) {
  std::vector<std::byte> bytes(length);
  mem.read_bytes(address, bytes);
  std::string result(length, '\0');
  for (std::size_t i = 0; i < length; ++i) {
    result[i] = static_cast<char>(std::to_integer<unsigned char>(bytes[i]));
  }
  return result;
}

kernel::Handle open_guest_file(xbox::GuestIoBridge& bridge,
                               memory::AddressSpace& mem,
                               std::string_view path,
                               std::uint32_t desired_access,
                               std::uint32_t create_options,
                               kernel::Handle root = kernel::kInvalidHandle) {
  write_ansi(mem, kAnsi, kPath, path);
  write_object_attributes(mem, root, kAnsi);
  mem.write32_be(kHandleOut, 0);
  mem.write64_be(kIosb, 0);
  const auto status = bridge.nt_create_file(
      kHandleOut, desired_access, kObjectAttributes, kIosb, 0, 0,
      xbox::share::All, 1, create_options);
  assert(status == xbox::status::Success);
  assert(mem.read32_be(kIosb) == xbox::status::Success);
  assert(mem.read32_be(kIosb + 4) ==
         static_cast<std::uint32_t>(xbox::FileAction::Opened));
  const auto handle = mem.read32_be(kHandleOut);
  assert(handle != kernel::kInvalidHandle);
  return handle;
}

void test_create_read_write_query_and_rooted_open() {
  TempDirectory temp;
  write_file(temp.path() / "data.bin", "abcdef");
  std::filesystem::create_directories(temp.path() / "Dir");
  write_file(temp.path() / "Dir" / "child.txt", "child");

  memory::AddressSpace mem(memory::GuestTranslationMode::Compact);
  assert(mem.initialize());
  assert(mem.commit_fixed(kGuestBase, 0x10000u, memory::kReadWrite));
  kernel::KernelIoManager io(make_vfs(temp.path()));
  xbox::GuestIoBridge bridge(mem, io);

  const auto file = open_guest_file(
      bridge, mem, "game:\\data.bin",
      xbox::access::GenericRead | xbox::access::GenericWrite,
      xbox::create_option::SynchronousIoNonAlert);

  mem.fill_bytes(kBuffer, 16, 0);
  auto status = bridge.nt_read_file(file, kernel::kInvalidHandle, 0, 0,
                                    kIosb, kBuffer, 3, 0);
  assert(status == xbox::status::Success);
  assert(mem.read32_be(kIosb) == xbox::status::Success);
  assert(mem.read32_be(kIosb + 4) == 3);
  assert(read_guest_string(mem, kBuffer, 3) == "abc");

  status = bridge.nt_query_information_file(
      file, kIosb, kInfo, 8,
      static_cast<std::uint32_t>(xbox::FileInformationClass::Position));
  assert(status == xbox::status::Success);
  assert(mem.read64_be(kInfo) == 3);
  assert(mem.read32_be(kIosb + 4) == 8);

  mem.write64_be(kInfo, 1);
  status = bridge.nt_set_information_file(
      file, kIosb, kInfo, 8,
      static_cast<std::uint32_t>(xbox::FileInformationClass::Position));
  assert(status == xbox::status::Success);

  const std::string replacement = "ZZ";
  mem.write_bytes(kBuffer, std::as_bytes(std::span(replacement.data(), replacement.size())));
  status = bridge.nt_write_file(file, kernel::kInvalidHandle, 0, 0,
                                kIosb, kBuffer, 2, 0);
  assert(status == xbox::status::Success);
  assert(mem.read32_be(kIosb + 4) == 2);

  // Directory root handle followed by a relative NtCreateFile-style open.
  const auto directory = open_guest_file(
      bridge, mem, "game:\\Dir", xbox::access::GenericRead,
      xbox::create_option::DirectoryFile |
          xbox::create_option::SynchronousIoNonAlert);
  const auto child = open_guest_file(
      bridge, mem, "child.txt", xbox::access::GenericRead,
      xbox::create_option::NonDirectoryFile |
          xbox::create_option::SynchronousIoNonAlert,
      directory);

  write_ansi(mem, kAnsi, kPath, "child.txt");
  write_object_attributes(mem, directory, kAnsi);
  status = bridge.nt_query_full_attributes_file(kObjectAttributes, kInfo);
  assert(status == xbox::status::Success);
  assert(mem.read64_be(kInfo + 0x28) == 5);

  assert(io.close(child) == kernel::KernelIoCode::Success);
  assert(io.close(directory) == kernel::KernelIoCode::Success);
  assert(io.close(file) == kernel::KernelIoCode::Success);

  std::ifstream result(temp.path() / "data.bin", std::ios::binary);
  std::string contents((std::istreambuf_iterator<char>(result)), {});
  assert(contents == "aZZdef");
}

void test_async_apc_memory_fault_and_scatter() {
  TempDirectory temp;
  std::string payload(5000, 'A');
  for (std::size_t i = 4096; i < payload.size(); ++i) payload[i] = 'B';
  write_file(temp.path() / "scatter.bin", payload);

  memory::AddressSpace mem(memory::GuestTranslationMode::Compact);
  assert(mem.initialize());
  assert(mem.commit_fixed(kGuestBase, 0x10000u, memory::kReadWrite));
  kernel::KernelIoManager io(make_vfs(temp.path()));
  xbox::GuestIoBridge bridge(mem, io);

  bool apc_queued = false;
  bridge.set_apc_queue([&](memory::GuestAddress routine,
                           memory::GuestAddress context,
                           memory::GuestAddress iosb) {
    assert(routine == 0x12345678u);
    assert(context == 0xCAFEBABEu);
    assert(iosb == kIosb);
    apc_queued = true;
  });

  const auto async_file = open_guest_file(
      bridge, mem, "game:\\scatter.bin", xbox::access::GenericRead, 0);
  auto status = bridge.nt_read_file(async_file, kernel::kInvalidHandle,
                                    0x12345679u, 0xCAFEBABEu, kIosb,
                                    kBuffer, 4, 0);
  assert(status == xbox::status::Pending);
  assert(mem.read32_be(kIosb) == xbox::status::Success);
  assert(mem.read32_be(kIosb + 4) == 4);
  assert(apc_queued);

  // Invalid guest destination is rejected before filesystem I/O, so the shared
  // file position remains unchanged.
  mem.write64_be(kInfo, 0);
  status = bridge.nt_set_information_file(
      async_file, kIosb, kInfo, 8,
      static_cast<std::uint32_t>(xbox::FileInformationClass::Position));
  assert(status == xbox::status::Success);
  status = bridge.nt_read_file(async_file, kernel::kInvalidHandle, 0, 0,
                               kIosb, 0x00200000u, 8, 0);
  assert(status == xbox::status::AccessViolation);
  status = bridge.nt_query_information_file(
      async_file, kIosb, kInfo, 8,
      static_cast<std::uint32_t>(xbox::FileInformationClass::Position));
  assert(status == xbox::status::Success);
  assert(mem.read64_be(kInfo) == 0);
  assert(io.close(async_file) == kernel::KernelIoCode::Success);

  const auto sync_file = open_guest_file(
      bridge, mem, "game:\\scatter.bin", xbox::access::GenericRead,
      xbox::create_option::SynchronousIoNonAlert);
  mem.write32_be(kSegmentArray + 0, kBuffer);
  mem.write32_be(kSegmentArray + 4, kBuffer2);
  status = bridge.nt_read_file_scatter(
      sync_file, kernel::kInvalidHandle, 0, 0, kIosb, kSegmentArray,
      static_cast<std::uint32_t>(payload.size()), 0);
  assert(status == xbox::status::Success);
  assert(mem.read32_be(kIosb + 4) == payload.size());
  assert(read_guest_string(mem, kBuffer, 4) == "AAAA");
  assert(read_guest_string(mem, kBuffer + 4092, 4) == "AAAA");
  assert(read_guest_string(mem, kBuffer2, 4) == "BBBB");
  assert(io.close(sync_file) == kernel::KernelIoCode::Success);
}

void test_directory_and_volume_marshalling() {
  TempDirectory temp;
  std::filesystem::create_directories(temp.path() / "List");
  write_file(temp.path() / "List" / "one.bin", "1");
  write_file(temp.path() / "List" / "two.txt", "22");

  memory::AddressSpace mem(memory::GuestTranslationMode::Compact);
  assert(mem.initialize());
  assert(mem.commit_fixed(kGuestBase, 0x10000u, memory::kReadWrite));
  kernel::KernelIoManager io(make_vfs(temp.path()));
  xbox::GuestIoBridge bridge(mem, io);

  const auto directory = open_guest_file(
      bridge, mem, "game:\\List", xbox::access::GenericRead,
      xbox::create_option::DirectoryFile |
          xbox::create_option::SynchronousIoNonAlert);

  write_ansi(mem, kAnsi, kPath, "*.bin");
  auto status = bridge.nt_query_directory_file(
      directory, kernel::kInvalidHandle, 0, 0, kIosb, kInfo, 256, kAnsi, true);
  assert(status == xbox::status::Success);
  assert(mem.read32_be(kInfo + 0x3C) == 7);
  assert(read_guest_string(mem, kInfo + 0x40, 7) == "one.bin");
  assert(mem.read32_be(kIosb + 4) >= 0x47u);

  status = bridge.nt_query_volume_information_file(
      directory, kIosb, kInfo, 64,
      static_cast<std::uint32_t>(xbox::FsInformationClass::Size));
  assert(status == xbox::status::Success);
  assert(mem.read32_be(kInfo + 0x14) == 0x200u);
  assert(mem.read32_be(kIosb + 4) == 24);

  status = bridge.nt_query_volume_information_file(
      directory, kIosb, kInfo, 64,
      static_cast<std::uint32_t>(xbox::FsInformationClass::Attribute));
  assert(status == xbox::status::Success);
  const auto name_length = mem.read32_be(kInfo + 8);
  assert(name_length != 0);
  assert(read_guest_string(mem, kInfo + 12, name_length) == "XENON");

  assert(io.close(directory) == kernel::KernelIoCode::Success);
}

}  // namespace

int main() {
  test_create_read_write_query_and_rooted_open();
  test_async_apc_memory_fault_and_scatter();
  test_directory_and_volume_marshalling();
  return 0;
}

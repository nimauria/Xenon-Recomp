#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "xenon/filesystem/host_path_device.hpp"
#include "xenon/filesystem/path.hpp"
#include "xenon/filesystem/virtual_file_system.hpp"

namespace fs = xenon::filesystem;

namespace {

class TempDirectory {
 public:
  TempDirectory() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("xenon_filesystem_v1_" + std::to_string(stamp));
    std::error_code ec;
    std::filesystem::create_directories(path_, ec);
    assert(!ec);
  }

  ~TempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_{};
};

std::shared_ptr<fs::HostPathDevice> mount_host(fs::VirtualFileSystem& vfs,
                                               const std::filesystem::path& root,
                                               bool read_only = false) {
  fs::HostPathDeviceOptions options{};
  options.read_only = read_only;
  options.create_root = !read_only;
  options.case_insensitive = true;
  auto device = std::make_shared<fs::HostPathDevice>(
      "\\Device\\Harddisk0\\Partition1", root, options);
  assert(vfs.register_device(device) == fs::FsError::None);
  assert(vfs.register_symbolic_link("game:", device->mount_point()) ==
         fs::FsError::None);
  assert(vfs.register_symbolic_link("d:", "game:") == fs::FsError::None);
  return device;
}

void test_path_normalization() {
  std::string path;
  assert(fs::normalize_guest_path("game:/Data/./Maps/../plane.bin", path) ==
         fs::FsError::None);
  assert(path == "game:\\Data\\plane.bin");

  assert(fs::normalize_guest_path(
             "\\Device\\Harddisk0\\Partition1\\a\\..\\b", path, false) ==
         fs::FsError::None);
  assert(path == "\\Device\\Harddisk0\\Partition1\\b");

  assert(fs::normalize_guest_path("../../escape.bin", path) ==
         fs::FsError::InvalidPath);
  assert(fs::normalize_guest_path("game:\\..\\escape.bin", path) ==
         fs::FsError::InvalidPath);
  assert(fs::guest_path_equal("GAME:\\DATA", "game:\\data"));
  assert(fs::guest_path_has_prefix("\\Device\\Harddisk0\\Partition1\\foo",
                                   "\\device\\harddisk0\\partition1"));
}

void test_mount_links_and_relative_paths() {
  TempDirectory temp;
  std::filesystem::create_directories(temp.path() / "Content");
  {
    std::ofstream file(temp.path() / "Content" / "Example.BIN", std::ios::binary);
    file << "xenon";
  }

  fs::VirtualFileSystem vfs;
  mount_host(vfs, temp.path());

  fs::ResolvedPath resolved;
  assert(vfs.resolve("D:/content/example.bin", resolved) == fs::FsError::None);
  assert(resolved.relative_path == "content\\example.bin");
  assert(fs::guest_path_equal(
      resolved.canonical_path,
      "\\Device\\Harddisk0\\Partition1\\content\\example.bin"));

  fs::FileInfo info{};
  assert(vfs.stat("Content\\EXAMPLE.bin", info) == fs::FsError::None);
  assert(info.size == 5);
  assert(!info.is_directory);
}

void test_file_lifecycle() {
  TempDirectory temp;
  fs::VirtualFileSystem vfs;
  mount_host(vfs, temp.path());

  assert(vfs.create_directory("game:\\Saves\\Slot01", true) ==
         fs::FsError::None);

  std::unique_ptr<fs::FileHandle> file;
  fs::OpenOptions create{};
  create.access = fs::FileAccess::Read | fs::FileAccess::Write;
  create.disposition = fs::CreateDisposition::Create;
  assert(vfs.open("game:\\Saves\\Slot01\\save.bin", create, file) ==
         fs::FsError::None);

  const std::array<std::byte, 6> payload{
      std::byte{0x58}, std::byte{0x45}, std::byte{0x4E},
      std::byte{0x4F}, std::byte{0x4E}, std::byte{0x21}};
  std::size_t transferred = 0;
  assert(file->write(payload, transferred) == fs::FsError::None);
  assert(transferred == payload.size());
  assert(file->tell() == payload.size());
  assert(file->flush() == fs::FsError::None);

  std::array<std::byte, 6> readback{};
  assert(file->read_at(0, readback, transferred) == fs::FsError::None);
  assert(transferred == readback.size());
  assert(readback == payload);
  assert(file->tell() == payload.size());

  std::uint64_t position = 0;
  assert(file->seek(-1, fs::SeekOrigin::End, position) == fs::FsError::None);
  assert(position == payload.size() - 1);
  assert(file->resize(4) == fs::FsError::None);
  std::uint64_t size = 0;
  assert(file->size(size) == fs::FsError::None);
  assert(size == 4);
  file.reset();

  std::vector<fs::DirectoryEntry> entries;
  assert(vfs.list("game:\\saves\\slot01", entries) == fs::FsError::None);
  assert(entries.size() == 1);
  assert(fs::guest_path_equal(entries.front().name, "save.bin"));
  assert(entries.front().info.size == 4);

  assert(vfs.rename("game:\\Saves\\Slot01\\save.bin",
                    "game:\\Saves\\Slot01\\renamed.bin") ==
         fs::FsError::None);
  fs::FileInfo info{};
  assert(vfs.stat("game:\\saves\\slot01\\RENAMED.BIN", info) ==
         fs::FsError::None);
  assert(info.size == 4);

  assert(vfs.remove("game:\\Saves\\Slot01\\renamed.bin") ==
         fs::FsError::None);
  assert(vfs.remove("game:\\Saves\\Slot01") == fs::FsError::None);
  assert(vfs.stat("game:\\Saves\\Slot01", info) == fs::FsError::NotFound);
}

void test_dispositions() {
  TempDirectory temp;
  fs::VirtualFileSystem vfs;
  mount_host(vfs, temp.path());

  std::unique_ptr<fs::FileHandle> file;
  fs::OpenOptions options{};
  options.access = fs::FileAccess::Write;
  options.disposition = fs::CreateDisposition::Open;
  assert(vfs.open("game:\\missing.bin", options, file) == fs::FsError::NotFound);

  options.disposition = fs::CreateDisposition::OpenIf;
  assert(vfs.open("game:\\created.bin", options, file) == fs::FsError::None);
  file.reset();

  options.disposition = fs::CreateDisposition::Create;
  assert(vfs.open("game:\\created.bin", options, file) ==
         fs::FsError::AlreadyExists);

  options.disposition = fs::CreateDisposition::Overwrite;
  assert(vfs.open("game:\\created.bin", options, file) == fs::FsError::None);
}

void test_read_only_mount() {
  TempDirectory temp;
  {
    std::ofstream file(temp.path() / "default.dat", std::ios::binary);
    file << "readonly";
  }

  fs::VirtualFileSystem vfs;
  mount_host(vfs, temp.path(), true);

  std::unique_ptr<fs::FileHandle> file;
  fs::OpenOptions read{};
  assert(vfs.open("game:\\DEFAULT.dat", read, file) == fs::FsError::None);
  file.reset();
  read.disposition = fs::CreateDisposition::OpenIf;
  assert(vfs.open("game:\\default.dat", read, file) == fs::FsError::None);
  file.reset();
  assert(vfs.open("game:\\missing.dat", read, file) == fs::FsError::AccessDenied);

  fs::OpenOptions write{};
  write.access = fs::FileAccess::Write;
  assert(vfs.open("game:\\default.dat", write, file) == fs::FsError::ReadOnly);
  assert(vfs.create_directory("game:\\newdir") == fs::FsError::ReadOnly);
  assert(vfs.remove("game:\\default.dat") == fs::FsError::ReadOnly);
}

void test_longest_mount_wins() {
  TempDirectory root;
  TempDirectory partition;
  std::filesystem::create_directories(root.path() / "Partition1");
  {
    std::ofstream file(partition.path() / "selected.txt");
    file << "partition";
  }

  fs::VirtualFileSystem vfs;
  fs::HostPathDeviceOptions options{};
  options.read_only = false;
  auto broad = std::make_shared<fs::HostPathDevice>(
      "\\Device\\Harddisk0", root.path(), options);
  auto specific = std::make_shared<fs::HostPathDevice>(
      "\\Device\\Harddisk0\\Partition1", partition.path(), options);
  assert(vfs.register_device(broad) == fs::FsError::None);
  assert(vfs.register_device(specific) == fs::FsError::None);

  fs::FileInfo info{};
  assert(vfs.stat("\\Device\\Harddisk0\\Partition1\\selected.txt", info) ==
         fs::FsError::None);
  assert(info.size == 9);
}

void test_symbolic_link_loop_guard() {
  fs::VirtualFileSystem vfs;
  assert(vfs.register_symbolic_link("a:", "b:") == fs::FsError::None);
  assert(vfs.register_symbolic_link("b:", "a:") == fs::FsError::None);
  fs::ResolvedPath resolved;
  assert(vfs.resolve("a:\\loop.bin", resolved) == fs::FsError::TooManyLinks);
}


void test_host_symlink_escape_is_rejected_when_supported() {
  TempDirectory mounted;
  TempDirectory outside;
  {
    std::ofstream file(outside.path() / "secret.bin", std::ios::binary);
    file << "outside";
  }

  std::error_code ec;
  std::filesystem::create_directory_symlink(outside.path(), mounted.path() / "escape", ec);
  if (ec) {
    // Windows may require Developer Mode/elevation for symlink creation. The
    // traversal guard is still covered separately, so skip only this host-OS
    // capability check when links cannot be created.
    return;
  }

  fs::VirtualFileSystem vfs;
  mount_host(vfs, mounted.path());
  fs::FileInfo info{};
  assert(vfs.stat("game:\\escape\\secret.bin", info) == fs::FsError::AccessDenied);
}

void test_traversal_cannot_escape_mount() {
  TempDirectory temp;
  fs::VirtualFileSystem vfs;
  mount_host(vfs, temp.path());

  fs::ResolvedPath resolved;
  assert(vfs.resolve("game:\\..\\outside.bin", resolved) ==
         fs::FsError::InvalidPath);

  std::unique_ptr<fs::FileHandle> file;
  fs::OpenOptions options{};
  options.access = fs::FileAccess::Write;
  options.disposition = fs::CreateDisposition::OverwriteIf;
  assert(vfs.open("game:\\folder\\..\\..\\outside.bin", options, file) ==
         fs::FsError::InvalidPath);
}

}  // namespace

int main() {
  test_path_normalization();
  test_mount_links_and_relative_paths();
  test_file_lifecycle();
  test_dispositions();
  test_read_only_mount();
  test_longest_mount_wins();
  test_symbolic_link_loop_guard();
  test_host_symlink_escape_is_rejected_when_supported();
  test_traversal_cannot_escape_mount();
  return 0;
}

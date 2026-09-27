#include <algorithm>
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

#include "xenon/filesystem/content_materializer.hpp"
#include "xenon/filesystem/content_probe.hpp"
#include "xenon/filesystem/gdfx_image_source.hpp"
#include "xenon/filesystem/host_path_device.hpp"
#include "xenon/filesystem/null_device.hpp"
#include "xenon/filesystem/path.hpp"
#include "xenon/filesystem/read_only_content_device.hpp"
#include "xenon/filesystem/stfs_package.hpp"
#include "xenon/filesystem/stfs_package_source.hpp"
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


void write_be32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  bytes[offset + 0] = static_cast<std::byte>((value >> 24u) & 0xFFu);
  bytes[offset + 1] = static_cast<std::byte>((value >> 16u) & 0xFFu);
  bytes[offset + 2] = static_cast<std::byte>((value >> 8u) & 0xFFu);
  bytes[offset + 3] = static_cast<std::byte>(value & 0xFFu);
}


void write_be16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset + 0] = static_cast<std::byte>((value >> 8u) & 0xFFu);
  bytes[offset + 1] = static_cast<std::byte>(value & 0xFFu);
}

void write_be64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value) {
  write_be32(bytes, offset, static_cast<std::uint32_t>(value >> 32u));
  write_be32(bytes, offset + 4, static_cast<std::uint32_t>(value & 0xFFFFFFFFu));
}

void write_le24(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  bytes[offset + 0] = static_cast<std::byte>(value & 0xFFu);
  bytes[offset + 1] = static_cast<std::byte>((value >> 8u) & 0xFFu);
  bytes[offset + 2] = static_cast<std::byte>((value >> 16u) & 0xFFu);
}

void write_utf16be(std::vector<std::byte>& bytes, std::size_t offset,
                   std::string_view text, std::size_t capacity_bytes) {
  assert(text.size() * 2 + 2 <= capacity_bytes);
  for (std::size_t i = 0; i < text.size(); ++i) {
    const auto c = static_cast<unsigned char>(text[i]);
    bytes[offset + i * 2] = std::byte{0};
    bytes[offset + i * 2 + 1] = static_cast<std::byte>(c);
  }
}

std::vector<std::byte> make_test_xex(std::uint32_t title_id,
                                     std::uint32_t media_id) {
  constexpr std::size_t kHeaderSize = 0x80;
  constexpr std::size_t kExecutionOffset = 0x40;
  constexpr std::size_t kPeNameOffset = 0x60;
  std::vector<std::byte> bytes(kHeaderSize);
  bytes[0] = std::byte{'X'};
  bytes[1] = std::byte{'E'};
  bytes[2] = std::byte{'X'};
  bytes[3] = std::byte{'2'};
  write_be32(bytes, 0x04, 0x00000001u);
  write_be32(bytes, 0x08, static_cast<std::uint32_t>(kHeaderSize));
  write_be32(bytes, 0x10, 0);
  write_be32(bytes, 0x14, 2);
  write_be32(bytes, 0x18, 0x00040006u);
  write_be32(bytes, 0x1C, static_cast<std::uint32_t>(kExecutionOffset));
  write_be32(bytes, 0x20, 0x000183FFu);
  write_be32(bytes, 0x24, static_cast<std::uint32_t>(kPeNameOffset));

  write_be32(bytes, kExecutionOffset + 0x00, media_id);
  // 2.1.12345.7 in XEX packed version form.
  const std::uint32_t version = 2u | (1u << 4u) | (12345u << 8u) | (7u << 24u);
  write_be32(bytes, kExecutionOffset + 0x04, version);
  write_be32(bytes, kExecutionOffset + 0x08, version);
  write_be32(bytes, kExecutionOffset + 0x0C, title_id);
  bytes[kExecutionOffset + 0x10] = std::byte{2};
  bytes[kExecutionOffset + 0x11] = std::byte{0};
  bytes[kExecutionOffset + 0x12] = std::byte{1};
  bytes[kExecutionOffset + 0x13] = std::byte{2};
  write_be32(bytes, kExecutionOffset + 0x14, title_id);

  const std::string pe_name = "default.exe";
  write_be32(bytes, kPeNameOffset,
             static_cast<std::uint32_t>(4 + pe_name.size() + 1));
  for (std::size_t i = 0; i < pe_name.size(); ++i) {
    bytes[kPeNameOffset + 4 + i] =
        static_cast<std::byte>(static_cast<unsigned char>(pe_name[i]));
  }
  return bytes;
}


void write_le16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset + 0] = static_cast<std::byte>(value & 0xFFu);
  bytes[offset + 1] = static_cast<std::byte>((value >> 8u) & 0xFFu);
}

void write_le32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  bytes[offset + 0] = static_cast<std::byte>(value & 0xFFu);
  bytes[offset + 1] = static_cast<std::byte>((value >> 8u) & 0xFFu);
  bytes[offset + 2] = static_cast<std::byte>((value >> 16u) & 0xFFu);
  bytes[offset + 3] = static_cast<std::byte>((value >> 24u) & 0xFFu);
}

void write_gdfx_entry(std::vector<std::byte>& image, std::size_t offset,
                      std::uint16_t left, std::uint16_t right,
                      std::uint32_t sector, std::uint32_t length,
                      std::uint8_t attributes, std::string_view name) {
  write_le16(image, offset + 0, left);
  write_le16(image, offset + 2, right);
  write_le32(image, offset + 4, sector);
  write_le32(image, offset + 8, length);
  image[offset + 12] = static_cast<std::byte>(attributes);
  image[offset + 13] = static_cast<std::byte>(name.size());
  for (std::size_t i = 0; i < name.size(); ++i) {
    image[offset + 14 + i] =
        static_cast<std::byte>(static_cast<unsigned char>(name[i]));
  }
}

std::vector<std::byte> make_test_gdfx(std::uint32_t title_id,
                                      std::uint32_t media_id,
                                      std::uint64_t game_offset = 0) {
  constexpr std::size_t kSector = 2048;
  constexpr std::uint32_t kRootSector = 40;
  constexpr std::uint32_t kDataSector = 41;
  constexpr std::uint32_t kXexSector = 50;
  constexpr std::uint32_t kPlaneSector = 51;
  constexpr std::uint32_t kRootSize = 64;
  constexpr std::uint32_t kDataDirectorySize = 32;

  const auto image_size = static_cast<std::size_t>(game_offset) + 64 * kSector;
  std::vector<std::byte> image(image_size);
  const auto descriptor = static_cast<std::size_t>(game_offset) + 32 * kSector;
  constexpr std::string_view magic = "MICROSOFT*XBOX*MEDIA";
  for (std::size_t i = 0; i < magic.size(); ++i) {
    image[descriptor + i] =
        static_cast<std::byte>(static_cast<unsigned char>(magic[i]));
  }
  write_le32(image, descriptor + 20, kRootSector);
  write_le32(image, descriptor + 24, kRootSize);

  const auto root = static_cast<std::size_t>(game_offset) + kRootSector * kSector;
  // Entry ordinals are byte offsets divided by four. The second record begins
  // at byte 28, so the root's right child ordinal is 7.
  write_gdfx_entry(image, root, 0, 7, kXexSector, 0x80, 0x20, "default.xex");
  write_gdfx_entry(image, root + 28, 0, 0, kDataSector, kDataDirectorySize,
                   fs::FileAttributeDirectory, "Data");

  const auto data = static_cast<std::size_t>(game_offset) + kDataSector * kSector;
  write_gdfx_entry(image, data, 0, 0, kPlaneSector, 6, 0x20, "plane.bin");

  const auto xex = make_test_xex(title_id, media_id);
  const auto xex_offset =
      static_cast<std::size_t>(game_offset) + kXexSector * kSector;
  std::copy(xex.begin(), xex.end(), image.begin() + xex_offset);
  constexpr std::string_view plane = "CFA-44";
  const auto plane_offset =
      static_cast<std::size_t>(game_offset) + kPlaneSector * kSector;
  for (std::size_t i = 0; i < plane.size(); ++i) {
    image[plane_offset + i] =
        static_cast<std::byte>(static_cast<unsigned char>(plane[i]));
  }
  return image;
}

std::vector<std::byte> make_test_stfs(std::uint32_t title_id,
                                      std::uint32_t media_id) {
  constexpr std::size_t kHeaderSize = 0xA000;
  constexpr std::size_t kHashOffset = kHeaderSize;
  constexpr std::size_t kDataBlock0 = kHeaderSize + 0x1000;
  constexpr std::size_t kDataBlock1 = kHeaderSize + 0x2000;
  constexpr std::size_t kDataBlock2 = kHeaderSize + 0x3000;
  constexpr std::size_t kPackageSize = kHeaderSize + 0x4000;
  constexpr std::size_t kPayloadSize = 5000;
  constexpr std::uint32_t kEnd = 0x00FFFFFFu;

  std::vector<std::byte> bytes(kPackageSize);
  bytes[0] = std::byte{'L'};
  bytes[1] = std::byte{'I'};
  bytes[2] = std::byte{'V'};
  bytes[3] = std::byte{'E'};

  for (std::size_t i = 0; i < 20; ++i) {
    bytes[0x32C + i] = static_cast<std::byte>(0x10u + i);
  }
  write_be32(bytes, 0x340, static_cast<std::uint32_t>(kHeaderSize));
  write_be32(bytes, 0x344, 0x00000002u);  // Marketplace content.
  write_be32(bytes, 0x348, 2);
  write_be64(bytes, 0x34C, kPayloadSize);

  const std::uint32_t version = 2u | (1u << 4u) | (12345u << 8u) | (7u << 24u);
  write_be32(bytes, 0x354, media_id);
  write_be32(bytes, 0x358, version);
  write_be32(bytes, 0x35C, version);
  write_be32(bytes, 0x360, title_id);
  bytes[0x364] = std::byte{2};
  bytes[0x365] = std::byte{0};
  bytes[0x366] = std::byte{1};
  bytes[0x367] = std::byte{1};
  write_be32(bytes, 0x368, title_id);

  constexpr std::size_t kDescriptor = 0x379;
  bytes[kDescriptor + 0] = std::byte{0x24};
  bytes[kDescriptor + 1] = std::byte{1};
  bytes[kDescriptor + 2] = std::byte{1};  // Read-only layout: one hash table.
  write_le16(bytes, kDescriptor + 3, 1);
  write_le24(bytes, kDescriptor + 5, 0);  // File table starts at logical block 0.
  write_be32(bytes, kDescriptor + 0x1C, 3);
  write_be32(bytes, kDescriptor + 0x20, 0);
  write_be32(bytes, 0x39D, 0);
  write_be64(bytes, 0x3A1, kPayloadSize);
  write_be32(bytes, 0x3A9, 0);  // STFS, not SVOD.

  write_utf16be(bytes, 0x411, "Test DLC", 256);
  write_utf16be(bytes, 0xD11, "Synthetic STFS package", 256);
  write_utf16be(bytes, 0x1611, "Xenon Tests", 128);
  write_utf16be(bytes, 0x1691, "Test Game", 128);

  // Hash table entries: block 0 (file table) is one block; the file payload
  // spans blocks 1 -> 2. Only the low 24 bits are the next-block index.
  write_be32(bytes, kHashOffset + 0x14, kEnd);
  write_be32(bytes, kHashOffset + 0x18 + 0x14, 2);
  write_be32(bytes, kHashOffset + 2 * 0x18 + 0x14, kEnd);

  auto write_directory_entry = [&](std::size_t entry_offset, std::string_view name,
                                   bool directory, std::uint16_t parent,
                                   std::uint32_t start_block,
                                   std::uint32_t block_count,
                                   std::uint32_t length) {
    assert(name.size() <= 40);
    for (std::size_t i = 0; i < name.size(); ++i) {
      bytes[entry_offset + i] =
          static_cast<std::byte>(static_cast<unsigned char>(name[i]));
    }
    bytes[entry_offset + 0x28] = static_cast<std::byte>(
        static_cast<std::uint8_t>(name.size()) | (directory ? 0x80u : 0u));
    write_le24(bytes, entry_offset + 0x29, block_count);
    write_le24(bytes, entry_offset + 0x2C, block_count);
    write_le24(bytes, entry_offset + 0x2F, start_block);
    write_be16(bytes, entry_offset + 0x32, parent);
    write_be32(bytes, entry_offset + 0x34, length);
    // 18 Sep 2026, 12:34:56 in FAT date/time form.
    constexpr std::uint16_t date = static_cast<std::uint16_t>(((2026 - 1980) << 9) | (9 << 5) | 18);
    constexpr std::uint16_t time = static_cast<std::uint16_t>((12 << 11) | (34 << 5) | (56 / 2));
    write_be16(bytes, entry_offset + 0x38, date);
    write_be16(bytes, entry_offset + 0x3A, time);
    write_be16(bytes, entry_offset + 0x3C, date);
    write_be16(bytes, entry_offset + 0x3E, time);
  };

  write_directory_entry(kDataBlock0, "Content", true, 0xFFFFu, 0, 0, 0);
  write_directory_entry(kDataBlock0 + 0x40, "data.bin", false, 0, 1, 2,
                        static_cast<std::uint32_t>(kPayloadSize));

  for (std::size_t i = 0; i < kPayloadSize; ++i) {
    const auto value = static_cast<std::byte>((i * 37u + 11u) & 0xFFu);
    if (i < 0x1000) {
      bytes[kDataBlock1 + i] = value;
    } else {
      bytes[kDataBlock2 + (i - 0x1000)] = value;
    }
  }
  return bytes;
}

void write_bytes(const std::filesystem::path& path,
                 const std::vector<std::byte>& bytes) {
  std::ofstream file(path, std::ios::binary);
  assert(file);
  file.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  assert(file.good());
}

class TestContentSource final : public fs::ReadOnlyContentSource {
 public:
  [[nodiscard]] fs::FsError initialize() override {
    initialized_ = true;
    return fs::FsError::None;
  }

  [[nodiscard]] fs::FsError stat(std::string_view path,
                                 fs::FileInfo& out_info) const override {
    if (!initialized_) return fs::FsError::IoError;
    out_info = {};
    if (path.empty() || fs::guest_path_equal(path, "Data")) {
      out_info.is_directory = true;
      out_info.attributes = fs::FileAttributeDirectory;
      return fs::FsError::None;
    }
    const auto payload = payload_for(path);
    if (payload.empty() && !fs::guest_path_equal(path, "Data\\empty.bin")) {
      return fs::FsError::NotFound;
    }
    out_info.size = payload.size();
    out_info.allocation_size = payload.size();
    out_info.attributes = fs::FileAttributeNormal | fs::FileAttributeArchive;
    return fs::FsError::None;
  }

  [[nodiscard]] fs::FsError list(
      std::string_view path,
      std::vector<fs::DirectoryEntry>& out_entries) const override {
    if (!initialized_) return fs::FsError::IoError;
    out_entries.clear();
    if (path.empty()) {
      fs::DirectoryEntry data{};
      data.name = "Data";
      data.info.is_directory = true;
      data.info.attributes = fs::FileAttributeDirectory;
      out_entries.push_back(std::move(data));
      return fs::FsError::None;
    }
    if (!fs::guest_path_equal(path, "Data")) return fs::FsError::NotFound;
    for (const auto name : {"default.xex", "README", "plane.bin", "empty.bin"}) {
      fs::DirectoryEntry entry{};
      entry.name = name;
      const auto payload = payload_for(std::string("Data\\") + name);
      entry.info.size = payload.size();
      entry.info.allocation_size = payload.size();
      entry.info.attributes = fs::FileAttributeNormal | fs::FileAttributeArchive;
      out_entries.push_back(std::move(entry));
    }
    return fs::FsError::None;
  }

  [[nodiscard]] fs::FsError read_at(
      std::string_view path, std::uint64_t offset,
      std::span<std::byte> destination,
      std::size_t& bytes_read) const override {
    bytes_read = 0;
    if (!initialized_) return fs::FsError::IoError;
    const auto payload = payload_for(path);
    if (payload.empty() && !fs::guest_path_equal(path, "Data\\empty.bin")) {
      return fs::FsError::NotFound;
    }
    if (offset >= payload.size() || destination.empty()) return fs::FsError::None;
    const auto count = std::min<std::size_t>(
        destination.size(), payload.size() - static_cast<std::size_t>(offset));
    for (std::size_t i = 0; i < count; ++i) {
      destination[i] = static_cast<std::byte>(static_cast<unsigned char>(
          payload[static_cast<std::size_t>(offset) + i]));
    }
    bytes_read = count;
    return fs::FsError::None;
  }

  [[nodiscard]] fs::FsError disk_space(fs::DiskSpace& out_space) const override {
    if (!initialized_) return fs::FsError::IoError;
    out_space.capacity = 4'700'000'000ull;
    out_space.free = 123;
    out_space.available = 123;
    return fs::FsError::None;
  }

 private:
  [[nodiscard]] static std::string_view payload_for(std::string_view path) {
    if (fs::guest_path_equal(path, "Data\\default.xex")) return "XENON-XEX";
    if (fs::guest_path_equal(path, "Data\\README")) return "disc readme";
    if (fs::guest_path_equal(path, "Data\\plane.bin")) return "CFA-44";
    if (fs::guest_path_equal(path, "Data\\empty.bin")) return {};
    return {};
  }

  bool initialized_{};
};

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


void test_wildcard_directory_queries_and_metadata() {
  TempDirectory temp;
  std::filesystem::create_directories(temp.path() / "Data" / "SubDir");
  {
    std::ofstream(temp.path() / "Data" / "ALPHA.BIN", std::ios::binary) << "alpha";
    std::ofstream(temp.path() / "Data" / "beta.bin", std::ios::binary) << "beta";
    std::ofstream(temp.path() / "Data" / "notes.txt", std::ios::binary) << "notes";
  }

  fs::VirtualFileSystem vfs;
  mount_host(vfs, temp.path());

  fs::DirectoryQuery query{};
  query.pattern = "*.bin";
  query.include_directories = false;
  std::vector<fs::DirectoryEntry> entries;
  assert(vfs.query_directory("game:\\Data", query, entries) == fs::FsError::None);
  assert(entries.size() == 2);
  assert(fs::guest_path_equal(entries[0].name, "ALPHA.BIN"));
  assert(fs::guest_path_equal(entries[1].name, "beta.bin"));

  query.pattern = "?eta.*";
  query.max_entries = 1;
  assert(vfs.query_directory("game:\\Data", query, entries) == fs::FsError::None);
  assert(entries.size() == 1);
  assert(fs::guest_path_equal(entries.front().name, "beta.bin"));

  query.pattern = "*";
  query.max_entries = 0;
  query.include_files = false;
  query.include_directories = true;
  assert(vfs.query_directory("game:\\Data", query, entries) == fs::FsError::None);
  assert(entries.size() == 1);
  assert(entries.front().info.is_directory);
  assert((entries.front().info.attributes & fs::FileAttributeDirectory) != 0);

  fs::FileInfo info{};
  assert(vfs.stat("game:\\Data\\ALPHA.BIN", info) == fs::FsError::None);
  assert(info.size == 5);
  assert(info.allocation_size >= info.size);
  assert((info.attributes & fs::FileAttributeNormal) != 0);
}

void test_open_actions_and_share_modes() {
  TempDirectory temp;
  fs::VirtualFileSystem vfs;
  mount_host(vfs, temp.path());

  fs::OpenOptions options{};
  options.access = fs::FileAccess::Read | fs::FileAccess::Write;
  options.disposition = fs::CreateDisposition::OpenIf;
  fs::OpenAction action = fs::OpenAction::None;
  std::unique_ptr<fs::FileHandle> first;
  assert(vfs.open("game:\\shared.bin", options, first, &action) == fs::FsError::None);
  assert(action == fs::OpenAction::Created);
  first.reset();

  options.access = fs::FileAccess::Read;
  options.share = fs::ShareAccess::Read;
  assert(vfs.open("game:\\shared.bin", options, first, &action) == fs::FsError::None);
  assert(action == fs::OpenAction::Opened);

  fs::OpenOptions writer{};
  writer.access = fs::FileAccess::Write;
  writer.share = fs::ShareAccess::All;
  writer.disposition = fs::CreateDisposition::Open;
  std::unique_ptr<fs::FileHandle> second;
  assert(vfs.open("game:\\SHARED.bin", writer, second) ==
         fs::FsError::SharingViolation);
  assert(vfs.remove("game:\\shared.bin") == fs::FsError::SharingViolation);
  first.reset();

  assert(vfs.open("game:\\shared.bin", writer, second, &action) == fs::FsError::None);
  assert(action == fs::OpenAction::Opened);
  second.reset();

  writer.disposition = fs::CreateDisposition::Overwrite;
  assert(vfs.open("game:\\shared.bin", writer, second, &action) == fs::FsError::None);
  assert(action == fs::OpenAction::Overwritten);
  second.reset();

  writer.disposition = fs::CreateDisposition::Supersede;
  assert(vfs.open("game:\\shared.bin", writer, second, &action) == fs::FsError::None);
  assert(action == fs::OpenAction::Superseded);
  second.reset();
}

void test_mount_link_introspection_and_disk_space() {
  TempDirectory temp;
  fs::VirtualFileSystem vfs;
  auto device = mount_host(vfs, temp.path());

  const auto mounts = vfs.mounts();
  assert(mounts.size() == 1);
  assert(fs::guest_path_equal(mounts.front().mount_point, device->mount_point()));
  assert(!mounts.front().read_only);

  const auto links = vfs.symbolic_links();
  assert(links.size() == 2);

  fs::DiskSpace space{};
  assert(vfs.disk_space("game:", space) == fs::FsError::None);
  assert(space.capacity > 0);
  assert(space.capacity >= space.available);
}

void test_null_device() {
  fs::VirtualFileSystem vfs;
  auto null_device = std::make_shared<fs::NullDevice>("\\Device\\NullStorage");
  assert(vfs.register_device(null_device) == fs::FsError::None);
  assert(vfs.register_symbolic_link("cache:", null_device->mount_point()) ==
         fs::FsError::None);

  fs::FileInfo root_info{};
  assert(vfs.stat("cache:", root_info) == fs::FsError::None);
  assert(root_info.is_directory);
  assert(root_info.read_only);

  std::vector<fs::DirectoryEntry> entries;
  assert(vfs.list("cache:", entries) == fs::FsError::None);
  assert(entries.empty());

  fs::FileInfo missing{};
  assert(vfs.stat("cache:\\probe.dat", missing) == fs::FsError::NotFound);
  assert(vfs.create_directory("cache:\\foo") == fs::FsError::ReadOnly);
}

void test_directory_cursor_resume_and_restart() {
  TempDirectory temp;
  std::filesystem::create_directories(temp.path() / "Cursor");
  for (const auto name : {"a.bin", "b.bin", "c.bin", "d.bin", "skip.txt"}) {
    std::ofstream(temp.path() / "Cursor" / name, std::ios::binary) << name;
  }

  fs::VirtualFileSystem vfs;
  mount_host(vfs, temp.path());
  fs::DirectoryQuery query{};
  query.pattern = "*.bin";
  query.include_directories = false;
  query.max_entries = 1;  // One-shot limit must not truncate cursor contents.

  std::unique_ptr<fs::DirectoryCursor> cursor;
  assert(vfs.open_directory_cursor("game:\\Cursor", query, cursor) ==
         fs::FsError::None);
  assert(cursor);
  assert(cursor->size() == 4);

  std::vector<fs::DirectoryEntry> batch;
  bool at_end = false;
  assert(cursor->read(2, batch, at_end) == fs::FsError::None);
  assert(batch.size() == 2);
  assert(!at_end);
  assert(cursor->position() == 2);

  assert(cursor->read(2, batch, at_end) == fs::FsError::None);
  assert(batch.size() == 2);
  assert(at_end);
  assert(cursor->exhausted());

  assert(cursor->read(2, batch, at_end) == fs::FsError::None);
  assert(batch.empty());
  assert(at_end);

  cursor->restart();
  assert(cursor->position() == 0);
  assert(cursor->read(1, batch, at_end) == fs::FsError::None);
  assert(batch.size() == 1);
  assert(!at_end);
}

void test_dos_wildcard_semantics() {
  assert(fs::guest_wildcard_match("*.*", "README"));
  assert(fs::guest_wildcard_match("README.*", "README"));
  assert(fs::guest_wildcard_match("file>.bin", "file1.bin"));
  assert(fs::guest_wildcard_match("file>.bin", "file.bin"));
  assert(fs::guest_wildcard_match("plane<.bin", "plane_variant.bin"));
  assert(fs::guest_wildcard_match("README\"", "README"));
  assert(fs::guest_wildcard_match("README\"", "README."));
  assert(!fs::guest_wildcard_match("*.bin", "notes.txt"));
}

void test_host_metadata_updates() {
  TempDirectory temp;
  {
    std::ofstream(temp.path() / "metadata.bin", std::ios::binary) << "metadata";
  }
  fs::VirtualFileSystem vfs;
  mount_host(vfs, temp.path());

  fs::FileAttributeUpdate update{};
  update.mask = fs::FileAttributeReadOnly;
  update.value = fs::FileAttributeReadOnly;
  assert(vfs.set_attributes("game:\\metadata.bin", update) == fs::FsError::None);

  fs::FileInfo info{};
  assert(vfs.stat("game:\\metadata.bin", info) == fs::FsError::None);
  assert(info.read_only);
  assert((info.attributes & fs::FileAttributeReadOnly) != 0);

  update.value = fs::FileAttributeNone;
  assert(vfs.set_attributes("game:\\metadata.bin", update) == fs::FsError::None);
  assert(vfs.stat("game:\\metadata.bin", info) == fs::FsError::None);
  assert(!info.read_only);

  fs::FileAttributeUpdate unsupported{};
  unsupported.mask = fs::FileAttributeHidden;
  unsupported.value = fs::FileAttributeHidden;
  assert(vfs.set_attributes("game:\\metadata.bin", unsupported) ==
         fs::FsError::Unsupported);

  const auto target = std::filesystem::file_time_type::clock::now() -
                      std::chrono::hours(24);
  assert(vfs.set_last_write_time("game:\\metadata.bin", target) ==
         fs::FsError::None);
  assert(vfs.stat("game:\\metadata.bin", info) == fs::FsError::None);
  auto delta = info.last_write_time - target;
  if (delta < decltype(delta)::zero()) delta = -delta;
  assert(delta < std::chrono::seconds(2));
}

void test_read_only_content_device_boundary() {
  fs::VirtualFileSystem vfs;
  auto source = std::make_shared<TestContentSource>();
  auto device = std::make_shared<fs::ReadOnlyContentDevice>(
      "\\Device\\CdRom0", source);
  assert(vfs.register_device(device) == fs::FsError::None);
  assert(vfs.register_symbolic_link("d:", device->mount_point()) ==
         fs::FsError::None);
  assert(vfs.set_working_directory("d:") == fs::FsError::None);

  fs::FileInfo info{};
  assert(vfs.stat("d:\\Data\\DEFAULT.XEX", info) == fs::FsError::None);
  assert(info.size == 9);
  assert(info.read_only);
  assert((info.attributes & fs::FileAttributeReadOnly) != 0);

  std::unique_ptr<fs::FileHandle> file;
  fs::OpenOptions read{};
  read.access = fs::FileAccess::Read;
  assert(vfs.open("Data\\default.xex", read, file) == fs::FsError::None);
  std::array<std::byte, 16> data{};
  std::size_t bytes_read = 0;
  assert(file->read(data, bytes_read) == fs::FsError::None);
  assert(bytes_read == 9);
  assert(data[0] == std::byte{'X'});
  assert(data[8] == std::byte{'X'});

  fs::OpenOptions write{};
  write.access = fs::FileAccess::Write;
  assert(vfs.open("d:\\Data\\default.xex", write, file) == fs::FsError::ReadOnly);
  assert(vfs.remove("d:\\Data\\default.xex") == fs::FsError::ReadOnly);

  std::vector<fs::DirectoryEntry> entries;
  fs::DirectoryQuery query{};
  query.pattern = "*.*";
  query.include_directories = false;
  assert(vfs.query_directory("d:\\Data", query, entries) == fs::FsError::None);
  assert(entries.size() == 4);  // README is included by DOS-style *.* semantics.

  fs::DiskSpace space{};
  assert(vfs.disk_space("d:", space) == fs::FsError::None);
  assert(space.capacity == 4'700'000'000ull);
  assert(space.free == 0);
  assert(space.available == 0);
}

void test_gdfx_image_source_and_vfs() {
  constexpr std::uint32_t kTitleId = 0x4E4D07D1u;
  constexpr std::uint32_t kMediaId = 0xAABBCCDDu;
  TempDirectory temp;
  const auto image_path = temp.path() / "game.iso";
  write_bytes(image_path, make_test_gdfx(kTitleId, kMediaId, 0x0000FB20ull));

  auto source = std::make_shared<fs::GdfxImageSource>(image_path);
  assert(source->initialize() == fs::FsError::None);
  assert(source->info().game_partition_offset == 0x0000FB20ull);
  assert(source->info().root_sector == 40);

  fs::FileInfo info{};
  assert(source->stat("DEFAULT.XEX", info) == fs::FsError::None);
  assert(info.size == 0x80);
  assert(info.read_only);
  assert(!info.is_directory);

  std::vector<fs::DirectoryEntry> root_entries;
  assert(source->list("", root_entries) == fs::FsError::None);
  assert(root_entries.size() == 2);
  assert(fs::guest_path_equal(root_entries[0].name, "default.xex"));
  assert(fs::guest_path_equal(root_entries[1].name, "Data"));

  std::array<std::byte, 6> plane{};
  std::size_t bytes_read = 0;
  assert(source->read_at("data\\PLANE.BIN", 0, plane, bytes_read) ==
         fs::FsError::None);
  assert(bytes_read == plane.size());
  assert(plane[0] == std::byte{'C'});
  assert(plane[5] == std::byte{'4'});

  fs::VirtualFileSystem vfs;
  auto device = std::make_shared<fs::ReadOnlyContentDevice>(
      "\\Device\\CdRom0", source);
  assert(vfs.register_device(device) == fs::FsError::None);
  assert(vfs.register_symbolic_link("game:", device->mount_point()) ==
         fs::FsError::None);
  assert(vfs.register_symbolic_link("d:", device->mount_point()) ==
         fs::FsError::None);

  std::unique_ptr<fs::FileHandle> file;
  fs::OpenOptions open{};
  assert(vfs.open("d:\\default.xex", open, file) == fs::FsError::None);
  std::array<std::byte, 4> magic{};
  assert(file->read(magic, bytes_read) == fs::FsError::None);
  assert(bytes_read == 4);
  assert(magic[0] == std::byte{'X'} && magic[3] == std::byte{'2'});

  fs::DiskSpace space{};
  assert(vfs.disk_space("d:", space) == fs::FsError::None);
  assert(space.capacity == source->info().image_size);
  assert(space.free == 0);

  auto corrupt = make_test_gdfx(kTitleId, kMediaId);
  constexpr std::size_t kRootOffset = 40 * 2048;
  // The second root record is ordinal 7. Point its left child back to itself
  // and ensure the parser rejects the directory-tree cycle.
  write_le16(corrupt, kRootOffset + 28, 7);
  const auto corrupt_path = temp.path() / "corrupt.iso";
  write_bytes(corrupt_path, corrupt);
  fs::GdfxImageSource corrupt_source(corrupt_path);
  assert(corrupt_source.initialize() == fs::FsError::InvalidArgument);
}

void test_stfs_package_source_and_materializer() {
  constexpr std::uint32_t kTitleId = 0x4E4D07D1u;
  constexpr std::uint32_t kMediaId = 0x11223344u;
  TempDirectory temp;
  const auto package_path = temp.path() / "addon.live";
  auto package = make_test_stfs(kTitleId, kMediaId);
  write_bytes(package_path, package);

  fs::StfsPackageMetadata metadata{};
  assert(fs::read_stfs_metadata(package_path, metadata) == fs::FsError::None);
  assert(metadata.package_type == fs::StfsPackageType::Live);
  assert(metadata.content_type == fs::XboxContentType::MarketplaceContent);
  assert(metadata.execution_info.title_id == kTitleId);
  assert(metadata.execution_info.media_id == kMediaId);
  assert(metadata.display_name == "Test DLC");
  assert(metadata.description == "Synthetic STFS package");
  assert(metadata.publisher == "Xenon Tests");
  assert(metadata.title_name == "Test Game");
  assert(metadata.content_id_hex ==
         "101112131415161718191A1B1C1D1E1F20212223");

  auto source = std::make_shared<fs::StfsPackageSource>(package_path);
  assert(source->initialize() == fs::FsError::None);

  std::vector<fs::DirectoryEntry> root;
  assert(source->list("", root) == fs::FsError::None);
  assert(root.size() == 1);
  assert(root[0].name == "Content");
  assert(root[0].info.is_directory);

  fs::FileInfo payload_info{};
  assert(source->stat("content\\DATA.BIN", payload_info) == fs::FsError::None);
  assert(!payload_info.is_directory);
  assert(payload_info.size == 5000);

  std::array<std::byte, 32> across_boundary{};
  std::size_t bytes_read = 0;
  assert(source->read_at("Content\\data.bin", 4088, across_boundary, bytes_read) ==
         fs::FsError::None);
  assert(bytes_read == across_boundary.size());
  for (std::size_t i = 0; i < across_boundary.size(); ++i) {
    const auto logical = 4088 + i;
    const auto expected = static_cast<std::byte>((logical * 37u + 11u) & 0xFFu);
    assert(across_boundary[i] == expected);
  }

  fs::VirtualFileSystem vfs;
  auto device = std::make_shared<fs::ReadOnlyContentDevice>(
      "\\\\Device\\\\Content0", source);
  assert(vfs.register_device(device) == fs::FsError::None);
  assert(vfs.register_symbolic_link("dlc:", device->mount_point()) ==
         fs::FsError::None);
  std::unique_ptr<fs::FileHandle> file;
  fs::OpenOptions open{};
  assert(vfs.open("dlc:\\Content\\data.bin", open, file) == fs::FsError::None);
  std::array<std::byte, 8> prefix{};
  assert(file->read(prefix, bytes_read) == fs::FsError::None);
  assert(bytes_read == prefix.size());

  const auto extracted = temp.path() / "materialized";
  assert(fs::materialize_content_source(*source, extracted) == fs::FsError::None);
  std::ifstream extracted_file(extracted / "Content" / "data.bin", std::ios::binary);
  assert(extracted_file);
  std::vector<unsigned char> materialized(5000);
  extracted_file.read(reinterpret_cast<char*>(materialized.data()),
                      static_cast<std::streamsize>(materialized.size()));
  assert(extracted_file.gcount() == static_cast<std::streamsize>(materialized.size()));
  for (std::size_t i = 0; i < materialized.size(); ++i) {
    assert(materialized[i] == static_cast<unsigned char>((i * 37u + 11u) & 0xFFu));
  }

  // Corrupt block 1's chain so it points to itself. The source must reject
  // the cycle while resolving the 5,000-byte payload.
  write_be32(package, 0xA000 + 0x18 + 0x14, 1);
  const auto corrupt_path = temp.path() / "corrupt.live";
  write_bytes(corrupt_path, package);
  fs::StfsPackageSource corrupt_source(corrupt_path);
  assert(corrupt_source.initialize() == fs::FsError::InvalidArgument);
}

void test_content_probe_and_xex_metadata() {
  constexpr std::uint32_t kTitleId = 0x4E4D07D1u;
  constexpr std::uint32_t kMediaId = 0x11223344u;
  TempDirectory temp;
  write_bytes(temp.path() / "DeFaUlT.XeX", make_test_xex(kTitleId, kMediaId));

  fs::ContentProbe probe;
  const auto result = probe.probe(temp.path());
  assert(result.identified());
  assert(result.source_type == fs::ContentSourceType::Directory);
  assert(result.executable_path.filename() == "DeFaUlT.XeX");
  assert(result.xex.execution_info.has_value());
  const auto& execution = *result.xex.execution_info;
  assert(execution.title_id == kTitleId);
  assert(execution.media_id == kMediaId);
  assert(execution.version.major == 2);
  assert(execution.version.minor == 1);
  assert(execution.version.build == 12345);
  assert(execution.version.qfe == 7);
  assert(execution.disc_number == 1);
  assert(execution.disc_count == 2);
  assert(result.xex.original_pe_name == "default.exe");
  assert(fs::format_xbox_id(kTitleId) == "4E4D07D1");

  const auto direct = probe.probe(temp.path() / "DeFaUlT.XeX");
  assert(direct.identified());
  assert(direct.source_type == fs::ContentSourceType::Xex);

  {
    std::ofstream package(temp.path() / "truncated.stfs", std::ios::binary);
    package.write("LIVE", 4);
  }
  const auto truncated_stfs = probe.probe(temp.path() / "truncated.stfs");
  assert(truncated_stfs.status == fs::ContentProbeStatus::Invalid);
  assert(truncated_stfs.source_type == fs::ContentSourceType::StfsPackageCandidate);

  write_bytes(temp.path() / "addon.stfs", make_test_stfs(kTitleId, kMediaId));
  const auto stfs = probe.probe(temp.path() / "addon.stfs");
  assert(stfs.identified());
  assert(!stfs.has_game_identity());
  assert(stfs.source_type == fs::ContentSourceType::StfsPackage);
  assert(stfs.stfs.has_value());
  assert(stfs.stfs->execution_info.title_id == kTitleId);
  assert(stfs.stfs->execution_info.media_id == kMediaId);
  assert(stfs.stfs->display_name == "Test DLC");
  assert(stfs.stfs->publisher == "Xenon Tests");

  write_bytes(temp.path() / "disc.iso", make_test_gdfx(kTitleId, kMediaId));
  const auto image = probe.probe(temp.path() / "disc.iso");
  assert(image.identified());
  assert(image.source_type == fs::ContentSourceType::GdfxImage);
  assert(image.executable_path.empty());
  assert(fs::guest_path_equal(image.executable_guest_path, "d:\\default.xex"));
  assert(image.xex.execution_info->title_id == kTitleId);

  {
    std::ofstream descriptor(temp.path() / "disc.dvd");
    descriptor << "LayerBreak=1913760\n";
    descriptor << "disc.iso\n";
  }
  const auto descriptor = probe.probe(temp.path() / "disc.dvd");
  assert(descriptor.identified());
  assert(descriptor.source_type == fs::ContentSourceType::GdfxImage);
  assert(descriptor.resolved_source_path.filename() == "disc.iso");

  {
    std::ofstream bad_image(temp.path() / "bad.iso", std::ios::binary);
    bad_image.write("notgdfx", 7);
  }
  const auto bad_image = probe.probe(temp.path() / "bad.iso");
  assert(bad_image.status == fs::ContentProbeStatus::Invalid);
  assert(bad_image.source_type == fs::ContentSourceType::GdfxImageCandidate);

  TempDirectory invalid_dir;
  const auto invalid = probe.probe(invalid_dir.path());
  assert(invalid.status == fs::ContentProbeStatus::Invalid);
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
  test_wildcard_directory_queries_and_metadata();
  test_open_actions_and_share_modes();
  test_mount_link_introspection_and_disk_space();
  test_null_device();
  test_directory_cursor_resume_and_restart();
  test_dos_wildcard_semantics();
  test_host_metadata_updates();
  test_read_only_content_device_boundary();
  test_gdfx_image_source_and_vfs();
  test_stfs_package_source_and_materializer();
  test_content_probe_and_xex_metadata();
  return 0;
}

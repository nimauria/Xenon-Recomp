#include "content_source.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

class TempDirectory {
 public:
  TempDirectory() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("xenon_runtime_content_" + std::to_string(stamp));
    std::filesystem::create_directories(path_);
  }
  ~TempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  const std::filesystem::path& path() const noexcept { return path_; }
 private:
  std::filesystem::path path_{};
};

void write_bytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  assert(file.good());
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
                      std::uint32_t sector, std::uint32_t length,
                      std::string_view name) {
  write_le16(image, offset + 0, 0);
  write_le16(image, offset + 2, 0);
  write_le32(image, offset + 4, sector);
  write_le32(image, offset + 8, length);
  image[offset + 12] = std::byte{0x20};
  image[offset + 13] = static_cast<std::byte>(name.size());
  for (std::size_t i = 0; i < name.size(); ++i) {
    image[offset + 14 + i] = static_cast<std::byte>(static_cast<unsigned char>(name[i]));
  }
}

std::vector<std::byte> make_disc_with_default_xex() {
  constexpr std::size_t kSectorSize = 2048;
  constexpr std::uint32_t kRootSector = 40;
  constexpr std::uint32_t kXexSector = 50;
  constexpr std::uint32_t kXexSize = 0x80;
  std::vector<std::byte> image(64 * kSectorSize);

  const auto descriptor = 32 * kSectorSize;
  constexpr std::string_view magic = "MICROSOFT*XBOX*MEDIA";
  for (std::size_t i = 0; i < magic.size(); ++i) {
    image[descriptor + i] = static_cast<std::byte>(static_cast<unsigned char>(magic[i]));
  }
  write_le32(image, descriptor + 20, kRootSector);
  write_le32(image, descriptor + 24, 32);

  const auto root = kRootSector * kSectorSize;
  write_gdfx_entry(image, root, kXexSector, kXexSize, "default.xex");

  const auto xex = kXexSector * kSectorSize;
  image[xex + 0] = std::byte{'X'};
  image[xex + 1] = std::byte{'E'};
  image[xex + 2] = std::byte{'X'};
  image[xex + 3] = std::byte{'2'};
  return image;
}

}  // namespace

int main() {
  using xenon::runtime_host::BaseContent;
  using xenon::runtime_host::load_base_content;

  TempDirectory temp;

  // Extracted directory input remains supported and is mounted as-is.
  const auto extracted = temp.path() / "extracted";
  std::filesystem::create_directories(extracted);
  write_bytes(extracted / "DEFAULT.XEX", {std::byte{'X'}, std::byte{'E'}, std::byte{'X'}, std::byte{'2'}});
  BaseContent content{};
  std::string error;
  assert(load_base_content(extracted, content, error));
  assert(content.mount_path == extracted);
  assert(content.xex_bytes.size() == 4);

  // A loose XEX uses its parent directory as game:/ so sibling assets remain visible.
  const auto loose = temp.path() / "loose_game.xex";
  write_bytes(loose, {std::byte{'X'}, std::byte{'E'}, std::byte{'X'}, std::byte{'2'}});
  assert(load_base_content(loose, content, error));
  assert(content.mount_path == temp.path());
  assert(content.xex_bytes.size() == 4);

  // A disc image is read directly; no extraction directory is created.
  const auto image = temp.path() / "disc.iso";
  write_bytes(image, make_disc_with_default_xex());
  assert(load_base_content(image, content, error));
  assert(content.mount_path == image);
  assert(content.xex_bytes.size() == 0x80);
  assert(content.xex_bytes[0] == std::byte{'X'});

  // .dvd descriptors resolve to and mount the exact referenced image.
  const auto descriptor = temp.path() / "disc.dvd";
  {
    std::ofstream out(descriptor);
    out << "LayerBreak=1913760\n";
    out << "disc.iso\n";
  }
  assert(load_base_content(descriptor, content, error));
  assert(content.mount_path == image);
  assert(content.xex_bytes.size() == 0x80);

  // Unsupported shapes fail explicitly rather than being interpreted as directories.
  const auto text = temp.path() / "game.txt";
  std::ofstream(text) << "not content";
  assert(!load_base_content(text, content, error));
  assert(!error.empty());

  std::cout << "All runtime content source tests passed!\n";
  return 0;
}

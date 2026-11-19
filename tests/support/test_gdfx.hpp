#pragma once

// A metadata-only XEX header and a small GDFX (XDVDFS) disc image holding it:
// default.xex at the root and Data/plane.bin ("CFA-44"). Shared by the
// filesystem tests and the input fuzz tests.

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xenon/filesystem/types.hpp"

namespace xenon::test::gdfx {

inline void write_be32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  bytes[offset + 0] = static_cast<std::byte>((value >> 24u) & 0xFFu);
  bytes[offset + 1] = static_cast<std::byte>((value >> 16u) & 0xFFu);
  bytes[offset + 2] = static_cast<std::byte>((value >> 8u) & 0xFFu);
  bytes[offset + 3] = static_cast<std::byte>(value & 0xFFu);
}


inline void write_be16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset + 0] = static_cast<std::byte>((value >> 8u) & 0xFFu);
  bytes[offset + 1] = static_cast<std::byte>(value & 0xFFu);
}

inline void write_be64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value) {
  write_be32(bytes, offset, static_cast<std::uint32_t>(value >> 32u));
  write_be32(bytes, offset + 4, static_cast<std::uint32_t>(value & 0xFFFFFFFFu));
}

inline void write_le24(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  bytes[offset + 0] = static_cast<std::byte>(value & 0xFFu);
  bytes[offset + 1] = static_cast<std::byte>((value >> 8u) & 0xFFu);
  bytes[offset + 2] = static_cast<std::byte>((value >> 16u) & 0xFFu);
}

inline void write_utf16be(std::vector<std::byte>& bytes, std::size_t offset,
                   std::string_view text, std::size_t capacity_bytes) {
  assert(text.size() * 2 + 2 <= capacity_bytes);
  for (std::size_t i = 0; i < text.size(); ++i) {
    const auto c = static_cast<unsigned char>(text[i]);
    bytes[offset + i * 2] = std::byte{0};
    bytes[offset + i * 2 + 1] = static_cast<std::byte>(c);
  }
}

inline std::vector<std::byte> make_test_xex(std::uint32_t title_id,
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


inline void write_le16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset + 0] = static_cast<std::byte>(value & 0xFFu);
  bytes[offset + 1] = static_cast<std::byte>((value >> 8u) & 0xFFu);
}

inline void write_le32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  bytes[offset + 0] = static_cast<std::byte>(value & 0xFFu);
  bytes[offset + 1] = static_cast<std::byte>((value >> 8u) & 0xFFu);
  bytes[offset + 2] = static_cast<std::byte>((value >> 16u) & 0xFFu);
  bytes[offset + 3] = static_cast<std::byte>((value >> 24u) & 0xFFu);
}

inline void write_gdfx_entry(std::vector<std::byte>& image, std::size_t offset,
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

inline std::vector<std::byte> make_test_gdfx(std::uint32_t title_id,
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
                   xenon::filesystem::FileAttributeDirectory, "Data");

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

}  // namespace xenon::test::gdfx

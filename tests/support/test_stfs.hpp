#pragma once

// A minimal, valid STFS (LIVE) marketplace package for tests: one hash table,
// a "Content" directory holding "data.bin" (5000 bytes, byte i is
// (i * 37 + 11) & 0xFF), display name "Test DLC", content ID 10 11 .. 23.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace xenon::test {
namespace stfs_detail {

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
inline void write_le16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset + 0] = static_cast<std::byte>(value & 0xFFu);
  bytes[offset + 1] = static_cast<std::byte>((value >> 8u) & 0xFFu);
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
    bytes[offset + i * 2] = std::byte{0};
    bytes[offset + i * 2 + 1] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
  }
}

}  // namespace stfs_detail

inline std::vector<std::byte> make_test_stfs(std::uint32_t title_id,
                                      std::uint32_t media_id) {
  constexpr std::size_t kHeaderSize = 0xA000;
  constexpr std::size_t kHashOffset = kHeaderSize;
  constexpr std::size_t kDataBlock0 = kHeaderSize + 0x1000;
  constexpr std::size_t kDataBlock1 = kHeaderSize + 0x2000;
  constexpr std::size_t kDataBlock2 = kHeaderSize + 0x3000;
  constexpr std::size_t kPackageSize = kHeaderSize + 0x4000;
  constexpr std::size_t kPayloadSize = 5000;
  constexpr std::uint32_t kEnd = 0x00FFFFFFu;

  using namespace stfs_detail;
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

}  // namespace xenon::test

#include "xenon/filesystem/xex_metadata.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

namespace xenon::filesystem {
namespace {
constexpr std::uint32_t kXex2Magic = 0x58455832u;  // "XEX2" in file order.
constexpr std::uint32_t kExecutionInfoKey = 0x00040006u;
constexpr std::uint32_t kOriginalPeNameKey = 0x000183FFu;
constexpr std::size_t kBaseHeaderSize = 0x18;
constexpr std::size_t kOptionalHeaderSize = 8;
constexpr std::size_t kExecutionInfoSize = 0x18;
constexpr std::size_t kMaxHeaderBytes = 16u * 1024u * 1024u;

std::uint32_t read_be32(std::span<const std::byte> bytes, std::size_t offset) {
  return (std::to_integer<std::uint32_t>(bytes[offset]) << 24u) |
         (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 16u) |
         (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 8u) |
         std::to_integer<std::uint32_t>(bytes[offset + 3]);
}

XexVersion decode_version(std::uint32_t value) {
  XexVersion result{};
  result.value = value;
  result.major = static_cast<std::uint8_t>(value & 0x0Fu);
  result.minor = static_cast<std::uint8_t>((value >> 4u) & 0x0Fu);
  result.build = static_cast<std::uint16_t>((value >> 8u) & 0xFFFFu);
  result.qfe = static_cast<std::uint8_t>((value >> 24u) & 0xFFu);
  return result;
}

bool range_valid(std::size_t offset, std::size_t length, std::size_t size) {
  return offset <= size && length <= size - offset;
}
}  // namespace

std::string XexVersion::to_string() const {
  return std::to_string(major) + "." + std::to_string(minor) + "." +
         std::to_string(build) + "." + std::to_string(qfe);
}

std::string format_xbox_id(std::uint32_t value) {
  std::ostringstream stream;
  stream << std::uppercase << std::hex << std::setfill('0') << std::setw(8)
         << value;
  return stream.str();
}

FsError parse_xex_metadata(std::span<const std::byte> bytes,
                           XexMetadata& out_metadata) {
  out_metadata = {};
  if (bytes.size() < kBaseHeaderSize) return FsError::InvalidArgument;
  if (read_be32(bytes, 0) != kXex2Magic) return FsError::InvalidArgument;

  const auto module_flags = read_be32(bytes, 4);
  const auto header_size_u32 = read_be32(bytes, 8);
  const auto security_offset_u32 = read_be32(bytes, 0x10);
  const auto header_count_u32 = read_be32(bytes, 0x14);
  const auto header_size = static_cast<std::size_t>(header_size_u32);
  const auto header_count = static_cast<std::size_t>(header_count_u32);

  if (header_size < kBaseHeaderSize || header_size > bytes.size() ||
      header_size > kMaxHeaderBytes) {
    return FsError::InvalidArgument;
  }
  if (header_count > (header_size - kBaseHeaderSize) / kOptionalHeaderSize) {
    return FsError::InvalidArgument;
  }
  if (security_offset_u32 >= header_size && security_offset_u32 != 0) {
    return FsError::InvalidArgument;
  }

  out_metadata.module_flags = module_flags;
  out_metadata.header_size = header_size_u32;
  out_metadata.security_offset = security_offset_u32;
  out_metadata.optional_header_count = header_count_u32;

  for (std::size_t index = 0; index < header_count; ++index) {
    const auto entry_offset = kBaseHeaderSize + index * kOptionalHeaderSize;
    const auto key = read_be32(bytes, entry_offset);
    const auto value = read_be32(bytes, entry_offset + 4);

    if (key == kExecutionInfoKey) {
      const auto offset = static_cast<std::size_t>(value);
      if (!range_valid(offset, kExecutionInfoSize, header_size)) {
        return FsError::InvalidArgument;
      }
      XexExecutionInfo info{};
      info.media_id = read_be32(bytes, offset + 0x00);
      info.version = decode_version(read_be32(bytes, offset + 0x04));
      info.base_version = decode_version(read_be32(bytes, offset + 0x08));
      info.title_id = read_be32(bytes, offset + 0x0C);
      info.platform = std::to_integer<std::uint8_t>(bytes[offset + 0x10]);
      info.executable_table =
          std::to_integer<std::uint8_t>(bytes[offset + 0x11]);
      info.disc_number = std::to_integer<std::uint8_t>(bytes[offset + 0x12]);
      info.disc_count = std::to_integer<std::uint8_t>(bytes[offset + 0x13]);
      info.savegame_id = read_be32(bytes, offset + 0x14);
      out_metadata.execution_info = info;
    } else if (key == kOriginalPeNameKey) {
      const auto offset = static_cast<std::size_t>(value);
      if (!range_valid(offset, 4, header_size)) return FsError::InvalidArgument;
      const auto record_size = static_cast<std::size_t>(read_be32(bytes, offset));
      if (record_size < 4 || !range_valid(offset, record_size, header_size)) {
        return FsError::InvalidArgument;
      }
      const auto name_bytes = bytes.subspan(offset + 4, record_size - 4);
      std::string name;
      name.reserve(name_bytes.size());
      for (const auto value_byte : name_bytes) {
        const auto character = static_cast<char>(std::to_integer<unsigned char>(value_byte));
        if (character == '\0') break;
        name.push_back(character);
      }
      out_metadata.original_pe_name = std::move(name);
    }
  }

  return FsError::None;
}

FsError read_xex_metadata(const std::filesystem::path& path,
                          XexMetadata& out_metadata) {
  out_metadata = {};
  std::error_code ec;
  const auto file_size_u64 = std::filesystem::file_size(path, ec);
  if (ec) return FsError::IoError;
  if (file_size_u64 < kBaseHeaderSize ||
      file_size_u64 > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return FsError::InvalidArgument;
  }

  std::ifstream file(path, std::ios::binary);
  if (!file) return FsError::IoError;

  std::array<std::byte, kBaseHeaderSize> base{};
  file.read(reinterpret_cast<char*>(base.data()),
            static_cast<std::streamsize>(base.size()));
  if (file.gcount() != static_cast<std::streamsize>(base.size())) {
    return FsError::IoError;
  }
  if (read_be32(base, 0) != kXex2Magic) return FsError::InvalidArgument;

  const auto header_size = static_cast<std::size_t>(read_be32(base, 8));
  if (header_size < kBaseHeaderSize || header_size > kMaxHeaderBytes ||
      header_size > file_size_u64) {
    return FsError::InvalidArgument;
  }

  std::vector<std::byte> header(header_size);
  std::copy(base.begin(), base.end(), header.begin());
  if (header_size > base.size()) {
    file.read(reinterpret_cast<char*>(header.data() + base.size()),
              static_cast<std::streamsize>(header_size - base.size()));
    if (file.gcount() !=
        static_cast<std::streamsize>(header_size - base.size())) {
      return FsError::IoError;
    }
  }
  return parse_xex_metadata(header, out_metadata);
}

}  // namespace xenon::filesystem

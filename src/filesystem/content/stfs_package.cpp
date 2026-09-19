#include "xenon/filesystem/stfs_package.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace xenon::filesystem {
namespace {
constexpr std::size_t kHeaderMetadataSize = 0x971A;
constexpr std::size_t kContentIdOffset = 0x32C;
constexpr std::size_t kHeaderSizeOffset = 0x340;
constexpr std::size_t kContentTypeOffset = 0x344;
constexpr std::size_t kMetadataVersionOffset = 0x348;
constexpr std::size_t kContentSizeOffset = 0x34C;
constexpr std::size_t kExecutionInfoOffset = 0x354;
constexpr std::size_t kVolumeDescriptorOffset = 0x379;
constexpr std::size_t kDataFileCountOffset = 0x39D;
constexpr std::size_t kDataFileSizeOffset = 0x3A1;
constexpr std::size_t kVolumeTypeOffset = 0x3A9;
constexpr std::size_t kDisplayNamesOffset = 0x411;
constexpr std::size_t kDescriptionsOffset = 0xD11;
constexpr std::size_t kPublisherOffset = 0x1611;
constexpr std::size_t kTitleNameOffset = 0x1691;
constexpr std::size_t kLanguageSlotBytes = 128 * 2;
constexpr std::size_t kPublisherBytes = 64 * 2;
constexpr std::size_t kTitleNameBytes = 64 * 2;
constexpr std::uint8_t kStfsDescriptorLength = 0x24;

std::uint16_t read_be16(std::span<const std::byte> bytes,
                        std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(
      (std::to_integer<std::uint16_t>(bytes[offset]) << 8u) |
      std::to_integer<std::uint16_t>(bytes[offset + 1]));
}

std::uint32_t read_be32(std::span<const std::byte> bytes,
                        std::size_t offset) noexcept {
  return (std::to_integer<std::uint32_t>(bytes[offset]) << 24u) |
         (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 16u) |
         (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 8u) |
         std::to_integer<std::uint32_t>(bytes[offset + 3]);
}

std::uint64_t read_be64(std::span<const std::byte> bytes,
                        std::size_t offset) noexcept {
  return (static_cast<std::uint64_t>(read_be32(bytes, offset)) << 32u) |
         read_be32(bytes, offset + 4);
}

std::uint32_t read_le24(std::span<const std::byte> bytes,
                        std::size_t offset) noexcept {
  return std::to_integer<std::uint32_t>(bytes[offset]) |
         (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8u) |
         (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16u);
}

XexVersion decode_version(std::uint32_t value) noexcept {
  XexVersion result{};
  result.value = value;
  result.major = static_cast<std::uint8_t>(value & 0x0Fu);
  result.minor = static_cast<std::uint8_t>((value >> 4u) & 0x0Fu);
  result.build = static_cast<std::uint16_t>((value >> 8u) & 0xFFFFu);
  result.qfe = static_cast<std::uint8_t>((value >> 24u) & 0xFFu);
  return result;
}

StfsPackageType package_type(std::span<const std::byte> bytes) noexcept {
  if (bytes.size() < 4) return StfsPackageType::Unknown;
  if (bytes[0] == std::byte{'C'} && bytes[1] == std::byte{'O'} &&
      bytes[2] == std::byte{'N'} && bytes[3] == std::byte{' '}) {
    return StfsPackageType::Con;
  }
  if (bytes[0] == std::byte{'L'} && bytes[1] == std::byte{'I'} &&
      bytes[2] == std::byte{'V'} && bytes[3] == std::byte{'E'}) {
    return StfsPackageType::Live;
  }
  if (bytes[0] == std::byte{'P'} && bytes[1] == std::byte{'I'} &&
      bytes[2] == std::byte{'R'} && bytes[3] == std::byte{'S'}) {
    return StfsPackageType::Pirs;
  }
  return StfsPackageType::Unknown;
}

std::string utf16be_to_utf8(std::span<const std::byte> bytes) {
  std::string out;
  out.reserve(bytes.size() / 2);
  for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
    const auto unit = read_be16(bytes, i);
    if (unit == 0) break;

    std::uint32_t codepoint = unit;
    if (unit >= 0xD800u && unit <= 0xDBFFu && i + 3 < bytes.size()) {
      const auto low = read_be16(bytes, i + 2);
      if (low >= 0xDC00u && low <= 0xDFFFu) {
        codepoint = 0x10000u +
                    ((static_cast<std::uint32_t>(unit) - 0xD800u) << 10u) +
                    (static_cast<std::uint32_t>(low) - 0xDC00u);
        i += 2;
      }
    }

    if (codepoint <= 0x7Fu) {
      out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FFu) {
      out.push_back(static_cast<char>(0xC0u | (codepoint >> 6u)));
      out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else if (codepoint <= 0xFFFFu) {
      out.push_back(static_cast<char>(0xE0u | (codepoint >> 12u)));
      out.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else if (codepoint <= 0x10FFFFu) {
      out.push_back(static_cast<char>(0xF0u | (codepoint >> 18u)));
      out.push_back(static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    }
  }
  return out;
}
}  // namespace

std::string_view to_string(StfsPackageType type) noexcept {
  switch (type) {
    case StfsPackageType::Con: return "CON";
    case StfsPackageType::Live: return "LIVE";
    case StfsPackageType::Pirs: return "PIRS";
    case StfsPackageType::Unknown: break;
  }
  return "unknown";
}

std::string_view to_string(XboxContentType type) noexcept {
  switch (type) {
    case XboxContentType::SavedGame: return "saved-game";
    case XboxContentType::MarketplaceContent: return "marketplace-content";
    case XboxContentType::Publisher: return "publisher";
    case XboxContentType::Xbox360Title: return "xbox-360-title";
    case XboxContentType::InstalledGame: return "installed-game";
    case XboxContentType::GamesOnDemand: return "games-on-demand";
    case XboxContentType::AvatarItem: return "avatar-item";
    case XboxContentType::Profile: return "profile";
    case XboxContentType::GamerPicture: return "gamer-picture";
    case XboxContentType::Theme: return "theme";
    case XboxContentType::GameDemo: return "game-demo";
    case XboxContentType::GameTitle: return "game-title";
    case XboxContentType::ArcadeTitle: return "arcade-title";
    case XboxContentType::CommunityGame: return "community-game";
    case XboxContentType::Unknown: break;
  }
  return "unknown";
}

std::string format_stfs_content_id(
    std::span<const std::uint8_t, 20> content_id) {
  std::ostringstream stream;
  stream << std::uppercase << std::hex << std::setfill('0');
  for (const auto byte : content_id) stream << std::setw(2) << unsigned(byte);
  return stream.str();
}

FsError parse_stfs_metadata(std::span<const std::byte> bytes,
                            StfsPackageMetadata& out_metadata) {
  out_metadata = {};
  if (bytes.size() < kHeaderMetadataSize) return FsError::InvalidArgument;

  const auto type = package_type(bytes);
  if (type == StfsPackageType::Unknown) return FsError::InvalidArgument;

  const auto header_size = read_be32(bytes, kHeaderSizeOffset);
  if (header_size < kHeaderMetadataSize || header_size > 64u * 1024u * 1024u) {
    return FsError::InvalidArgument;
  }

  out_metadata.package_type = type;
  for (std::size_t i = 0; i < out_metadata.content_id.size(); ++i) {
    out_metadata.content_id[i] =
        std::to_integer<std::uint8_t>(bytes[kContentIdOffset + i]);
  }
  out_metadata.content_id_hex = format_stfs_content_id(out_metadata.content_id);
  out_metadata.header_size = header_size;
  out_metadata.content_type =
      static_cast<XboxContentType>(read_be32(bytes, kContentTypeOffset));
  out_metadata.metadata_version = read_be32(bytes, kMetadataVersionOffset);
  out_metadata.content_size = read_be64(bytes, kContentSizeOffset);

  auto& execution = out_metadata.execution_info;
  execution.media_id = read_be32(bytes, kExecutionInfoOffset + 0x00);
  execution.version = decode_version(read_be32(bytes, kExecutionInfoOffset + 0x04));
  execution.base_version =
      decode_version(read_be32(bytes, kExecutionInfoOffset + 0x08));
  execution.title_id = read_be32(bytes, kExecutionInfoOffset + 0x0C);
  execution.platform =
      std::to_integer<std::uint8_t>(bytes[kExecutionInfoOffset + 0x10]);
  execution.executable_table =
      std::to_integer<std::uint8_t>(bytes[kExecutionInfoOffset + 0x11]);
  execution.disc_number =
      std::to_integer<std::uint8_t>(bytes[kExecutionInfoOffset + 0x12]);
  execution.disc_count =
      std::to_integer<std::uint8_t>(bytes[kExecutionInfoOffset + 0x13]);
  execution.savegame_id = read_be32(bytes, kExecutionInfoOffset + 0x14);

  const auto descriptor_length =
      std::to_integer<std::uint8_t>(bytes[kVolumeDescriptorOffset]);
  if (descriptor_length != kStfsDescriptorLength) return FsError::InvalidArgument;
  const auto descriptor_flags =
      std::to_integer<std::uint8_t>(bytes[kVolumeDescriptorOffset + 2]);
  auto& volume = out_metadata.volume;
  volume.read_only_format = (descriptor_flags & 0x01u) != 0;
  volume.root_active_index = (descriptor_flags & 0x02u) != 0;
  volume.directory_overallocated = (descriptor_flags & 0x04u) != 0;
  volume.directory_index_bounds_valid = (descriptor_flags & 0x08u) != 0;
  volume.file_table_block_count =
      static_cast<std::uint16_t>(
          std::to_integer<std::uint16_t>(bytes[kVolumeDescriptorOffset + 3]) |
          (std::to_integer<std::uint16_t>(bytes[kVolumeDescriptorOffset + 4]) << 8u));
  volume.file_table_block_number = read_le24(bytes, kVolumeDescriptorOffset + 5);
  volume.total_block_count = read_be32(bytes, kVolumeDescriptorOffset + 0x1C);
  volume.free_block_count = read_be32(bytes, kVolumeDescriptorOffset + 0x20);

  out_metadata.data_file_count = read_be32(bytes, kDataFileCountOffset);
  out_metadata.data_file_size = read_be64(bytes, kDataFileSizeOffset);
  out_metadata.volume_type = read_be32(bytes, kVolumeTypeOffset);

  // Language slot zero is English in the Xbox XContent language table.
  out_metadata.display_name = utf16be_to_utf8(
      bytes.subspan(kDisplayNamesOffset, kLanguageSlotBytes));
  out_metadata.description = utf16be_to_utf8(
      bytes.subspan(kDescriptionsOffset, kLanguageSlotBytes));
  out_metadata.publisher =
      utf16be_to_utf8(bytes.subspan(kPublisherOffset, kPublisherBytes));
  out_metadata.title_name =
      utf16be_to_utf8(bytes.subspan(kTitleNameOffset, kTitleNameBytes));
  return FsError::None;
}

FsError read_stfs_metadata(const std::filesystem::path& path,
                           StfsPackageMetadata& out_metadata) {
  out_metadata = {};
  std::error_code ec;
  const auto status = std::filesystem::symlink_status(path, ec);
  if (ec || std::filesystem::is_symlink(status) ||
      !std::filesystem::is_regular_file(status)) {
    return FsError::InvalidArgument;
  }
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) return FsError::IoError;
  if (size < kHeaderMetadataSize) return FsError::InvalidArgument;

  std::ifstream file(path, std::ios::binary);
  if (!file) return FsError::IoError;
  std::array<std::byte, kHeaderMetadataSize> header{};
  file.read(reinterpret_cast<char*>(header.data()),
            static_cast<std::streamsize>(header.size()));
  if (file.gcount() != static_cast<std::streamsize>(header.size())) {
    return FsError::IoError;
  }
  const auto error = parse_stfs_metadata(header, out_metadata);
  if (error != FsError::None) return error;
  return out_metadata.header_size <= size ? FsError::None
                                          : FsError::InvalidArgument;
}

}  // namespace xenon::filesystem

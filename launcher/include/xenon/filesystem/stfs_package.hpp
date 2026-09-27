#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

#include "xenon/filesystem/types.hpp"
#include "xenon/filesystem/xex_metadata.hpp"

namespace xenon::filesystem {

enum class StfsPackageType : std::uint8_t {
  Unknown = 0,
  Con,
  Live,
  Pirs,
};

enum class XboxContentType : std::uint32_t {
  Unknown = 0,
  SavedGame = 0x00000001,
  MarketplaceContent = 0x00000002,
  Publisher = 0x00000003,
  Xbox360Title = 0x00001000,
  InstalledGame = 0x00004000,
  GamesOnDemand = 0x00007000,
  AvatarItem = 0x00009000,
  Profile = 0x00010000,
  GamerPicture = 0x00020000,
  Theme = 0x00030000,
  GameDemo = 0x00080000,
  GameTitle = 0x000A0000,
  ArcadeTitle = 0x000D0000,
  CommunityGame = 0x02000000,
};

struct StfsVolumeInfo {
  bool read_only_format{};
  bool root_active_index{};
  bool directory_overallocated{};
  bool directory_index_bounds_valid{};
  std::uint16_t file_table_block_count{};
  std::uint32_t file_table_block_number{};
  std::uint32_t total_block_count{};
  std::uint32_t free_block_count{};
};

struct StfsPackageMetadata {
  StfsPackageType package_type{StfsPackageType::Unknown};
  XboxContentType content_type{XboxContentType::Unknown};
  std::uint32_t metadata_version{};
  std::uint64_t content_size{};
  XexExecutionInfo execution_info{};
  std::array<std::uint8_t, 20> content_id{};
  std::string content_id_hex{};
  std::uint32_t header_size{};
  std::uint32_t volume_type{};
  std::uint32_t data_file_count{};
  std::uint64_t data_file_size{};
  StfsVolumeInfo volume{};
  std::string display_name{};
  std::string description{};
  std::string publisher{};
  std::string title_name{};
};

[[nodiscard]] std::string_view to_string(StfsPackageType type) noexcept;
[[nodiscard]] std::string_view to_string(XboxContentType type) noexcept;
[[nodiscard]] std::string format_stfs_content_id(
    std::span<const std::uint8_t, 20> content_id);

// Parses the fixed XContent/STFS metadata header only. Signature verification,
// licenses and payload hashes are deliberately separate concerns.
[[nodiscard]] FsError parse_stfs_metadata(std::span<const std::byte> bytes,
                                          StfsPackageMetadata& out_metadata);
[[nodiscard]] FsError read_stfs_metadata(const std::filesystem::path& path,
                                         StfsPackageMetadata& out_metadata);

}  // namespace xenon::filesystem

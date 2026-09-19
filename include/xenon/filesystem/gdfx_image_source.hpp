#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenon/filesystem/read_only_content_device.hpp"

namespace xenon::filesystem {

struct GdfxImageInfo {
  std::filesystem::path image_path{};
  std::uint64_t image_size{};
  std::uint64_t game_partition_offset{};
  std::uint32_t root_sector{};
  std::uint32_t root_size{};
};

// Read-only Xbox Game Disc Filesystem source. This intentionally implements
// only filesystem/container semantics; executable loading, decryption and guest
// memory mapping remain in higher runtime layers.
class GdfxImageSource final : public ReadOnlyContentSource {
 public:
  explicit GdfxImageSource(std::filesystem::path image_path);
  ~GdfxImageSource() override = default;

  [[nodiscard]] FsError initialize() override;
  [[nodiscard]] FsError stat(std::string_view relative_path,
                             FileInfo& out_info) const override;
  [[nodiscard]] FsError list(
      std::string_view relative_path,
      std::vector<DirectoryEntry>& out_entries) const override;
  [[nodiscard]] FsError read_at(std::string_view relative_path,
                                std::uint64_t offset,
                                std::span<std::byte> destination,
                                std::size_t& bytes_read) const override;
  [[nodiscard]] FsError disk_space(DiskSpace& out_space) const override;

  [[nodiscard]] const GdfxImageInfo& info() const noexcept { return info_; }

 private:
  struct Node {
    std::string name{};
    std::string path{};
    std::uint64_t data_offset{};
    FileInfo info{};
    std::vector<std::size_t> children{};
  };

  [[nodiscard]] FsError normalize_relative(std::string_view relative_path,
                                           std::string& out_path) const;
  [[nodiscard]] FsError parse_directory(std::ifstream& file,
                                        std::uint64_t directory_offset,
                                        std::uint32_t directory_size,
                                        std::size_t parent_index,
                                        std::size_t depth);
  [[nodiscard]] FsError parse_entry_tree(std::ifstream& file,
                                         std::uint64_t directory_offset,
                                         std::uint32_t directory_size,
                                         std::uint16_t ordinal,
                                         std::size_t parent_index,
                                         std::size_t depth,
                                         std::vector<std::uint8_t>& visit_state);
  [[nodiscard]] FsError read_exact(std::ifstream& file, std::uint64_t offset,
                                   std::span<std::byte> destination) const;
  [[nodiscard]] const Node* find_node(std::string_view relative_path) const;

  std::filesystem::path image_path_{};
  GdfxImageInfo info_{};
  std::filesystem::file_time_type image_time_{};
  std::vector<Node> nodes_{};
  std::unordered_map<std::string, std::size_t> path_index_{};
  bool initialized_{};
};

// Resolves a common .dvd descriptor to its referenced image. Non-.dvd paths
// are returned unchanged. The descriptor is metadata only; LayerBreak lines
// are intentionally ignored because GDFX discovery locates the game partition.
[[nodiscard]] FsError resolve_gdfx_image_path(
    const std::filesystem::path& source,
    std::filesystem::path& out_image_path);

}  // namespace xenon::filesystem

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenon/filesystem/read_only_content_device.hpp"
#include "xenon/filesystem/stfs_package.hpp"

namespace xenon::filesystem {

struct StfsPackageInfo {
  std::filesystem::path package_path{};
  std::uint64_t package_size{};
  StfsPackageMetadata metadata{};
};

// Read-only provider for single-file STFS containers (CON/LIVE/PIRS). SVOD is
// intentionally a separate provider because its fragment/layout rules differ.
class StfsPackageSource final : public ReadOnlyContentSource {
 public:
  explicit StfsPackageSource(std::filesystem::path package_path);
  ~StfsPackageSource() override = default;

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

  [[nodiscard]] const StfsPackageInfo& info() const noexcept { return info_; }

 private:
  struct Node {
    std::string name{};
    std::string path{};
    FileInfo info{};
    std::vector<std::size_t> children{};
    std::vector<std::uint32_t> blocks{};
  };

  [[nodiscard]] FsError normalize_relative(std::string_view relative_path,
                                           std::string& out_path) const;
  [[nodiscard]] const Node* find_node(std::string_view relative_path) const;
  [[nodiscard]] FsError read_exact(std::ifstream& file, std::uint64_t offset,
                                   std::span<std::byte> destination) const;
  [[nodiscard]] bool block_to_offset(std::uint64_t block_index,
                                     std::uint64_t& out_offset) const noexcept;
  [[nodiscard]] bool hash_block_number(std::uint32_t block_index,
                                       std::uint32_t hash_level,
                                       std::uint32_t& out_block) const noexcept;
  [[nodiscard]] bool hash_block_offset(std::uint32_t block_index,
                                       std::uint32_t hash_level,
                                       std::uint64_t& out_offset) const noexcept;
  [[nodiscard]] FsError read_hash_entry(std::ifstream& file,
                                        std::uint32_t block_index,
                                        std::uint32_t& out_next_block) const;
  [[nodiscard]] FsError build_block_chain(std::ifstream& file,
                                          std::uint32_t start_block,
                                          std::uint32_t allocated_blocks,
                                          std::uint64_t logical_size,
                                          std::vector<std::uint32_t>& out_blocks) const;
  [[nodiscard]] FsError parse_file_table(std::ifstream& file);

  std::filesystem::path package_path_{};
  StfsPackageInfo info_{};
  std::filesystem::file_time_type package_time_{};
  std::uint32_t blocks_per_hash_table_{1};
  std::array<std::uint32_t, 2> block_step_{};
  std::vector<Node> nodes_{};
  std::unordered_map<std::string, std::size_t> path_index_{};
  bool initialized_{};
};

}  // namespace xenon::filesystem

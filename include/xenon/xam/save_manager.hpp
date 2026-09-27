#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "xenon/xam/content_graph.hpp"
#include "xenon/xam/types.hpp"

namespace xenon::xam {

// Save operation result
struct SaveOperationResult {
  bool success{false};
  std::string error_message{};
  std::filesystem::path saved_path{};
  std::filesystem::path backup_path{};
};

// Save container configuration
struct SaveContainerConfig {
  std::uint64_t profile_xuid{0};
  std::uint32_t title_id{0};
  std::string save_name{};
  std::filesystem::path base_directory{};
  bool enable_backup{true};
  std::uint32_t max_backups{3};
};

// Save manager for per-profile/per-game storage
class SaveManager {
 public:
  SaveManager() = default;
  ~SaveManager() = default;

  SaveManager(const SaveManager&) = delete;
  SaveManager& operator=(const SaveManager&) = delete;

  // Initialize the manager with a base save directory
  void initialize(const std::filesystem::path& base_save_directory);

  // Set the base directory for all saves
  void set_base_directory(const std::filesystem::path& directory);

  // Get the base directory
  [[nodiscard]] std::filesystem::path base_directory() const {
    return base_directory_;
  }

  // Create save container for profile/title
  [[nodiscard]] XResult create_save_container(
      const SaveContainerConfig& config,
      std::string& out_container_id);

  // Delete save container
  [[nodiscard]] XResult delete_save_container(
      const std::string& container_id);

  // Enumerate saves for a profile/title
  [[nodiscard]] std::vector<std::unique_ptr<SaveDataContent>> enumerate_saves(
      std::uint64_t profile_xuid,
      std::uint32_t title_id) const;

  // Get save container directory path
  [[nodiscard]] std::filesystem::path get_save_path(
      std::uint64_t profile_xuid,
      std::uint32_t title_id,
      const std::string& save_name) const;

  // Write save data with atomic operation
  [[nodiscard]] SaveOperationResult write_save_atomic(
      std::uint64_t profile_xuid,
      std::uint32_t title_id,
      const std::string& save_name,
      const std::filesystem::path& source_data);

  // Create backup of existing save
  [[nodiscard]] SaveOperationResult create_backup(
      std::uint64_t profile_xuid,
      std::uint32_t title_id,
      const std::string& save_name);

  // Restore save from backup
  [[nodiscard]] SaveOperationResult restore_from_backup(
      std::uint64_t profile_xuid,
      std::uint32_t title_id,
      const std::string& save_name,
      std::uint32_t backup_index = 0);

  // List available backups
  [[nodiscard]] std::vector<std::filesystem::path> list_backups(
      std::uint64_t profile_xuid,
      std::uint32_t title_id,
      const std::string& save_name) const;

  // Clean old backups (keep only max_count newest)
  [[nodiscard]] std::size_t clean_old_backups(
      std::uint64_t profile_xuid,
      std::uint32_t title_id,
      const std::string& save_name,
      std::uint32_t max_count);

  // Validate save container structure
  [[nodiscard]] bool validate_save_container(
      const std::filesystem::path& container_path) const;

 private:
  [[nodiscard]] std::filesystem::path get_profile_directory(
      std::uint64_t profile_xuid) const;
  
  [[nodiscard]] std::filesystem::path get_title_directory(
      std::uint64_t profile_xuid,
      std::uint32_t title_id) const;
  
  [[nodiscard]] std::filesystem::path get_backup_directory(
      std::uint64_t profile_xuid,
      std::uint32_t title_id,
      const std::string& save_name) const;
  
  [[nodiscard]] std::string generate_backup_filename() const;

  std::filesystem::path base_directory_{};
  std::map<std::string, SaveContainerConfig> containers_{};
  bool initialized_{false};
};

}  // namespace xenon::xam

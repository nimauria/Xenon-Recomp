#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xenon/xam/content_graph.hpp"
#include "xenon/xam/types.hpp"

namespace xenon::xam {

// DLC manifest entry (provided by game module)
struct DLCManifestEntry {
  std::vector<std::uint8_t> content_id;      // 20-byte XContent Content ID
  std::string content_id_hex;
  std::string display_name;
  std::string description;
  std::uint32_t title_id{0};
  std::uint32_t media_id{0};                 // Optional: specific media ID
  std::uint32_t version{0};                  // Optional: minimum version
  bool required{false};
};

// DLC validation result
struct DLCValidation {
  bool is_legitimate{false};
  bool content_id_matches{false};
  bool title_id_matches{false};
  bool structure_valid{false};
  std::string matched_name{};
  std::string error_message{};
};

// DLC manager for identification and mounting
class DLCManager {
 public:
  DLCManager() = default;
  ~DLCManager() = default;

  DLCManager(const DLCManager&) = delete;
  DLCManager& operator=(const DLCManager&) = delete;

  // Initialize the manager
  void initialize();

  // Register DLC manifest entries for a title
  void register_dlc_manifest(std::uint32_t title_id,
                            std::vector<DLCManifestEntry> entries);

  // Discover DLC packages in a directory
  [[nodiscard]] std::vector<std::unique_ptr<DLCContent>>
  discover_dlc_packages(const std::filesystem::path& search_path,
                       std::uint32_t title_id) const;

  // Identify and validate a single DLC package
  [[nodiscard]] std::optional<DLCContent> identify_dlc_package(
      const std::filesystem::path& package_path,
      std::uint32_t title_id) const;

  // Validate DLC against registered manifest
  [[nodiscard]] DLCValidation validate_dlc(
      const DLCContent& dlc,
      std::uint32_t title_id) const;

  // Get DLC manifest entries for a title
  [[nodiscard]] std::vector<DLCManifestEntry> get_manifest_entries(
      std::uint32_t title_id) const;

  // Check if content ID is in manifest
  [[nodiscard]] bool is_content_id_registered(
      std::uint32_t title_id,
      std::span<const std::uint8_t, 20> content_id) const;

 private:
  // Title ID -> list of valid DLC entries
  std::map<std::uint32_t, std::vector<DLCManifestEntry>> manifests_{};
  bool initialized_{false};
};

}  // namespace xenon::xam

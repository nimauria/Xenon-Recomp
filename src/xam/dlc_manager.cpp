#include "xenon/xam/dlc_manager.hpp"

#include <algorithm>
#include <fstream>

#include "xenon/filesystem/stfs_package.hpp"

namespace xenon::xam {

void DLCManager::initialize() {
  if (initialized_) return;
  initialized_ = true;
}

void DLCManager::register_dlc_manifest(
    std::uint32_t title_id,
    std::vector<DLCManifestEntry> entries) {
  manifests_[title_id] = std::move(entries);
}

std::vector<std::unique_ptr<DLCContent>> DLCManager::discover_dlc_packages(
    const std::filesystem::path& search_path,
    std::uint32_t title_id) const {
  
  std::vector<std::unique_ptr<DLCContent>> dlc_packages;
  
  if (!std::filesystem::exists(search_path)) {
    return dlc_packages;
  }

  // If it's a file, check if it's a single DLC package
  if (std::filesystem::is_regular_file(search_path)) {
    auto dlc_opt = identify_dlc_package(search_path, title_id);
    if (dlc_opt.has_value()) {
      auto dlc = std::make_unique<DLCContent>(std::move(*dlc_opt));
      dlc_packages.push_back(std::move(dlc));
    }
    return dlc_packages;
  }

  // Search directory for DLC packages
  if (std::filesystem::is_directory(search_path)) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             search_path, std::filesystem::directory_options::skip_permission_denied)) {
      if (!entry.is_regular_file()) continue;
      
      // Try to identify as DLC package
      auto dlc_opt = identify_dlc_package(entry.path(), title_id);
      if (dlc_opt.has_value()) {
        auto dlc = std::make_unique<DLCContent>(std::move(*dlc_opt));
        dlc_packages.push_back(std::move(dlc));
      }
    }
  }
  
  return dlc_packages;
}

std::optional<DLCContent> DLCManager::identify_dlc_package(
    const std::filesystem::path& package_path,
    std::uint32_t title_id) const {
  
  if (!std::filesystem::exists(package_path) ||
      !std::filesystem::is_regular_file(package_path)) {
    return std::nullopt;
  }
  
  // Read STFS package metadata
  filesystem::StfsPackageMetadata metadata{};
  auto error = filesystem::read_stfs_metadata(package_path, metadata);
  if (error != filesystem::FsError::None) {
    return std::nullopt;
  }
  
  // Verify it's a valid STFS package type
  if (metadata.package_type == filesystem::StfsPackageType::Unknown) {
    return std::nullopt;
  }
  
  // Check if it's DLC-type content or related
  const auto ct = metadata.content_type;
  const bool is_dlc_type = 
      ct == filesystem::XboxContentType::MarketplaceContent ||
      ct == filesystem::XboxContentType::Publisher ||
      ct == filesystem::XboxContentType::InstalledGame ||
      ct == filesystem::XboxContentType::GamesOnDemand ||
      ct == filesystem::XboxContentType::AvatarItem;
  
  if (!is_dlc_type) {
    return std::nullopt;
  }
  
  // Verify title ID matches if provided
  if (title_id != 0 && metadata.execution_info.title_id != 0) {
    if (metadata.execution_info.title_id != title_id) {
      return std::nullopt;
    }
  }
  
  // Create DLC content node
  DLCContent dlc{};
  dlc.id = metadata.content_id_hex;
  dlc.display_name = !metadata.display_name.empty() ? 
                     metadata.display_name : "DLC Package";
  dlc.source_path = package_path;
  dlc.effective_path = package_path;
  dlc.size_bytes = std::filesystem::file_size(package_path);
  dlc.title_id = metadata.execution_info.title_id;
  dlc.media_id = metadata.execution_info.media_id;
  dlc.content_id.assign(metadata.content_id.begin(), metadata.content_id.end());
  dlc.content_id_hex = metadata.content_id_hex;
  dlc.content_type = metadata.content_type;
  dlc.read_only = true;
  dlc.status = ContentStatus::Available;
  
  // Validate against manifest if available
  if (title_id != 0) {
    auto validation = validate_dlc(dlc, title_id);
    dlc.is_legitimate = validation.is_legitimate;
    if (validation.is_legitimate && !validation.matched_name.empty()) {
      dlc.display_name = validation.matched_name;
    }
  }
  
  return dlc;
}

DLCValidation DLCManager::validate_dlc(
    const DLCContent& dlc,
    std::uint32_t title_id) const {
  
  DLCValidation result{};
  
  // Check structure validity
  result.structure_valid = (dlc.content_id.size() == 20) &&
                          !dlc.content_id_hex.empty();
  
  if (!result.structure_valid) {
    result.error_message = "Invalid DLC structure";
    return result;
  }
  
  // Check title ID match
  result.title_id_matches = (dlc.title_id == title_id || dlc.title_id == 0);
  
  // Look up in manifest
  auto it = manifests_.find(title_id);
  if (it == manifests_.end()) {
    // No manifest registered - mark as legitimate if title ID matches
    result.is_legitimate = result.title_id_matches;
    result.error_message = result.is_legitimate ? 
                          "No manifest available (assumed valid)" :
                          "Title ID mismatch";
    return result;
  }
  
  // Search for content ID in manifest
  const auto& entries = it->second;
  for (const auto& entry : entries) {
    if (entry.content_id == dlc.content_id) {
      result.content_id_matches = true;
      result.matched_name = entry.display_name;
      
      // Additional validation checks if manifest specifies them
      if (entry.media_id != 0 && dlc.media_id != 0) {
        if (entry.media_id != dlc.media_id) {
          result.error_message = "Media ID mismatch with manifest";
          return result;
        }
      }
      
      result.is_legitimate = true;
      result.error_message = "Validated against manifest";
      return result;
    }
  }
  
  // Content ID not found in manifest
  result.error_message = "Content ID not declared in module manifest";
  return result;
}

std::vector<DLCManifestEntry> DLCManager::get_manifest_entries(
    std::uint32_t title_id) const {
  auto it = manifests_.find(title_id);
  if (it != manifests_.end()) {
    return it->second;
  }
  return {};
}

bool DLCManager::is_content_id_registered(
    std::uint32_t title_id,
    std::span<const std::uint8_t, 20> content_id) const {
  
  auto it = manifests_.find(title_id);
  if (it == manifests_.end()) {
    return false;
  }
  
  const auto& entries = it->second;
  return std::any_of(entries.begin(), entries.end(),
                    [&content_id](const DLCManifestEntry& entry) {
                      return std::equal(entry.content_id.begin(),
                                       entry.content_id.end(),
                                       content_id.begin(),
                                       content_id.end());
                    });
}

}  // namespace xenon::xam

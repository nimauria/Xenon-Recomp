#include "xenon/xam/title_update_manager.hpp"

#include <algorithm>
#include <fstream>

#include "xenon/filesystem/xex_metadata.hpp"

namespace xenon::xam {

void TitleUpdateManager::initialize() {
  if (initialized_) return;
  initialized_ = true;
}

std::vector<std::unique_ptr<TitleUpdateContent>>
TitleUpdateManager::discover_title_updates(
    const std::filesystem::path& search_path,
    std::uint32_t title_id) const {
  
  std::vector<std::unique_ptr<TitleUpdateContent>> updates;
  
  if (!std::filesystem::exists(search_path) ||
      !std::filesystem::is_directory(search_path)) {
    return updates;
  }

  // Search for XEX files that might be title updates
  for (const auto& entry : std::filesystem::recursive_directory_iterator(
           search_path, std::filesystem::directory_options::skip_permission_denied)) {
    if (!entry.is_regular_file()) continue;
    
    const auto& path = entry.path();
    const auto ext = path.extension().string();
    
    // Look for .xex files or common TU naming patterns
    if (ext != ".xex" && ext != ".XEX") continue;
    
    // Try to load as title update
    auto tu_opt = load_title_update(path, title_id);
    if (tu_opt.has_value()) {
      auto tu = std::make_unique<TitleUpdateContent>(std::move(*tu_opt));
      tu->status = ContentStatus::Available;
      updates.push_back(std::move(tu));
    }
  }
  
  // Sort by version (newest first)
  std::sort(updates.begin(), updates.end(),
            [](const auto& a, const auto& b) {
              return is_newer_version(a->version, b->version);
            });
  
  return updates;
}

TitleUpdateValidation TitleUpdateManager::validate_title_update(
    const TitleUpdateContent& title_update,
    const BaseGameContent& base_game) const {
  
  TitleUpdateValidation result{};
  
  // Validate title ID match
  result.title_id_matches = (title_update.title_id == base_game.title_id);
  if (!result.title_id_matches) {
    result.error_message = "Title ID mismatch";
    return result;
  }
  
  // Validate media ID compatibility
  // Title updates typically support specific base media IDs
  result.media_id_compatible = true;  // Basic check
  if (title_update.supported_base_executable != 0 &&
      title_update.supported_base_executable != base_game.media_id) {
    result.media_id_compatible = false;
    result.error_message = "Media ID not supported by this title update";
  }
  
  // Validate version compatibility
  // Title update base_version should be <= base game version
  result.version_compatible = true;
  if (base_game.execution_info.version.value != 0) {
    if (is_newer_version(title_update.base_version, 
                         base_game.execution_info.version)) {
      result.version_compatible = false;
      result.error_message = "Base game version too old for this title update";
    }
  }
  
  // Check if base executable is supported
  result.base_executable_supported = result.media_id_compatible;
  
  // Overall validity
  result.is_valid = result.title_id_matches &&
                   result.media_id_compatible &&
                   result.version_compatible &&
                   result.base_executable_supported;
  
  if (result.is_valid) {
    result.error_message = "Compatible";
  }
  
  return result;
}

std::unique_ptr<TitleUpdateContent> TitleUpdateManager::select_best_title_update(
    const std::vector<std::unique_ptr<TitleUpdateContent>>& candidates,
    const BaseGameContent& base_game) const {
  
  if (candidates.empty()) {
    return nullptr;
  }
  
  // Find the newest compatible title update
  for (const auto& candidate : candidates) {
    auto validation = validate_title_update(*candidate, base_game);
    if (validation.is_valid) {
      auto selected = std::make_unique<TitleUpdateContent>(*candidate);
      selected->is_compatible = true;
      selected->is_selected = true;
      return selected;
    }
  }
  
  return nullptr;
}

std::optional<TitleUpdateContent> TitleUpdateManager::load_title_update(
    const std::filesystem::path& path,
    std::uint32_t expected_title_id) const {
  
  if (!std::filesystem::exists(path) || !std::filesystem::is_regular_file(path)) {
    return std::nullopt;
  }
  
  // Read XEX metadata
  filesystem::XexMetadata metadata{};
  auto error = filesystem::read_xex_metadata(path, metadata);
  if (error != filesystem::FsError::None) {
    return std::nullopt;
  }
  
  // Must have execution info to be a valid title update
  if (!metadata.execution_info.has_value()) {
    return std::nullopt;
  }
  
  const auto& exec_info = *metadata.execution_info;
  
  // If expected title ID is provided, validate it
  if (expected_title_id != 0 && exec_info.title_id != expected_title_id) {
    return std::nullopt;
  }
  
  // Create title update content node
  TitleUpdateContent tu{};
  tu.id = filesystem::format_xbox_id(exec_info.title_id) + "_TU_" +
          exec_info.version.to_string();
  tu.display_name = "Title Update " + exec_info.version.to_string();
  tu.source_path = path;
  tu.effective_path = path;
  tu.size_bytes = std::filesystem::file_size(path);
  tu.title_id = exec_info.title_id;
  tu.media_id = exec_info.media_id;
  tu.version = exec_info.version;
  tu.base_version = exec_info.base_version;
  tu.supported_base_executable = exec_info.media_id;
  tu.read_only = true;
  tu.status = ContentStatus::Available;
  
  return tu;
}

bool TitleUpdateManager::is_newer_version(
    const filesystem::XexVersion& a,
    const filesystem::XexVersion& b) noexcept {
  
  if (a.major != b.major) return a.major > b.major;
  if (a.minor != b.minor) return a.minor > b.minor;
  if (a.build != b.build) return a.build > b.build;
  return a.qfe > b.qfe;
}

}  // namespace xenon::xam

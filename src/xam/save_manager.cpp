#include "xenon/xam/save_manager.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "xenon/filesystem/xex_metadata.hpp"

namespace xenon::xam {

void SaveManager::initialize(const std::filesystem::path& base_save_directory) {
  if (initialized_) return;
  
  base_directory_ = base_save_directory;
  
  // Ensure base directory exists
  if (!std::filesystem::exists(base_directory_)) {
    std::filesystem::create_directories(base_directory_);
  }
  
  initialized_ = true;
}

void SaveManager::set_base_directory(const std::filesystem::path& directory) {
  base_directory_ = directory;
  
  // Ensure directory exists
  if (!std::filesystem::exists(base_directory_)) {
    std::filesystem::create_directories(base_directory_);
  }
}

XResult SaveManager::create_save_container(
    const SaveContainerConfig& config,
    std::string& out_container_id) {
  
  if (!initialized_) {
    return result::InvalidState;
  }
  
  if (config.profile_xuid == 0 || config.title_id == 0 ||
      config.save_name.empty()) {
    return result::InvalidParameter;
  }
  
  // Generate container ID
  std::ostringstream oss;
  oss << std::hex << std::setfill('0')
      << std::setw(16) << config.profile_xuid << "_"
      << std::setw(8) << config.title_id << "_"
      << config.save_name;
  out_container_id = oss.str();
  
  // Create directory structure
  auto save_path = get_save_path(config.profile_xuid, config.title_id,
                                 config.save_name);
  
  try {
    std::filesystem::create_directories(save_path);
    
    // Create backup directory if enabled
    if (config.enable_backup) {
      auto backup_dir = get_backup_directory(config.profile_xuid,
                                             config.title_id,
                                             config.save_name);
      std::filesystem::create_directories(backup_dir);
    }
  } catch (const std::filesystem::filesystem_error&) {
    return result::Unavailable;
  }
  
  // Register container
  containers_[out_container_id] = config;
  
  return result::Success;
}

XResult SaveManager::delete_save_container(const std::string& container_id) {
  auto it = containers_.find(container_id);
  if (it == containers_.end()) {
    return result::InvalidParameter;
  }
  
  const auto& config = it->second;
  auto save_path = get_save_path(config.profile_xuid, config.title_id,
                                 config.save_name);
  
  try {
    if (std::filesystem::exists(save_path)) {
      std::filesystem::remove_all(save_path);
    }
    
    // Also remove backup directory
    auto backup_dir = get_backup_directory(config.profile_xuid,
                                           config.title_id,
                                           config.save_name);
    if (std::filesystem::exists(backup_dir)) {
      std::filesystem::remove_all(backup_dir);
    }
  } catch (const std::filesystem::filesystem_error&) {
    return result::Unavailable;
  }
  
  containers_.erase(it);
  return result::Success;
}

std::vector<std::unique_ptr<SaveDataContent>> SaveManager::enumerate_saves(
    std::uint64_t profile_xuid,
    std::uint32_t title_id) const {
  
  std::vector<std::unique_ptr<SaveDataContent>> saves;
  
  auto title_dir = get_title_directory(profile_xuid, title_id);
  if (!std::filesystem::exists(title_dir) ||
      !std::filesystem::is_directory(title_dir)) {
    return saves;
  }
  
  // Enumerate save directories
  for (const auto& entry : std::filesystem::directory_iterator(title_dir)) {
    if (!entry.is_directory()) continue;
    
    auto save_name = entry.path().filename().string();
    
    // Skip backup directories
    if (save_name.ends_with(".backups")) continue;
    
    auto save_path = entry.path();
    
    // Create save data content node
    auto save = std::make_unique<SaveDataContent>();
    save->id = std::to_string(profile_xuid) + "_" +
               filesystem::format_xbox_id(title_id) + "_" + save_name;
    save->display_name = save_name;
    save->source_path = save_path;
    save->effective_path = save_path;
    save->title_id = title_id;
    save->profile_xuid = profile_xuid;
    save->read_only = false;
    save->status = ContentStatus::Available;
    
    // Get size
    try {
      std::uint64_t total_size = 0;
      for (const auto& file : std::filesystem::recursive_directory_iterator(save_path)) {
        if (file.is_regular_file()) {
          total_size += file.file_size();
        }
      }
      save->size_bytes = total_size;
    } catch (...) {
      save->size_bytes = 0;
    }
    
    // Check for backups
    auto backup_dir = get_backup_directory(profile_xuid, title_id, save_name);
    save->has_backup = std::filesystem::exists(backup_dir) &&
                       std::filesystem::is_directory(backup_dir);
    if (save->has_backup) {
      save->backup_path = backup_dir;
    }
    
    // Get last modified time
    try {
      auto ftime = std::filesystem::last_write_time(save_path);
      auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
          ftime - std::filesystem::file_time_type::clock::now() +
          std::chrono::system_clock::now());
      save->last_modified_time = 
          std::chrono::system_clock::to_time_t(sctp);
    } catch (...) {
      save->last_modified_time = 0;
    }
    
    saves.push_back(std::move(save));
  }
  
  return saves;
}

std::filesystem::path SaveManager::get_save_path(
    std::uint64_t profile_xuid,
    std::uint32_t title_id,
    const std::string& save_name) const {
  return get_title_directory(profile_xuid, title_id) / save_name;
}

SaveOperationResult SaveManager::write_save_atomic(
    std::uint64_t profile_xuid,
    std::uint32_t title_id,
    const std::string& save_name,
    const std::filesystem::path& source_data) {
  
  SaveOperationResult result{};
  
  if (!initialized_) {
    result.error_message = "Save manager not initialized";
    return result;
  }
  
  auto final_path = get_save_path(profile_xuid, title_id, save_name);
  auto temp_path = final_path;
  temp_path += ".tmp";
  
  try {
    // Ensure parent directory exists
    std::filesystem::create_directories(final_path.parent_path());
    
    // Create backup of existing save if it exists
    if (std::filesystem::exists(final_path)) {
      auto backup_result = create_backup(profile_xuid, title_id, save_name);
      if (!backup_result.success) {
        result.error_message = "Failed to create backup: " + 
                              backup_result.error_message;
        return result;
      }
    }
    
    // Copy source to temporary location
    if (std::filesystem::is_directory(source_data)) {
      std::filesystem::copy(source_data, temp_path,
                           std::filesystem::copy_options::recursive |
                           std::filesystem::copy_options::overwrite_existing);
    } else {
      std::filesystem::copy_file(source_data, temp_path,
                                std::filesystem::copy_options::overwrite_existing);
    }
    
    // Atomic rename
    if (std::filesystem::exists(final_path)) {
      std::filesystem::remove_all(final_path);
    }
    std::filesystem::rename(temp_path, final_path);
    
    result.success = true;
    result.saved_path = final_path;
    
  } catch (const std::filesystem::filesystem_error& e) {
    result.error_message = e.what();
    
    // Clean up temp file on failure
    try {
      if (std::filesystem::exists(temp_path)) {
        std::filesystem::remove_all(temp_path);
      }
    } catch (...) {}
  }
  
  return result;
}

SaveOperationResult SaveManager::create_backup(
    std::uint64_t profile_xuid,
    std::uint32_t title_id,
    const std::string& save_name) {
  SaveOperationResult result{};
  auto save_path = get_save_path(profile_xuid, title_id, save_name);
  if (!std::filesystem::exists(save_path)) {
    result.error_message = "Save does not exist";
    return result;
  }
  auto backup_dir = get_backup_directory(profile_xuid, title_id, save_name);
  try {
    std::filesystem::create_directories(backup_dir);
    auto backup_filename = generate_backup_filename();
    auto backup_path = backup_dir / backup_filename;
    std::filesystem::copy(save_path, backup_path, std::filesystem::copy_options::recursive);
    result.success = true;
    result.backup_path = backup_path;
    clean_old_backups(profile_xuid, title_id, save_name, 3);
  } catch (const std::filesystem::filesystem_error& e) {
    result.error_message = e.what();
  }
  return result;
}

SaveOperationResult SaveManager::restore_from_backup(
    std::uint64_t profile_xuid, std::uint32_t title_id,
    const std::string& save_name, std::uint32_t backup_index) {
  SaveOperationResult result{};
  auto backups = list_backups(profile_xuid, title_id, save_name);
  if (backups.empty() || backup_index >= backups.size()) {
    result.error_message = "Backup not found";
    return result;
  }
  auto backup_path = backups[backup_index];
  auto save_path = get_save_path(profile_xuid, title_id, save_name);
  try {
    if (std::filesystem::exists(save_path)) {
      std::filesystem::remove_all(save_path);
    }
    std::filesystem::copy(backup_path, save_path, std::filesystem::copy_options::recursive);
    result.success = true;
    result.saved_path = save_path;
  } catch (const std::filesystem::filesystem_error& e) {
    result.error_message = e.what();
  }
  return result;
}

std::vector<std::filesystem::path> SaveManager::list_backups(
    std::uint64_t profile_xuid, std::uint32_t title_id,
    const std::string& save_name) const {
  std::vector<std::filesystem::path> backups;
  auto backup_dir = get_backup_directory(profile_xuid, title_id, save_name);
  if (!std::filesystem::exists(backup_dir)) return backups;
  for (const auto& entry : std::filesystem::directory_iterator(backup_dir)) {
    backups.push_back(entry.path());
  }
  std::sort(backups.begin(), backups.end(), [](const auto& a, const auto& b) {
    return std::filesystem::last_write_time(a) > std::filesystem::last_write_time(b);
  });
  return backups;
}

std::size_t SaveManager::clean_old_backups(std::uint64_t profile_xuid, std::uint32_t title_id,
    const std::string& save_name, std::uint32_t max_count) {
  auto backups = list_backups(profile_xuid, title_id, save_name);
  if (backups.size() <= max_count) return 0;
  std::size_t removed = 0;
  for (std::size_t i = max_count; i < backups.size(); ++i) {
    try { std::filesystem::remove_all(backups[i]); ++removed; } catch (...) {}
  }
  return removed;
}

bool SaveManager::validate_save_container(const std::filesystem::path& container_path) const {
  return std::filesystem::exists(container_path) && std::filesystem::is_directory(container_path);
}

std::filesystem::path SaveManager::get_profile_directory(std::uint64_t profile_xuid) const {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0') << std::setw(16) << profile_xuid;
  return base_directory_ / oss.str();
}

std::filesystem::path SaveManager::get_title_directory(
    std::uint64_t profile_xuid, std::uint32_t title_id) const {
  return get_profile_directory(profile_xuid) / filesystem::format_xbox_id(title_id);
}

std::filesystem::path SaveManager::get_backup_directory(
    std::uint64_t profile_xuid, std::uint32_t title_id,
    const std::string& save_name) const {
  return get_title_directory(profile_xuid, title_id) / (save_name + ".backups");
}

std::string SaveManager::generate_backup_filename() const {
  auto now = std::chrono::system_clock::now();
  auto time_t = std::chrono::system_clock::to_time_t(now);
  std::ostringstream oss;
  oss << "backup_" << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S");
  return oss.str();
}

}  // namespace xenon::xam

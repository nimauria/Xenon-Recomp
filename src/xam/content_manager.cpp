#include "xenon/xam/content_manager.hpp"

#include "xenon/filesystem/gdfx_image_source.hpp"
#include "xenon/filesystem/host_path_device.hpp"
#include "xenon/filesystem/read_only_content_device.hpp"
#include "xenon/filesystem/stfs_package_source.hpp"
#include "xenon/filesystem/virtual_file_system.hpp"
#include "xenon/xam/content_graph.hpp"
#include "xenon/xam/dlc_manager.hpp"
#include "xenon/xam/save_manager.hpp"
#include "xenon/xam/title_update_manager.hpp"

namespace xenon::xam {

ContentManager::ContentManager() = default;
ContentManager::~ContentManager() = default;

void ContentManager::initialize() {
  if (initialized_) return;

  // Create default HDD device
  StorageDevice hdd{};
  hdd.device_id = storage_device::HardDisk;
  hdd.name = "HDD";
  hdd.total_bytes = 20ULL * 1024 * 1024 * 1024;  // 20 GB virtual
  hdd.free_bytes = 15ULL * 1024 * 1024 * 1024;   // 15 GB free
  hdd.is_valid = true;

  devices_[storage_device::HardDisk] = hdd;
  default_device_id_ = storage_device::HardDisk;

  initialized_ = true;
}

std::vector<StorageDevice> ContentManager::enumerate_devices() const {
  std::vector<StorageDevice> result;
  result.reserve(devices_.size());
  for (const auto& [id, device] : devices_) {
    if (device.is_valid) {
      result.push_back(device);
    }
  }
  return result;
}

std::optional<StorageDevice> ContentManager::get_device(std::uint32_t device_id) const {
  auto it = devices_.find(device_id);
  if (it != devices_.end() && it->second.is_valid) {
    return it->second;
  }
  return std::nullopt;
}

std::vector<ContentData> ContentManager::enumerate_content(
    std::uint32_t device_id,
    ContentType type,
    std::uint64_t title_id) const {
  
  std::vector<ContentData> result;
  for (const auto& [id, content] : content_) {
    bool matches = content.device_id == device_id &&
                   content.content_type == type;
    
    if (title_id != 0) {
      matches = matches && content.title_id == title_id;
    }
    
    if (matches) {
      result.push_back(content);
    }
  }
  return result;
}

XResult ContentManager::create_content(
    std::uint32_t device_id,
    const std::string& name,
    ContentType type,
    std::uint64_t title_id,
    std::uint32_t& out_content_id) {
  
  auto device_opt = get_device(device_id);
  if (!device_opt.has_value()) {
    return result::InvalidParameter;
  }

  ContentData content{};
  content.content_id = next_content_id_++;
  content.content_type = type;
  content.display_name = name;
  content.file_name = name;
  content.device_id = device_id;
  content.title_id = title_id;
  content.size_bytes = 0;

  content_[content.content_id] = content;
  out_content_id = content.content_id;

  return result::Success;
}

XResult ContentManager::delete_content(std::uint32_t content_id) {
  auto it = content_.find(content_id);
  if (it == content_.end()) {
    return result::InvalidParameter;
  }

  content_.erase(it);
  return result::Success;
}


// Content Services V1 Implementation

void ContentManager::initialize_content_services(
    const std::filesystem::path& base_save_directory) {
  if (content_services_initialized_) return;
  
  title_update_manager_ = std::make_unique<TitleUpdateManager>();
  title_update_manager_->initialize();
  
  dlc_manager_ = std::make_unique<DLCManager>();
  dlc_manager_->initialize();
  
  save_manager_ = std::make_unique<SaveManager>();
  save_manager_->initialize(base_save_directory);
  
  content_services_initialized_ = true;
}

void ContentManager::register_dlc_manifest(
    std::uint32_t title_id,
    std::vector<DLCManifestEntry> entries) {
  if (!dlc_manager_) return;
  dlc_manager_->register_dlc_manifest(title_id, std::move(entries));
}

std::unique_ptr<ContentGraph> ContentManager::build_content_graph(
    std::uint32_t title_id,
    const std::filesystem::path& base_content_path,
    const std::filesystem::path& title_update_path,
    const std::filesystem::path& dlc_path,
    std::uint64_t profile_xuid) {
  
  if (!content_services_initialized_) return nullptr;
  
  auto graph = std::make_unique<ContentGraph>();
  graph->title_id = title_id;
  graph->title_id_hex = filesystem::format_xbox_id(title_id);
  
  // Build base game content
  if (std::filesystem::exists(base_content_path)) {
    auto base = std::make_unique<BaseGameContent>();
    base->id = "base_" + graph->title_id_hex;
    base->display_name = "Base Game";
    base->source_path = base_content_path;
    base->effective_path = base_content_path;
    base->title_id = title_id;
    base->status = ContentStatus::Available;
    
    // Try to load XEX metadata
    std::filesystem::path xex_path;
    if (std::filesystem::is_directory(base_content_path)) {
      auto default_xex = base_content_path / "default.xex";
      if (std::filesystem::exists(default_xex)) {
        xex_path = default_xex;
      }
    } else if (std::filesystem::is_regular_file(base_content_path) &&
               base_content_path.extension() == ".xex") {
      xex_path = base_content_path;
    }
    
    if (!xex_path.empty()) {
      base->default_xex_path = xex_path;
      base->default_xex_guest_path = "game:\\default.xex";
      
      filesystem::XexMetadata metadata{};
      if (filesystem::read_xex_metadata(xex_path, metadata) == filesystem::FsError::None) {
        if (metadata.execution_info.has_value()) {
          base->execution_info = *metadata.execution_info;
          base->media_id = base->execution_info.media_id;
        }
      }
    }
    
    graph->base = std::move(base);
  }
  
  // Discover title updates
  if (graph->base && std::filesystem::exists(title_update_path)) {
    auto updates = title_update_manager_->discover_title_updates(title_update_path, title_id);
    for (auto& update : updates) {
      graph->available_title_updates.push_back(std::move(update));
    }
    if (!graph->available_title_updates.empty()) {
      auto selected = title_update_manager_->select_best_title_update(
          graph->available_title_updates, *graph->base);
      if (selected) {
        graph->selected_title_update = std::move(selected);
      }
    }
  }
  
  // Discover DLC
  if (std::filesystem::exists(dlc_path)) {
    auto dlc_packages = dlc_manager_->discover_dlc_packages(dlc_path, title_id);
    for (auto& dlc : dlc_packages) {
      if (dlc->is_legitimate) {
        graph->installed_dlc.push_back(std::move(dlc));
      }
    }
  }
  
  // Enumerate saves
  if (save_manager_) {
    graph->saves = save_manager_->enumerate_saves(profile_xuid, title_id);
  }
  
  return graph;
}

bool ContentManager::mount_content_graph(
    filesystem::VirtualFileSystem& vfs,
    const ContentGraph& graph) {

  if (!graph.has_base()) return false;

  // Mount base game. Failure here is fatal to the whole mount: nothing else
  // is guest-visible without "game:".
  const auto& base_path = graph.base->source_path;
  bool base_mounted = false;

  if (std::filesystem::is_directory(base_path)) {
    filesystem::HostPathDeviceOptions options{};
    options.read_only = true;
    options.create_root = false;
    auto device = std::make_shared<filesystem::HostPathDevice>("game:", base_path, options);
    base_mounted = vfs.register_device(device) == filesystem::FsError::None;
  } else if (base_path.extension() == ".iso" || base_path.extension() == ".dvd" ||
             base_path.extension() == ".xgd") {
    auto source = std::make_shared<filesystem::GdfxImageSource>(base_path);
    if (source->initialize() == filesystem::FsError::None) {
      auto device = std::make_shared<filesystem::ReadOnlyContentDevice>("game:", std::move(source));
      base_mounted = vfs.register_device(device) == filesystem::FsError::None;
    }
  }
  if (!base_mounted) return false;

  // Register symbolic links. Best-effort beyond this point: a title update,
  // DLC package, or save directory that fails to mount just leaves that one
  // guest path unavailable rather than aborting the whole session.
  static_cast<void>(vfs.register_symbolic_link("d:", "game:"));
  static_cast<void>(vfs.register_symbolic_link("dvd:", "game:"));
  static_cast<void>(vfs.set_working_directory("game:"));

  // Mount DLC
  for (std::size_t i = 0; i < graph.installed_dlc.size(); ++i) {
    const auto& dlc = graph.installed_dlc[i];
    auto stfs_source = std::make_shared<filesystem::StfsPackageSource>(dlc->source_path);
    if (stfs_source->initialize() == filesystem::FsError::None) {
      const auto mount_point = "dlc" + std::to_string(i) + ":";
      auto device = std::make_shared<filesystem::ReadOnlyContentDevice>(mount_point, std::move(stfs_source));
      static_cast<void>(vfs.register_device(device));
    }
  }

  // Mount save directory
  if (!graph.saves.empty() && save_manager_) {
    const auto& first_save = graph.saves[0];
    auto save_base = save_manager_->base_directory() /
                    std::to_string(first_save->profile_xuid) /
                    filesystem::format_xbox_id(graph.title_id);
    if (std::filesystem::exists(save_base)) {
      filesystem::HostPathDeviceOptions save_options{};
      save_options.read_only = false;
      save_options.create_root = true;
      auto device = std::make_shared<filesystem::HostPathDevice>("saves:", save_base, save_options);
      static_cast<void>(vfs.register_device(device));
    }
  }

  return true;
}

}  // namespace xenon::xam


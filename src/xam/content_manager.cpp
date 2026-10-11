#include "xenon/xam/content_manager.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string_view>
#include <system_error>

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
ContentManager::~ContentManager() {
  if (!scratch_root_.empty()) {
    std::error_code error;
    std::filesystem::remove_all(scratch_root_, error);
  }
}

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
  std::scoped_lock lock(mutex_);
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

namespace {

std::string hex32(std::uint64_t value) {
  char text[9]{};
  std::snprintf(text, sizeof(text), "%08X", static_cast<unsigned>(value & 0xFFFFFFFFu));
  return text;
}

// A content file name becomes a host directory name, so it must be one
// plain path component.
bool is_plain_file_name(const std::string& name) {
  if (name.empty() || name == "." || name == "..") return false;
  return std::none_of(name.begin(), name.end(), [](char c) {
    const auto u = static_cast<unsigned char>(c);
    return u < 0x20u || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
           c == '"' || c == '<' || c == '>' || c == '|';
  });
}

std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

constexpr XResult kErrorFileNotFound = 0x00000002u;
constexpr XResult kErrorAccessDenied = 0x00000005u;
constexpr XResult kErrorAlreadyExists = 0x000000B7u;

}  // namespace

std::filesystem::path ContentManager::package_root() const {
  if (!content_root_.empty()) return content_root_;
  if (scratch_root_.empty()) {
    static std::atomic<std::uint32_t> counter{0};
    scratch_root_ = std::filesystem::temp_directory_path() /
                    ("xenon-content-" + std::to_string(static_cast<unsigned long long>(
                                            std::chrono::steady_clock::now()
                                                .time_since_epoch()
                                                .count())) +
                     "-" + std::to_string(counter.fetch_add(1)));
  }
  return scratch_root_;
}

std::filesystem::path ContentManager::package_path(const ContentData& data) const {
  return package_root() / hex32(data.title_id) /
         hex32(static_cast<std::uint32_t>(data.content_type)) / data.file_name;
}

void ContentManager::set_content_root(std::filesystem::path root) {
  std::scoped_lock lock(mutex_);
  content_root_ = std::move(root);
  // List the packages already there, as xenia's ListContent does.
  std::error_code error;
  for (const auto& title : std::filesystem::directory_iterator(content_root_, error)) {
    if (!title.is_directory()) continue;
    const auto title_text = title.path().filename().string();
    if (title_text.size() != 8u) continue;
    const auto title_id = std::strtoull(title_text.c_str(), nullptr, 16);
    for (const auto& type : std::filesystem::directory_iterator(title.path(), error)) {
      if (!type.is_directory()) continue;
      const auto type_text = type.path().filename().string();
      if (type_text.size() != 8u) continue;
      const auto content_type = static_cast<ContentType>(std::strtoul(type_text.c_str(), nullptr, 16));
      for (const auto& package : std::filesystem::directory_iterator(type.path(), error)) {
        if (!package.is_directory()) continue;
        ContentData content{};
        content.content_id = next_content_id_++;
        content.content_type = content_type;
        content.file_name = package.path().filename().string();
        content.display_name = content.file_name;
        std::ifstream display(type.path() / (content.file_name + ".display"), std::ios::binary);
        if (display) std::getline(display, content.display_name, '\0');
        content.device_id = storage_device::HardDisk;
        content.title_id = title_id;
        content_[content.content_id] = std::move(content);
      }
    }
  }
}

std::optional<ContentData> ContentManager::find_content(std::uint32_t device_id, ContentType type,
                                                        std::uint64_t title_id,
                                                        const std::string& file_name) const {
  std::scoped_lock lock(mutex_);
  for (const auto& [id, content] : content_) {
    if (content.device_id == device_id && content.content_type == type &&
        content.title_id == title_id && lower(content.file_name) == lower(file_name)) {
      return content;
    }
  }
  return std::nullopt;
}

XResult ContentManager::create_content(
    std::uint32_t device_id,
    const std::string& name,
    ContentType type,
    std::uint64_t title_id,
    std::uint32_t& out_content_id) {
  ContentData content{};
  content.content_type = type;
  content.display_name = name;
  content.file_name = name;
  content.device_id = device_id;
  content.title_id = title_id;
  return create_content(content, out_content_id);
}

XResult ContentManager::create_content(const ContentData& data, std::uint32_t& out_content_id) {
  if (!get_device(data.device_id).has_value() || !is_plain_file_name(data.file_name)) {
    return result::InvalidParameter;
  }
  if (find_content(data.device_id, data.content_type, data.title_id, data.file_name)) {
    return kErrorAlreadyExists;
  }
  std::scoped_lock lock(mutex_);
  const auto path = package_path(data);
  std::error_code error;
  std::filesystem::create_directories(path, error);
  if (error || !std::filesystem::is_directory(path)) return kErrorAccessDenied;
  {
    std::ofstream display(path.parent_path() / (data.file_name + ".display"),
                          std::ios::binary | std::ios::trunc);
    display << data.display_name;
  }
  ContentData content = data;
  content.content_id = next_content_id_++;
  content.size_bytes = 0;
  out_content_id = content.content_id;
  content_[content.content_id] = std::move(content);
  return result::Success;
}

XResult ContentManager::delete_content(std::uint32_t content_id) {
  std::scoped_lock lock(mutex_);
  auto it = content_.find(content_id);
  if (it == content_.end()) {
    return result::InvalidParameter;
  }
  for (const auto& [root, package] : open_packages_) {
    if (package.content_id == content_id) return kErrorAccessDenied;
  }
  // A package the host cannot remove (on Windows, one with a file still
  // open) is reported as access denied and stays listed.
  const auto path = package_path(it->second);
  std::error_code error;
  std::filesystem::remove_all(path, error);
  if (error || std::filesystem::exists(path)) return kErrorAccessDenied;
  std::filesystem::remove(path.parent_path() / (it->second.file_name + ".display"), error);
  content_.erase(it);
  return result::Success;
}

XResult ContentManager::open_package(filesystem::VirtualFileSystem& vfs,
                                     const std::string& root_name, std::uint32_t content_id) {
  if (root_name.empty() || root_name.find_first_of(":\\/") != std::string::npos) {
    return result::InvalidParameter;
  }
  std::scoped_lock lock(mutex_);
  const auto key = lower(root_name);
  if (open_packages_.count(key) != 0u) return kErrorAlreadyExists;
  const auto it = content_.find(content_id);
  if (it == content_.end()) return kErrorFileNotFound;
  const auto path = package_path(it->second);
  if (!std::filesystem::is_directory(path)) return kErrorFileNotFound;

  filesystem::HostPathDeviceOptions options{};
  options.read_only = false;
  options.create_root = false;
  OpenPackage package{"\\Device\\XContent" + std::to_string(next_package_device_++),
                      root_name + ":", content_id};
  auto device = std::make_shared<filesystem::HostPathDevice>(package.device_path, path, options);
  if (vfs.register_device(device) != filesystem::FsError::None) return kErrorAccessDenied;
  if (vfs.register_symbolic_link(package.alias, package.device_path) != filesystem::FsError::None) {
    static_cast<void>(vfs.unregister_device(package.device_path));
    return kErrorAlreadyExists;
  }
  open_packages_.emplace(key, std::move(package));
  return result::Success;
}

XResult ContentManager::close_package(filesystem::VirtualFileSystem& vfs,
                                      const std::string& root_name) {
  std::scoped_lock lock(mutex_);
  const auto it = open_packages_.find(lower(root_name));
  if (it == open_packages_.end()) return kErrorFileNotFound;
  static_cast<void>(vfs.unregister_symbolic_link(it->second.alias));
  static_cast<void>(vfs.unregister_device(it->second.device_path));
  open_packages_.erase(it);
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

  // VFS devices are always mounted at absolute guest device paths. Drive-
  // style names such as game:/d:/dvd:/dlcN:/saves: are symbolic aliases,
  // not Device mount points (Device::Device enforces this contract).
  constexpr std::string_view kBaseDevicePath = "\\Device\\CdRom0";
  constexpr std::string_view kSaveDevicePath = "\\Device\\Harddisk0\\Partition1\\Saves";

  // Mount base game. Failure here is fatal to the whole mount: nothing else
  // is guest-visible without "game:".
  const auto& base_path = graph.base->source_path;
  bool base_mounted = false;

  if (std::filesystem::is_directory(base_path)) {
    filesystem::HostPathDeviceOptions options{};
    options.read_only = true;
    options.create_root = false;
    auto device = std::make_shared<filesystem::HostPathDevice>(
        std::string(kBaseDevicePath), base_path, options);
    base_mounted = vfs.register_device(device) == filesystem::FsError::None;
  } else {
    auto extension = base_path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (extension == ".xex") {
      // A loose XEX is an executable selection, not a complete filesystem.
      // Mount its containing directory as game:/ so sibling assets remain
      // visible exactly as they are for an extracted game directory.
      filesystem::HostPathDeviceOptions options{};
      options.read_only = true;
      options.create_root = false;
      auto root = base_path.parent_path();
      if (root.empty()) root = ".";
      auto device = std::make_shared<filesystem::HostPathDevice>(
          std::string(kBaseDevicePath), root, options);
      base_mounted = vfs.register_device(device) == filesystem::FsError::None;
    } else if (extension == ".iso" || extension == ".dvd" || extension == ".xgd") {
      // .dvd is a descriptor, not disc bytes. Resolve it through the same
      // helper the content probe/preparation path uses so all runtime entry
      // points mount the referenced image rather than trying to parse the
      // descriptor text as GDFX. Non-.dvd paths are returned unchanged.
      std::filesystem::path image_path;
      if (filesystem::resolve_gdfx_image_path(base_path, image_path) == filesystem::FsError::None) {
        auto source = std::make_shared<filesystem::GdfxImageSource>(image_path);
        if (source->initialize() == filesystem::FsError::None) {
          auto device = std::make_shared<filesystem::ReadOnlyContentDevice>(
              std::string(kBaseDevicePath), std::move(source));
          base_mounted = vfs.register_device(device) == filesystem::FsError::None;
        }
      }
    }
  }
  if (!base_mounted) return false;

  // Register the aliases titles expect. Point them directly at the physical
  // device path (rather than chaining aliases) to keep resolution simple and
  // deterministic. This also matches the convention exercised by the VFS
  // tests and used by established Xbox 360 recomp runtimes.
  static_cast<void>(vfs.register_symbolic_link("game:", kBaseDevicePath));
  static_cast<void>(vfs.register_symbolic_link("d:", kBaseDevicePath));
  static_cast<void>(vfs.register_symbolic_link("dvd:", kBaseDevicePath));
  static_cast<void>(vfs.set_working_directory("game:"));

  // Mount DLC. Each package gets a real internal device path and a stable
  // dlcN: alias. Device constructors must never receive the alias itself.
  for (std::size_t i = 0; i < graph.installed_dlc.size(); ++i) {
    const auto& dlc = graph.installed_dlc[i];
    auto stfs_source = std::make_shared<filesystem::StfsPackageSource>(dlc->source_path);
    if (stfs_source->initialize() == filesystem::FsError::None) {
      const auto device_path = "\\Device\\Content" + std::to_string(i);
      const auto alias = "dlc" + std::to_string(i) + ":";
      auto device = std::make_shared<filesystem::ReadOnlyContentDevice>(
          device_path, std::move(stfs_source));
      if (vfs.register_device(device) == filesystem::FsError::None) {
        static_cast<void>(vfs.register_symbolic_link(alias, device_path));
      }
    }
  }

  // Mount save directory through the same physical-device + alias model.
  if (!graph.saves.empty() && save_manager_) {
    const auto& first_save = graph.saves[0];
    auto save_base = save_manager_->base_directory() /
                    std::to_string(first_save->profile_xuid) /
                    filesystem::format_xbox_id(graph.title_id);
    if (std::filesystem::exists(save_base)) {
      filesystem::HostPathDeviceOptions save_options{};
      save_options.read_only = false;
      save_options.create_root = true;
      auto device = std::make_shared<filesystem::HostPathDevice>(
          std::string(kSaveDevicePath), save_base, save_options);
      if (vfs.register_device(device) == filesystem::FsError::None) {
        static_cast<void>(vfs.register_symbolic_link("saves:", kSaveDevicePath));
      }
    }
  }

  return true;
}

}  // namespace xenon::xam


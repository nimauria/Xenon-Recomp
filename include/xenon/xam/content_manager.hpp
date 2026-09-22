#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "xenon/xam/types.hpp"

// Forward declarations
namespace xenon::filesystem {
class VirtualFileSystem;
}

namespace xenon::xam {

// Forward declarations
struct ContentGraph;
class TitleUpdateManager;
class DLCManager;
class SaveManager;
struct DLCManifestEntry;

// Content types
enum class ContentType : std::uint32_t {
  SavedGame = 0x00000001,
  Marketplace = 0x00000002,
  Publisher = 0x00000003,
  Profile = 0x00010000,
  GamerPicture = 0x00020000,
  Theme = 0x00030000,
  Video = 0x00040000,
  GameDemo = 0x00080000,
  GameTitle = 0x000A0000,
  Installer = 0x000B0000,
  DLC = 0x000C0000
};

// Storage device information
struct StorageDevice {
  std::uint32_t device_id{storage_device::Invalid};
  std::string name{};
  std::uint64_t free_bytes{0};
  std::uint64_t total_bytes{0};
  bool is_valid{false};
};

// Content metadata
struct ContentData {
  std::uint32_t content_id{0};
  ContentType content_type{ContentType::SavedGame};
  std::string display_name{};
  std::string file_name{};
  std::uint32_t device_id{storage_device::Invalid};
  std::uint64_t size_bytes{0};
  std::uint64_t title_id{0};
};

// Manages storage devices and content enumeration
class ContentManager {
 public:
  ContentManager();
  // Out-of-line so std::unique_ptr<SaveManager>'s destructor instantiates
  // where SaveManager's complete type is visible (content_manager.cpp),
  // not wherever this header happens to be included.
  ~ContentManager();

  ContentManager(const ContentManager&) = delete;
  ContentManager& operator=(const ContentManager&) = delete;

  void initialize();

  // Storage devices
  [[nodiscard]] std::vector<StorageDevice> enumerate_devices() const;
  [[nodiscard]] std::optional<StorageDevice> get_device(std::uint32_t device_id) const;
  [[nodiscard]] std::uint32_t default_device() const { return default_device_id_; }
  void set_default_device(std::uint32_t device_id) { default_device_id_ = device_id; }

  // Content enumeration
  [[nodiscard]] std::vector<ContentData> enumerate_content(
      std::uint32_t device_id,
      ContentType type,
      std::uint64_t title_id = 0) const;

  // Content operations
  [[nodiscard]] XResult create_content(
      std::uint32_t device_id,
      const std::string& name,
      ContentType type,
      std::uint64_t title_id,
      std::uint32_t& out_content_id);

  [[nodiscard]] XResult delete_content(std::uint32_t content_id);

  // Content Services V1 Integration
  
  // Initialize content services (TU manager, DLC manager, Save manager)
  void initialize_content_services(const std::filesystem::path& base_save_directory);
  
  // Register DLC manifest for a title (called by game modules)
  void register_dlc_manifest(std::uint32_t title_id,
                            std::vector<DLCManifestEntry> entries);
  
  // Build complete content graph for a title
  [[nodiscard]] std::unique_ptr<ContentGraph> build_content_graph(
      std::uint32_t title_id,
      const std::filesystem::path& base_content_path,
      const std::filesystem::path& title_update_path,
      const std::filesystem::path& dlc_path,
      std::uint64_t profile_xuid);
  
  // Mount content graph to VFS
  [[nodiscard]] bool mount_content_graph(
      filesystem::VirtualFileSystem& vfs,
      const ContentGraph& graph);
  
  // Get content services managers (for advanced usage)
  [[nodiscard]] TitleUpdateManager* title_update_manager() { 
    return title_update_manager_.get(); 
  }
  [[nodiscard]] DLCManager* dlc_manager() { 
    return dlc_manager_.get(); 
  }
  [[nodiscard]] SaveManager* save_manager() { 
    return save_manager_.get(); 
  }

 private:
  std::map<std::uint32_t, StorageDevice> devices_{};
  std::map<std::uint32_t, ContentData> content_{};
  std::uint32_t default_device_id_{storage_device::HardDisk};
  std::uint32_t next_content_id_{1};
  bool initialized_{false};
  
  // Content Services V1 managers
  std::unique_ptr<TitleUpdateManager> title_update_manager_{};
  std::unique_ptr<DLCManager> dlc_manager_{};
  std::unique_ptr<SaveManager> save_manager_{};
  bool content_services_initialized_{false};
};

}  // namespace xenon::xam

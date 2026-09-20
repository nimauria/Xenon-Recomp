#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "xenon/filesystem/stfs_package.hpp"
#include "xenon/filesystem/types.hpp"
#include "xenon/filesystem/xex_metadata.hpp"

namespace xenon::xam {

// Content node types in the content graph
enum class ContentNodeType : std::uint8_t {
  BaseGame = 0,
  TitleUpdate,
  DLC,
  SaveData,
  Profile,
  WritableStorage,
};

// Content availability status
enum class ContentStatus : std::uint8_t {
  Unknown = 0,
  Available,
  Mounted,
  Missing,
  Invalid,
  Incompatible,
};

// Base content node in the graph
struct ContentNode {
  std::string id{};                          // Unique identifier
  ContentNodeType type{ContentNodeType::BaseGame};
  ContentStatus status{ContentStatus::Unknown};
  std::string display_name{};
  std::filesystem::path source_path{};       // Original location
  std::filesystem::path effective_path{};    // Mounted/working path
  std::uint64_t size_bytes{0};
  std::uint32_t title_id{0};
  std::uint32_t media_id{0};
  bool read_only{true};
  
  [[nodiscard]] bool is_available() const noexcept {
    return status == ContentStatus::Available || status == ContentStatus::Mounted;
  }
};

// Base game content
struct BaseGameContent : public ContentNode {
  filesystem::XexExecutionInfo execution_info{};
  std::filesystem::path default_xex_path{};
  std::string default_xex_guest_path{};
  
  BaseGameContent() {
    type = ContentNodeType::BaseGame;
  }
};

// Title update content
struct TitleUpdateContent : public ContentNode {
  filesystem::XexVersion version{};
  filesystem::XexVersion base_version{};      // Minimum base version required
  std::uint32_t supported_base_executable{};  // Base media_id this TU supports
  bool is_selected{false};
  bool is_compatible{false};
  
  TitleUpdateContent() {
    type = ContentNodeType::TitleUpdate;
  }
};

// DLC content
struct DLCContent : public ContentNode {
  std::vector<std::uint8_t> content_id{};    // 20-byte XContent Content ID
  std::string content_id_hex{};
  filesystem::XboxContentType content_type{filesystem::XboxContentType::Unknown};
  bool is_legitimate{false};                 // Validated against title manifest
  
  DLCContent() {
    type = ContentNodeType::DLC;
  }
};

// Save data content
struct SaveDataContent : public ContentNode {
  std::uint64_t profile_xuid{0};
  std::string profile_name{};
  std::filesystem::path backup_path{};
  std::uint64_t last_modified_time{0};
  bool has_backup{false};
  
  SaveDataContent() {
    type = ContentNodeType::SaveData;
    read_only = false;
  }
};

// Writable storage for game-specific data
struct WritableStorageContent : public ContentNode {
  std::uint64_t profile_xuid{0};
  std::uint64_t max_size_bytes{0};
  
  WritableStorageContent() {
    type = ContentNodeType::WritableStorage;
    read_only = false;
  }
};

// Complete content graph for a title
struct ContentGraph {
  std::string title_id_hex{};
  std::uint32_t title_id{0};
  
  std::unique_ptr<BaseGameContent> base{};
  std::unique_ptr<TitleUpdateContent> selected_title_update{};
  std::vector<std::unique_ptr<TitleUpdateContent>> available_title_updates{};
  std::vector<std::unique_ptr<DLCContent>> installed_dlc{};
  std::vector<std::unique_ptr<SaveDataContent>> saves{};
  std::vector<std::unique_ptr<WritableStorageContent>> writable_storage{};
  
  [[nodiscard]] bool has_base() const noexcept {
    return base && base->is_available();
  }
  
  [[nodiscard]] bool has_title_update() const noexcept {
    return selected_title_update && selected_title_update->is_available();
  }
  
  [[nodiscard]] std::size_t dlc_count() const noexcept {
    return installed_dlc.size();
  }
  
  [[nodiscard]] std::size_t save_count() const noexcept {
    return saves.size();
  }
};

[[nodiscard]] std::string_view to_string(ContentNodeType type) noexcept;
[[nodiscard]] std::string_view to_string(ContentStatus status) noexcept;

}  // namespace xenon::xam

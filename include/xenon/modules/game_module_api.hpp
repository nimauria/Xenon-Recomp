#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "xenon/xam/dlc_manager.hpp"

namespace xenon::modules {

// Game Module API for integrating with Content Services V1
// Game modules implement this interface to provide DLC manifests and content configuration

struct GameModuleInfo {
  std::string module_id{};           // Unique module identifier
  std::string display_name{};        // Human-readable name
  std::uint32_t title_id{0};         // Xbox title ID
  std::string version{};             // Module version
  std::vector<std::uint32_t> supported_media_ids{};  // Supported media IDs
};

// DLC manifest provider interface
class IDLCManifestProvider {
 public:
  virtual ~IDLCManifestProvider() = default;
  
  // Get DLC manifest for this game
  [[nodiscard]] virtual std::vector<xam::DLCManifestEntry> get_dlc_manifest() const = 0;
  
  // Get expected DLC count (for UI display)
  [[nodiscard]] virtual std::size_t expected_dlc_count() const { return 0; }
};

// Game module interface
class IGameModule : public IDLCManifestProvider {
 public:
  ~IGameModule() override = default;
  
  // Get module information
  [[nodiscard]] virtual GameModuleInfo get_module_info() const = 0;
  
  // Initialize module (called once at startup)
  virtual bool initialize() { return true; }
  
  // Shutdown module
  virtual void shutdown() {}
};

// Example DLC manifest entry helper
inline xam::DLCManifestEntry make_dlc_entry(
    const std::string& content_id_hex,
    const std::string& display_name,
    const std::string& description,
    std::uint32_t title_id,
    bool required = false) {
  
  xam::DLCManifestEntry entry{};
  entry.content_id_hex = content_id_hex;
  entry.display_name = display_name;
  entry.description = description;
  entry.title_id = title_id;
  entry.required = required;
  
  // Convert hex string to bytes
  entry.content_id.reserve(20);
  for (std::size_t i = 0; i < content_id_hex.length() && i < 40; i += 2) {
    std::uint8_t byte = 0;
    auto hex_char = [](char c) -> std::uint8_t {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      return 0;
    };
    byte = (hex_char(content_id_hex[i]) << 4) | hex_char(content_id_hex[i + 1]);
    entry.content_id.push_back(byte);
  }
  
  return entry;
}

}  // namespace xenon::modules

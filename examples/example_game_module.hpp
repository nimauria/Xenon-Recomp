#pragma once

#include "xenon/modules/game_module_api.hpp"

namespace examples {

// Example game module showing how to integrate with Content Services V1
// This demonstrates DLC manifest registration for a hypothetical game

class ExampleGameModule : public xenon::modules::IGameModule {
 public:
  ExampleGameModule() = default;
  ~ExampleGameModule() override = default;

  [[nodiscard]] xenon::modules::GameModuleInfo get_module_info() const override {
    xenon::modules::GameModuleInfo info{};
    info.module_id = "com.example.testgame";
    info.display_name = "Example Test Game";
    info.title_id = 0x12345678;  // Replace with actual title ID
    info.version = "1.0.0";
    info.supported_media_ids = {0xABCDEF01, 0xABCDEF02};
    return info;
  }

  [[nodiscard]] std::vector<xenon::xam::DLCManifestEntry> get_dlc_manifest() const override {
    using xenon::modules::make_dlc_entry;
    
    std::vector<xenon::xam::DLCManifestEntry> manifest;
    
    // Example DLC Pack 1
    manifest.push_back(make_dlc_entry(
        "0123456789ABCDEF0123456789ABCDEF01234567",  // 20-byte content ID (40 hex chars)
        "DLC Pack 1: Expansion Content",
        "First major expansion with new missions",
        0x12345678,   // Must match title_id
        false         // not required
    ));
    
    // Example DLC Pack 2
    manifest.push_back(make_dlc_entry(
        "FEDCBA9876543210FEDCBA9876543210FEDCBA98",
        "DLC Pack 2: Bonus Content",
        "Additional weapons and items",
        0x12345678,
        false
    ));
    
    // Example Required DLC (for complete edition)
    manifest.push_back(make_dlc_entry(
        "AABBCCDDEEFF00112233445566778899AABBCCDD",
        "Required Content Pack",
        "Essential content for complete edition",
        0x12345678,
        true  // required
    ));
    
    return manifest;
  }

  [[nodiscard]] std::size_t expected_dlc_count() const override {
    return 3;  // We expect 3 DLC packages
  }

  bool initialize() override {
    // Perform any module-specific initialization
    // E.g., load configuration, validate resources, etc.
    return true;
  }

  void shutdown() override {
    // Clean up module resources
  }
};

// Factory function for creating the module
inline std::unique_ptr<xenon::modules::IGameModule> create_example_module() {
  return std::make_unique<ExampleGameModule>();
}

}  // namespace examples

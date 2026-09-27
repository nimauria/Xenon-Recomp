#include <cassert>
#include <filesystem>
#include <iostream>
#include <memory>

#include "xenon/xam/content_graph.hpp"
#include "xenon/xam/title_update_manager.hpp"
#include "xenon/xam/dlc_manager.hpp"
#include "xenon/xam/save_manager.hpp"

using namespace xenon::xam;

void test_content_graph() {
  std::cout << "Testing Content Graph..." << std::endl;
  
  // Create content graph
  ContentGraph graph;
  graph.title_id = 0x12345678;
  graph.title_id_hex = "12345678";
  
  // Add base game
  auto base = std::make_unique<BaseGameContent>();
  base->id = "base_12345678";
  base->display_name = "Test Game";
  base->title_id = 0x12345678;
  base->status = ContentStatus::Available;
  graph.base = std::move(base);
  
  assert(graph.has_base());
  assert(!graph.has_title_update());
  assert(graph.dlc_count() == 0);
  
  std::cout << "✓ Content Graph basic tests passed" << std::endl;
}

void test_title_update_manager() {
  std::cout << "Testing Title Update Manager..." << std::endl;
  
  TitleUpdateManager tu_manager;
  tu_manager.initialize();
  
  // Test version comparison
  filesystem::XexVersion v1{.major = 1, .minor = 2, .build = 0, .qfe = 0};
  filesystem::XexVersion v2{.major = 1, .minor = 3, .build = 0, .qfe = 0};
  
  assert(TitleUpdateManager::is_newer_version(v2, v1));
  assert(!TitleUpdateManager::is_newer_version(v1, v2));
  
  std::cout << "✓ Title Update Manager basic tests passed" << std::endl;
}

void test_dlc_manager() {
  std::cout << "Testing DLC Manager..." << std::endl;
  
  DLCManager dlc_manager;
  dlc_manager.initialize();
  
  // Register DLC manifest
  std::vector<DLCManifestEntry> manifest;
  DLCManifestEntry entry{};
  entry.content_id = std::vector<std::uint8_t>(20, 0xAB);
  entry.content_id_hex = "ABABABABABABABABABABABABABABABABABABABAB";
  entry.display_name = "Test DLC";
  entry.title_id = 0x12345678;
  manifest.push_back(entry);
  
  dlc_manager.register_dlc_manifest(0x12345678, manifest);
  
  // Verify manifest
  auto entries = dlc_manager.get_manifest_entries(0x12345678);
  assert(entries.size() == 1);
  assert(entries[0].display_name == "Test DLC");
  
  std::cout << "✓ DLC Manager basic tests passed" << std::endl;
}

void test_save_manager() {
  std::cout << "Testing Save Manager..." << std::endl;
  
  // Use a temporary directory for testing
  std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "xenon_test_saves";
  std::filesystem::create_directories(temp_dir);
  
  SaveManager save_manager;
  save_manager.initialize(temp_dir);
  
  // Create save container
  SaveContainerConfig config{};
  config.profile_xuid = 0x0123456789ABCDEF;
  config.title_id = 0x12345678;
  config.save_name = "TestSave";
  config.enable_backup = true;
  config.max_backups = 3;
  
  std::string container_id;
  auto result = save_manager.create_save_container(config, container_id);
  assert(result == result::Success);
  assert(!container_id.empty());
  
  // Verify save path exists
  auto save_path = save_manager.get_save_path(
      config.profile_xuid, config.title_id, config.save_name);
  assert(std::filesystem::exists(save_path));
  
  // Cleanup
  std::filesystem::remove_all(temp_dir);
  
  std::cout << "✓ Save Manager basic tests passed" << std::endl;
}

void test_content_node_strings() {
  std::cout << "Testing Content Node String Conversions..." << std::endl;
  
  assert(to_string(ContentNodeType::BaseGame) == "BaseGame");
  assert(to_string(ContentNodeType::TitleUpdate) == "TitleUpdate");
  assert(to_string(ContentNodeType::DLC) == "DLC");
  assert(to_string(ContentNodeType::SaveData) == "SaveData");
  
  assert(to_string(ContentStatus::Available) == "Available");
  assert(to_string(ContentStatus::Mounted) == "Mounted");
  assert(to_string(ContentStatus::Invalid) == "Invalid");
  
  std::cout << "✓ Content Node string tests passed" << std::endl;
}

int main() {
  std::cout << "=== Content Services V1 Tests ===" << std::endl << std::endl;
  
  try {
    test_content_graph();
    test_title_update_manager();
    test_dlc_manager();
    test_save_manager();
    test_content_node_strings();
    
    std::cout << std::endl << "=== All Content Services Tests Passed ===" << std::endl;
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Test failed with exception: " << e.what() << std::endl;
    return 1;
  }
}

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xenon/xam/content_graph.hpp"
#include "xenon/xam/types.hpp"

namespace xenon::xam {

// Title update validation result
struct TitleUpdateValidation {
  bool is_valid{false};
  bool title_id_matches{false};
  bool media_id_compatible{false};
  bool version_compatible{false};
  bool base_executable_supported{false};
  std::string error_message{};
};

// Title update manager for automatic identification and validation
class TitleUpdateManager {
 public:
  TitleUpdateManager() = default;
  ~TitleUpdateManager() = default;

  TitleUpdateManager(const TitleUpdateManager&) = delete;
  TitleUpdateManager& operator=(const TitleUpdateManager&) = delete;

  // Initialize the manager
  void initialize();

  // Identify title updates in a directory
  [[nodiscard]] std::vector<std::unique_ptr<TitleUpdateContent>>
  discover_title_updates(const std::filesystem::path& search_path,
                        std::uint32_t title_id) const;

  // Validate a title update against base game
  [[nodiscard]] TitleUpdateValidation validate_title_update(
      const TitleUpdateContent& title_update,
      const BaseGameContent& base_game) const;

  // Select the best compatible title update
  [[nodiscard]] std::unique_ptr<TitleUpdateContent> select_best_title_update(
      const std::vector<std::unique_ptr<TitleUpdateContent>>& candidates,
      const BaseGameContent& base_game) const;

  // Load title update metadata from a file
  [[nodiscard]] std::optional<TitleUpdateContent> load_title_update(
      const std::filesystem::path& path,
      std::uint32_t expected_title_id = 0) const;

  // Compare title update versions
  [[nodiscard]] static bool is_newer_version(
      const filesystem::XexVersion& a,
      const filesystem::XexVersion& b) noexcept;

 private:
  bool initialized_{false};
};

}  // namespace xenon::xam

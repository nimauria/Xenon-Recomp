#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "xenon/xam/types.hpp"

namespace xenon::xam {

// Achievement data
struct Achievement {
  std::uint32_t id{0};
  std::string name{};
  std::string description{};
  std::uint32_t gamerscore{0};
  bool unlocked{false};
  std::uint64_t unlock_time{0};  // Unix timestamp
};

// User statistics
struct UserStat {
  std::uint32_t id{0};
  std::string name{};
  std::int64_t value{0};
};

// Per-title achievement tracking
struct TitleAchievements {
  std::uint64_t title_id{0};
  std::map<std::uint32_t, Achievement> achievements{};
  std::map<std::uint32_t, UserStat> stats{};
};

// Manages achievements and statistics (offline only)
class AchievementManager {
 public:
  AchievementManager();
  ~AchievementManager() = default;

  AchievementManager(const AchievementManager&) = delete;
  AchievementManager& operator=(const AchievementManager&) = delete;

  void initialize();

  // Achievement operations
  [[nodiscard]] XResult unlock_achievement(
      std::uint32_t user_index,
      std::uint64_t title_id,
      std::uint32_t achievement_id);

  [[nodiscard]] std::optional<Achievement> get_achievement(
      std::uint32_t user_index,
      std::uint64_t title_id,
      std::uint32_t achievement_id) const;

  [[nodiscard]] std::vector<Achievement> enumerate_achievements(
      std::uint32_t user_index,
      std::uint64_t title_id) const;

  // Statistics operations
  [[nodiscard]] XResult write_stat(
      std::uint32_t user_index,
      std::uint64_t title_id,
      std::uint32_t stat_id,
      std::int64_t value);

  [[nodiscard]] std::optional<UserStat> read_stat(
      std::uint32_t user_index,
      std::uint64_t title_id,
      std::uint32_t stat_id) const;

  [[nodiscard]] std::vector<UserStat> enumerate_stats(
      std::uint32_t user_index,
      std::uint64_t title_id) const;

 private:
  // Map: user_index -> title_id -> TitleAchievements
  std::map<std::uint32_t, std::map<std::uint64_t, TitleAchievements>> user_data_{};
  bool initialized_{false};

  [[nodiscard]] TitleAchievements& get_or_create_title_data(
      std::uint32_t user_index,
      std::uint64_t title_id);
};

}  // namespace xenon::xam

#include "xenon/xam/achievement_manager.hpp"

#include <chrono>
#include <iostream>

namespace xenon::xam {

AchievementManager::AchievementManager() = default;

void AchievementManager::initialize() {
  if (initialized_) return;
  initialized_ = true;
}

TitleAchievements& AchievementManager::get_or_create_title_data(
    std::uint32_t user_index,
    std::uint64_t title_id) {
  
  auto& user_titles = user_data_[user_index];
  auto it = user_titles.find(title_id);
  
  if (it == user_titles.end()) {
    TitleAchievements title_data{};
    title_data.title_id = title_id;
    user_titles[title_id] = title_data;
    return user_titles[title_id];
  }
  
  return it->second;
}

XResult AchievementManager::unlock_achievement(
    std::uint32_t user_index,
    std::uint64_t title_id,
    std::uint32_t achievement_id) {
  
  auto& title_data = get_or_create_title_data(user_index, title_id);
  auto it = title_data.achievements.find(achievement_id);
  
  if (it == title_data.achievements.end()) {
    // Create new achievement if it doesn't exist
    Achievement ach{};
    ach.id = achievement_id;
    ach.name = "Achievement " + std::to_string(achievement_id);
    ach.description = "Unlocked achievement";
    ach.gamerscore = 10;  // Default
    ach.unlocked = true;
    
    auto now = std::chrono::system_clock::now();
    ach.unlock_time = std::chrono::system_clock::to_time_t(now);
    
    title_data.achievements[achievement_id] = ach;
    
    std::cout << "[XAM Achievement] User " << user_index
              << " unlocked achievement " << achievement_id
              << " in title " << std::hex << title_id << std::dec
              << std::endl;
  } else {
    // Mark existing as unlocked
    if (!it->second.unlocked) {
      it->second.unlocked = true;
      auto now = std::chrono::system_clock::now();
      it->second.unlock_time = std::chrono::system_clock::to_time_t(now);
      
      std::cout << "[XAM Achievement] User " << user_index
                << " unlocked achievement \"" << it->second.name << "\""
                << " (" << it->second.gamerscore << "G)"
                << std::endl;
    }
  }
  
  return result::Success;
}

std::optional<Achievement> AchievementManager::get_achievement(
    std::uint32_t user_index,
    std::uint64_t title_id,
    std::uint32_t achievement_id) const {
  
  auto user_it = user_data_.find(user_index);
  if (user_it == user_data_.end()) {
    return std::nullopt;
  }
  
  auto title_it = user_it->second.find(title_id);
  if (title_it == user_it->second.end()) {
    return std::nullopt;
  }
  
  auto ach_it = title_it->second.achievements.find(achievement_id);
  if (ach_it == title_it->second.achievements.end()) {
    return std::nullopt;
  }
  
  return ach_it->second;
}

std::vector<Achievement> AchievementManager::enumerate_achievements(
    std::uint32_t user_index,
    std::uint64_t title_id) const {
  
  std::vector<Achievement> result;
  
  auto user_it = user_data_.find(user_index);
  if (user_it == user_data_.end()) {
    return result;
  }
  
  auto title_it = user_it->second.find(title_id);
  if (title_it == user_it->second.end()) {
    return result;
  }
  
  result.reserve(title_it->second.achievements.size());
  for (const auto& [id, ach] : title_it->second.achievements) {
    result.push_back(ach);
  }
  
  return result;
}

XResult AchievementManager::write_stat(
    std::uint32_t user_index,
    std::uint64_t title_id,
    std::uint32_t stat_id,
    std::int64_t value) {
  
  auto& title_data = get_or_create_title_data(user_index, title_id);
  
  UserStat stat{};
  stat.id = stat_id;
  stat.name = "Stat " + std::to_string(stat_id);
  stat.value = value;
  
  title_data.stats[stat_id] = stat;
  
  return result::Success;
}

std::optional<UserStat> AchievementManager::read_stat(
    std::uint32_t user_index,
    std::uint64_t title_id,
    std::uint32_t stat_id) const {
  
  auto user_it = user_data_.find(user_index);
  if (user_it == user_data_.end()) {
    return std::nullopt;
  }
  
  auto title_it = user_it->second.find(title_id);
  if (title_it == user_it->second.end()) {
    return std::nullopt;
  }
  
  auto stat_it = title_it->second.stats.find(stat_id);
  if (stat_it == title_it->second.stats.end()) {
    return std::nullopt;
  }
  
  return stat_it->second;
}

std::vector<UserStat> AchievementManager::enumerate_stats(
    std::uint32_t user_index,
    std::uint64_t title_id) const {
  
  std::vector<UserStat> result;
  
  auto user_it = user_data_.find(user_index);
  if (user_it == user_data_.end()) {
    return result;
  }
  
  auto title_it = user_it->second.find(title_id);
  if (title_it == user_it->second.end()) {
    return result;
  }
  
  result.reserve(title_it->second.stats.size());
  for (const auto& [id, stat] : title_it->second.stats) {
    result.push_back(stat);
  }
  
  return result;
}

}  // namespace xenon::xam

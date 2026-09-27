#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "xenon/xam/types.hpp"

namespace xenon::xam {

// User profile data
struct UserProfile {
  XUID xuid{};
  std::string gamertag{};
  SigninState signin_state{SigninState::NotSignedIn};
  std::uint32_t flags{};
  LanguageId language{LanguageId::English};
  bool is_guest{false};
};

// Manages Xbox 360 user accounts and sign-in state
// Provides a coherent offline local user by default
class UserManager {
 public:
  static constexpr std::uint32_t kMaxUsers = 4;
  
  // Default offline XUID for local user
  static constexpr XUID kDefaultOfflineXuid = 0xE000000000000001ull;

  UserManager();
  ~UserManager() = default;

  UserManager(const UserManager&) = delete;
  UserManager& operator=(const UserManager&) = delete;

  // Initialize with default offline user
  void initialize();

  // User queries
  [[nodiscard]] bool is_signed_in(std::uint32_t user_index) const;
  [[nodiscard]] SigninState signin_state(std::uint32_t user_index) const;
  [[nodiscard]] XUID xuid(std::uint32_t user_index) const;
  [[nodiscard]] std::string gamertag(std::uint32_t user_index) const;
  [[nodiscard]] std::uint32_t user_flags(std::uint32_t user_index) const;
  [[nodiscard]] LanguageId language(std::uint32_t user_index) const;

  // Profile management
  [[nodiscard]] const UserProfile* profile(std::uint32_t user_index) const;
  [[nodiscard]] std::optional<std::uint32_t> user_index_for_xuid(XUID xuid) const;

  // Sign-in / sign-out (for future implementation)
  [[nodiscard]] XResult sign_in_user(std::uint32_t user_index, 
                                      const std::string& gamertag,
                                      bool as_guest = false);
  [[nodiscard]] XResult sign_out_user(std::uint32_t user_index);

 private:
  [[nodiscard]] bool is_valid_user_index(std::uint32_t index) const;

  std::array<std::optional<UserProfile>, kMaxUsers> users_{};
  bool initialized_{false};
};

}  // namespace xenon::xam

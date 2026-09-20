#include "xenon/xam/user_manager.hpp"

namespace xenon::xam {

UserManager::UserManager() = default;

void UserManager::initialize() {
  if (initialized_) return;

  // Create default offline user at slot 0
  UserProfile default_user{};
  default_user.xuid = kDefaultOfflineXuid;
  default_user.gamertag = "XenonPlayer";
  default_user.signin_state = SigninState::SignedInLocally;
  default_user.flags = 0;  // No special flags for default user
  default_user.language = LanguageId::English;
  default_user.is_guest = false;

  users_[0] = default_user;
  initialized_ = true;
}

bool UserManager::is_valid_user_index(std::uint32_t index) const {
  return index < kMaxUsers;
}

bool UserManager::is_signed_in(std::uint32_t user_index) const {
  if (!is_valid_user_index(user_index)) return false;
  const auto& user = users_[user_index];
  return user.has_value() && user->signin_state != SigninState::NotSignedIn;
}

SigninState UserManager::signin_state(std::uint32_t user_index) const {
  if (!is_valid_user_index(user_index)) return SigninState::NotSignedIn;
  const auto& user = users_[user_index];
  return user.has_value() ? user->signin_state : SigninState::NotSignedIn;
}

XUID UserManager::xuid(std::uint32_t user_index) const {
  if (!is_valid_user_index(user_index)) return 0;
  const auto& user = users_[user_index];
  return user.has_value() ? user->xuid : 0;
}

std::string UserManager::gamertag(std::uint32_t user_index) const {
  if (!is_valid_user_index(user_index)) return {};
  const auto& user = users_[user_index];
  return user.has_value() ? user->gamertag : std::string{};
}

std::uint32_t UserManager::user_flags(std::uint32_t user_index) const {
  if (!is_valid_user_index(user_index)) return 0;
  const auto& user = users_[user_index];
  return user.has_value() ? user->flags : 0;
}

LanguageId UserManager::language(std::uint32_t user_index) const {
  if (!is_valid_user_index(user_index)) return LanguageId::Invalid;
  const auto& user = users_[user_index];
  return user.has_value() ? user->language : LanguageId::Invalid;
}

const UserProfile* UserManager::profile(std::uint32_t user_index) const {
  if (!is_valid_user_index(user_index)) return nullptr;
  const auto& user = users_[user_index];
  return user.has_value() ? &(*user) : nullptr;
}

std::optional<std::uint32_t> UserManager::user_index_for_xuid(XUID target_xuid) const {
  for (std::uint32_t i = 0; i < kMaxUsers; ++i) {
    if (users_[i].has_value() && users_[i]->xuid == target_xuid) {
      return i;
    }
  }
  return std::nullopt;
}

XResult UserManager::sign_in_user(std::uint32_t user_index,
                                   const std::string& gamertag,
                                   bool as_guest) {
  if (!is_valid_user_index(user_index)) {
    return result::InvalidParameter;
  }

  UserProfile new_user{};
  new_user.xuid = kDefaultOfflineXuid + user_index + 1;  // Generate unique XUID
  new_user.gamertag = gamertag.empty() ? "XenonPlayer" : gamertag;
  new_user.signin_state = SigninState::SignedInLocally;
  new_user.flags = as_guest ? user_flag::Guest : 0;
  new_user.language = LanguageId::English;
  new_user.is_guest = as_guest;

  users_[user_index] = new_user;
  return result::Success;
}

XResult UserManager::sign_out_user(std::uint32_t user_index) {
  if (!is_valid_user_index(user_index)) {
    return result::InvalidParameter;
  }

  users_[user_index].reset();
  return result::Success;
}

}  // namespace xenon::xam

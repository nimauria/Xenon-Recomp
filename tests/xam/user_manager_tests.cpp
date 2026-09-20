#include "xenon/xam/user_manager.hpp"

#include <cassert>
#include <iostream>

using namespace xenon::xam;

void test_default_user_initialization() {
  std::cout << "[TEST] Default user initialization..." << std::endl;
  
  UserManager manager;
  manager.initialize();
  
  // Check user 0 is signed in
  assert(manager.is_signed_in(0));
  assert(manager.signin_state(0) == SigninState::SignedInLocally);
  
  // Check XUID
  assert(manager.xuid(0) == UserManager::kDefaultOfflineXuid);
  
  // Check gamertag
  assert(manager.gamertag(0) == "XenonPlayer");
  
  // Check language
  assert(manager.language(0) == LanguageId::English);
  
  // Check profile access
  const auto* profile = manager.profile(0);
  assert(profile != nullptr);
  assert(profile->xuid == UserManager::kDefaultOfflineXuid);
  assert(profile->gamertag == "XenonPlayer");
  assert(!profile->is_guest);
  
  // Check other slots are not signed in
  assert(!manager.is_signed_in(1));
  assert(!manager.is_signed_in(2));
  assert(!manager.is_signed_in(3));
  
  std::cout << "  ✓ Default user initialized correctly" << std::endl;
}

void test_user_index_for_xuid() {
  std::cout << "[TEST] User index lookup by XUID..." << std::endl;
  
  UserManager manager;
  manager.initialize();
  
  // Find default user by XUID
  auto index = manager.user_index_for_xuid(UserManager::kDefaultOfflineXuid);
  assert(index.has_value());
  assert(index.value() == 0);
  
  // Invalid XUID
  auto invalid = manager.user_index_for_xuid(0x1234567890ABCDEFull);
  assert(!invalid.has_value());
  
  std::cout << "  ✓ XUID lookup works correctly" << std::endl;
}

void test_invalid_user_index() {
  std::cout << "[TEST] Invalid user index handling..." << std::endl;
  
  UserManager manager;
  manager.initialize();
  
  // Test with invalid indices
  assert(!manager.is_signed_in(4));
  assert(!manager.is_signed_in(100));
  assert(manager.xuid(4) == 0);
  assert(manager.gamertag(5).empty());
  assert(manager.profile(10) == nullptr);
  
  std::cout << "  ✓ Invalid indices handled correctly" << std::endl;
}

void test_sign_in_additional_users() {
  std::cout << "[TEST] Sign in additional users..." << std::endl;
  
  UserManager manager;
  manager.initialize();
  
  // Sign in user 1
  auto result = manager.sign_in_user(1, "Player2", false);
  assert(result == result::Success);
  assert(manager.is_signed_in(1));
  assert(manager.gamertag(1) == "Player2");
  assert(manager.signin_state(1) == SigninState::SignedInLocally);
  
  // Sign in user 2 as guest
  result = manager.sign_in_user(2, "Guest", true);
  assert(result == result::Success);
  assert(manager.is_signed_in(2));
  assert(manager.gamertag(2) == "Guest");
  const auto* profile = manager.profile(2);
  assert(profile != nullptr);
  assert(profile->is_guest);
  assert((profile->flags & user_flag::Guest) != 0);
  
  std::cout << "  ✓ Additional users signed in correctly" << std::endl;
}

void test_sign_out_user() {
  std::cout << "[TEST] Sign out user..." << std::endl;
  
  UserManager manager;
  manager.initialize();
  
  // Default user should be signed in
  assert(manager.is_signed_in(0));
  
  // Sign out user 0
  auto result = manager.sign_out_user(0);
  assert(result == result::Success);
  assert(!manager.is_signed_in(0));
  assert(manager.profile(0) == nullptr);
  
  std::cout << "  ✓ User signed out correctly" << std::endl;
}

int main() {
  std::cout << "\n=== XAM User Manager Tests ===" << std::endl;
  
  test_default_user_initialization();
  test_user_index_for_xuid();
  test_invalid_user_index();
  test_sign_in_additional_users();
  test_sign_out_user();
  
  std::cout << "\n✅ All tests passed!" << std::endl;
  return 0;
}

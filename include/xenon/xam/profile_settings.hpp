#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <tuple>
#include <vector>

#include "xenon/xam/types.hpp"

namespace xenon::xam {

// A profile setting id encodes its own type and maximum size:
// bits 0-13 id, 14-15 unknown, 16-27 size in bytes, 28-31 type.
// XPROFILE_TITLE_SPECIFIC1 (0x63E83FFF) is BINARY, at most 1000 bytes.
enum class ProfileSettingType : std::uint8_t {
  Content = 0,
  Int32 = 1,
  Int64 = 2,
  Double = 3,
  WString = 4,
  Float = 5,
  Binary = 6,
  DateTime = 7,
  Unset = 0xFF,
};

[[nodiscard]] constexpr ProfileSettingType profile_setting_type(std::uint32_t setting_id) noexcept {
  return static_cast<ProfileSettingType>((setting_id >> 28) & 0xFu);
}
[[nodiscard]] constexpr std::uint32_t profile_setting_max_size(std::uint32_t setting_id) noexcept {
  return (setting_id >> 16) & 0xFFFu;
}
// Title-specific settings (ids 0x3F00-0x3FFF) belong to one title and are
// written by it; the others belong to the profile.
[[nodiscard]] constexpr bool profile_setting_is_title_specific(std::uint32_t setting_id) noexcept {
  return (setting_id & 0x3F00u) == 0x3F00u;
}

// One setting value. Scalar types keep their value in `bits` (the raw
// 32- or 64-bit pattern); WSTRING keeps big-endian UTF-16 including the
// terminator in `bytes`, BINARY and CONTENT keep their bytes.
struct ProfileSettingValue {
  ProfileSettingType type{ProfileSettingType::Unset};
  std::uint64_t bits{0};
  std::vector<std::uint8_t> bytes{};
};

// Profile settings for every signed-in profile (XUserReadProfileSettings,
// XUserWriteProfileSettings). Profile settings start at the defaults below;
// title-specific settings start unset and, once a title writes them, are
// kept per (profile, title) and saved under the storage directory, if one
// is set, as <dir>/<XUID>/<title id>/<setting id>, so they survive a
// restart as on hardware. Defaults are the ones xenia and rexglue-sdk give
// a new profile (py360 constants), not values read from a real console.
class ProfileSettings {
 public:
  ProfileSettings();

  // Where title-specific settings are saved. Empty keeps them in memory.
  void set_storage_directory(std::filesystem::path directory);

  // The setting's value for `xuid` in `title_id`, or nothing when it has no
  // value (XSOURCE_NO_VALUE). Title-specific settings are loaded from the
  // storage directory on first use.
  [[nodiscard]] std::optional<ProfileSettingValue> read(XUID xuid, std::uint32_t title_id,
                                                        std::uint32_t setting_id);

  // Stores a value; a title-specific one is saved to the storage directory.
  // Returns false only when saving it failed.
  bool write(XUID xuid, std::uint32_t title_id, std::uint32_t setting_id,
             ProfileSettingValue value);

 private:
  using Key = std::tuple<XUID, std::uint32_t, std::uint32_t>;  // xuid, title (0 for profile), id

  [[nodiscard]] std::filesystem::path path_for(XUID xuid, std::uint32_t title_id,
                                               std::uint32_t setting_id) const;

  std::mutex mutex_;
  std::map<std::uint32_t, ProfileSettingValue> defaults_;
  std::map<Key, ProfileSettingValue> values_;
  std::map<Key, bool> loaded_;
  std::filesystem::path storage_directory_;
};

}  // namespace xenon::xam

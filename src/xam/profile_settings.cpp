#include "xenon/xam/profile_settings.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace xenon::xam {
namespace {

ProfileSettingValue int32_value(std::int32_t value) {
  return {ProfileSettingType::Int32, static_cast<std::uint32_t>(value), {}};
}

ProfileSettingValue empty_wstring() { return {ProfileSettingType::WString, 0u, {}}; }

bool is_scalar(ProfileSettingType type) {
  return type != ProfileSettingType::WString && type != ProfileSettingType::Binary &&
         type != ProfileSettingType::Content;
}

std::string hex(std::uint64_t value, int digits) {
  char text[17]{};
  std::snprintf(text, sizeof(text), "%0*llX", digits, static_cast<unsigned long long>(value));
  return text;
}

}  // namespace

ProfileSettings::ProfileSettings() {
  // The profile defaults xenia and rexglue-sdk use (ids from py360's
  // constants.py). XPROFILE_GAMERCARD_PICTURE_KEY is left without a value:
  // Xenon has no gamer picture to point it at.
  defaults_[0x10040002u] = int32_value(0);            // XPROFILE_GAMER_YAXIS_INVERSION
  defaults_[0x10040003u] = int32_value(3);            // XPROFILE_OPTION_CONTROLLER_VIBRATION
  defaults_[0x10040004u] = int32_value(0);            // XPROFILE_GAMERCARD_ZONE
  defaults_[0x10040005u] = int32_value(0);            // XPROFILE_GAMERCARD_REGION
  defaults_[0x10040006u] = int32_value(0xFA);         // XPROFILE_GAMERCARD_CRED
  defaults_[0x5004000Bu] = {ProfileSettingType::Float, 0u, {}};  // XPROFILE_GAMERCARD_REP
  defaults_[0x1004000Cu] = int32_value(0);            // XPROFILE_OPTION_VOICE_MUTED
  defaults_[0x1004000Du] = int32_value(0);            // XPROFILE_OPTION_VOICE_THRU_SPEAKERS
  defaults_[0x1004000Eu] = int32_value(0x64);         // XPROFILE_OPTION_VOICE_VOLUME
  defaults_[0x402C0011u] = empty_wstring();           // XPROFILE_GAMERCARD_MOTTO
  defaults_[0x10040012u] = int32_value(1);            // XPROFILE_GAMERCARD_TITLES_PLAYED
  defaults_[0x10040013u] = int32_value(0);            // XPROFILE_GAMERCARD_ACHIEVEMENTS_EARNED
  defaults_[0x10040015u] = int32_value(0);            // XPROFILE_GAMER_DIFFICULTY
  defaults_[0x10040018u] = int32_value(0);            // XPROFILE_GAMER_CONTROL_SENSITIVITY
  defaults_[0x1004001Du] = int32_value(static_cast<std::int32_t>(0xFFFF0000u));  // preferred color 1
  defaults_[0x1004001Eu] = int32_value(static_cast<std::int32_t>(0xFF00FF00u));  // preferred color 2
  defaults_[0x10040022u] = int32_value(1);            // XPROFILE_GAMER_ACTION_AUTO_AIM
  defaults_[0x10040023u] = int32_value(0);            // XPROFILE_GAMER_ACTION_AUTO_CENTER
  defaults_[0x10040024u] = int32_value(0);            // XPROFILE_GAMER_ACTION_MOVEMENT_CONTROL
  defaults_[0x10040026u] = int32_value(0);            // XPROFILE_GAMER_RACE_TRANSMISSION
  defaults_[0x10040027u] = int32_value(0);            // XPROFILE_GAMER_RACE_CAMERA_LOCATION
  defaults_[0x10040028u] = int32_value(0);            // XPROFILE_GAMER_RACE_BRAKE_CONTROL
  defaults_[0x10040029u] = int32_value(0);            // XPROFILE_GAMER_RACE_ACCELERATOR_CONTROL
  defaults_[0x10040038u] = int32_value(0);            // XPROFILE_GAMERCARD_TITLE_CRED_EARNED
  defaults_[0x10040039u] = int32_value(0);            // XPROFILE_GAMERCARD_TITLE_ACHIEVEMENTS_EARNED
}

void ProfileSettings::set_storage_directory(std::filesystem::path directory) {
  std::scoped_lock lock(mutex_);
  storage_directory_ = std::move(directory);
  loaded_.clear();
}

std::filesystem::path ProfileSettings::path_for(XUID xuid, std::uint32_t title_id,
                                                std::uint32_t setting_id) const {
  return storage_directory_ / hex(xuid, 16) / hex(title_id, 8) / hex(setting_id, 8);
}

std::optional<ProfileSettingValue> ProfileSettings::read(XUID xuid, std::uint32_t title_id,
                                                         std::uint32_t setting_id) {
  std::scoped_lock lock(mutex_);
  const bool title_specific = profile_setting_is_title_specific(setting_id);
  const Key key{xuid, title_specific ? title_id : 0u, setting_id};
  if (title_specific && !storage_directory_.empty() && !loaded_[key]) {
    loaded_[key] = true;
    std::ifstream file(path_for(xuid, title_id, setting_id), std::ios::binary);
    std::vector<std::uint8_t> data;
    if (file) data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (!data.empty() && values_.find(key) == values_.end()) {
      // Saved as a type byte, then 8 big-endian bytes for a scalar or the
      // value's bytes otherwise.
      ProfileSettingValue value{static_cast<ProfileSettingType>(data[0]), 0u, {}};
      if (is_scalar(value.type)) {
        if (data.size() == 9u) {
          for (std::size_t i = 1; i < 9u; ++i) value.bits = (value.bits << 8u) | data[i];
          values_[key] = std::move(value);
        }
      } else {
        value.bytes.assign(data.begin() + 1, data.end());
        values_[key] = std::move(value);
      }
    }
  }
  if (const auto it = values_.find(key); it != values_.end()) return it->second;
  if (!title_specific) {
    if (const auto it = defaults_.find(setting_id); it != defaults_.end()) return it->second;
  }
  return std::nullopt;
}

bool ProfileSettings::write(XUID xuid, std::uint32_t title_id, std::uint32_t setting_id,
                            ProfileSettingValue value) {
  std::scoped_lock lock(mutex_);
  const bool title_specific = profile_setting_is_title_specific(setting_id);
  const Key key{xuid, title_specific ? title_id : 0u, setting_id};
  loaded_[key] = true;
  bool saved = true;
  if (title_specific && !storage_directory_.empty()) {
    const auto path = path_for(xuid, title_id, setting_id);
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::vector<std::uint8_t> data{static_cast<std::uint8_t>(value.type)};
    if (is_scalar(value.type)) {
      for (int shift = 56; shift >= 0; shift -= 8) {
        data.push_back(static_cast<std::uint8_t>(value.bits >> shift));
      }
    } else {
      data.insert(data.end(), value.bytes.begin(), value.bytes.end());
    }
    const auto temporary = path.string() + ".tmp";
    {
      std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
      file.write(reinterpret_cast<const char*>(data.data()),
                 static_cast<std::streamsize>(data.size()));
      saved = static_cast<bool>(file);
    }
    if (saved) {
      std::filesystem::rename(temporary, path, error);
      saved = !error;
    }
  }
  values_[key] = std::move(value);
  return saved;
}

}  // namespace xenon::xam

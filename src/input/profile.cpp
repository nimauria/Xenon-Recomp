#include "xenon/input/profile.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

namespace xenon::input {
namespace {

float clamp01(float value) { return std::clamp(value, 0.0f, 1.0f); }

bool finite_profile_value(float value) {
  return std::isfinite(value) && value >= 0.0f;
}

float normalize_axis(std::int16_t value, const AxisCalibration& calibration) {
  const auto v = static_cast<int>(value);
  const auto c = static_cast<int>(calibration.center);
  if (v >= c) {
    const auto span = static_cast<int>(calibration.maximum) - c;
    return span > 0 ? clamp01(static_cast<float>(v - c) / span) : 0.0f;
  }
  const auto span = c - static_cast<int>(calibration.minimum);
  return span > 0 ? -clamp01(static_cast<float>(c - v) / span) : 0.0f;
}

std::int16_t denormalize_axis(float value) {
  value = std::clamp(value, -1.0f, 1.0f);
  if (value >= 0.0f) {
    return static_cast<std::int16_t>(std::lround(value * 32767.0f));
  }
  return static_cast<std::int16_t>(std::lround(value * 32768.0f));
}

void apply_stick(const StickProfile& profile, std::int16_t& x_value,
                 std::int16_t& y_value) {
  auto x = normalize_axis(x_value, profile.x);
  auto y = normalize_axis(y_value, profile.y);
  if (profile.invert_x) x = -x;
  if (profile.invert_y) y = -y;

  auto magnitude = std::sqrt(x * x + y * y);
  if (magnitude <= profile.inner_deadzone || magnitude <= 0.000001f) {
    x_value = 0;
    y_value = 0;
    return;
  }
  magnitude = std::min(magnitude, 1.0f);
  const auto usable = std::max(0.0001f,
      1.0f - profile.inner_deadzone - profile.outer_deadzone);
  auto scaled = clamp01((magnitude - profile.inner_deadzone) / usable);
  scaled = std::pow(scaled, profile.response_exponent);
  scaled = clamp01(scaled * profile.sensitivity);
  const auto factor = scaled / magnitude;
  x_value = denormalize_axis(x * factor);
  y_value = denormalize_axis(y * factor);
}

std::uint8_t apply_trigger(const TriggerProfile& profile, std::uint8_t value) {
  const auto min_value = static_cast<float>(profile.calibration.minimum);
  const auto max_value = static_cast<float>(profile.calibration.maximum);
  const auto span = max_value - min_value;
  if (span <= 0.0f) return 0;
  auto normalized = clamp01((static_cast<float>(value) - min_value) / span);
  if (profile.invert) normalized = 1.0f - normalized;
  if (normalized <= profile.inner_deadzone) return 0;
  const auto usable = std::max(0.0001f,
      1.0f - profile.inner_deadzone - profile.outer_deadzone);
  normalized = clamp01((normalized - profile.inner_deadzone) / usable);
  normalized = std::pow(normalized, profile.response_exponent);
  normalized = clamp01(normalized * profile.sensitivity);
  return static_cast<std::uint8_t>(std::lround(normalized * 255.0f));
}


bool identity_axis(const AxisCalibration& axis) noexcept {
  return axis.minimum == -32768 && axis.center == 0 && axis.maximum == 32767;
}

bool identity_stick(const StickProfile& profile) noexcept {
  return identity_axis(profile.x) && identity_axis(profile.y) &&
         profile.inner_deadzone == 0.0f && profile.outer_deadzone == 0.0f &&
         profile.response_exponent == 1.0f && profile.sensitivity == 1.0f &&
         !profile.invert_x && !profile.invert_y;
}

bool identity_trigger(const TriggerProfile& profile) noexcept {
  return profile.calibration.minimum == 0 &&
         profile.calibration.maximum == 255 &&
         profile.inner_deadzone == 0.0f && profile.outer_deadzone == 0.0f &&
         profile.response_exponent == 1.0f && profile.sensitivity == 1.0f &&
         !profile.invert;
}

bool identity_profile(const InputProfile& profile) noexcept {
  return profile.enabled && identity_stick(profile.left_stick) &&
         identity_stick(profile.right_stick) &&
         identity_trigger(profile.left_trigger) &&
         identity_trigger(profile.right_trigger);
}

bool write_stick(std::ostream& out, std::string_view label,
                 const StickProfile& p) {
  out << label << ' ' << p.x.minimum << ' ' << p.x.center << ' ' << p.x.maximum
      << ' ' << p.y.minimum << ' ' << p.y.center << ' ' << p.y.maximum << ' '
      << p.inner_deadzone << ' ' << p.outer_deadzone << ' '
      << p.response_exponent << ' ' << p.sensitivity << ' ' << p.invert_x
      << ' ' << p.invert_y << '\n';
  return static_cast<bool>(out);
}

bool read_stick(std::istream& in, StickProfile& p) {
  int xmin{}, xcenter{}, xmax{}, ymin{}, ycenter{}, ymax{};
  int invert_x{}, invert_y{};
  if (!(in >> xmin >> xcenter >> xmax >> ymin >> ycenter >> ymax >>
        p.inner_deadzone >> p.outer_deadzone >> p.response_exponent >>
        p.sensitivity >> invert_x >> invert_y)) {
    return false;
  }
  p.x = {static_cast<std::int16_t>(xmin), static_cast<std::int16_t>(xcenter),
         static_cast<std::int16_t>(xmax)};
  p.y = {static_cast<std::int16_t>(ymin), static_cast<std::int16_t>(ycenter),
         static_cast<std::int16_t>(ymax)};
  p.invert_x = invert_x != 0;
  p.invert_y = invert_y != 0;
  return true;
}

bool write_trigger(std::ostream& out, std::string_view label,
                   const TriggerProfile& p) {
  out << label << ' ' << unsigned(p.calibration.minimum) << ' '
      << unsigned(p.calibration.maximum) << ' ' << p.inner_deadzone << ' '
      << p.outer_deadzone << ' ' << p.response_exponent << ' ' << p.sensitivity
      << ' ' << p.invert << '\n';
  return static_cast<bool>(out);
}

bool read_trigger(std::istream& in, TriggerProfile& p) {
  unsigned minimum{}, maximum{};
  int invert{};
  if (!(in >> minimum >> maximum >> p.inner_deadzone >> p.outer_deadzone >>
        p.response_exponent >> p.sensitivity >> invert) || minimum > 255 ||
      maximum > 255) {
    return false;
  }
  p.calibration.minimum = static_cast<std::uint8_t>(minimum);
  p.calibration.maximum = static_cast<std::uint8_t>(maximum);
  p.invert = invert != 0;
  return true;
}

}  // namespace

bool validate_profile(const InputProfile& profile, std::string* out_error) {
  auto fail = [&](std::string message) {
    if (out_error) *out_error = std::move(message);
    return false;
  };
  if (profile.id.empty()) return fail("profile id is empty");
  const auto valid_axis = [](const AxisCalibration& axis) {
    return axis.minimum < axis.center && axis.center < axis.maximum;
  };
  const auto valid_stick = [&](const StickProfile& stick) {
    return valid_axis(stick.x) && valid_axis(stick.y) &&
           finite_profile_value(stick.inner_deadzone) &&
           finite_profile_value(stick.outer_deadzone) &&
           stick.inner_deadzone < 1.0f && stick.outer_deadzone < 1.0f &&
           stick.inner_deadzone + stick.outer_deadzone < 1.0f &&
           std::isfinite(stick.response_exponent) && stick.response_exponent > 0.0f &&
           std::isfinite(stick.sensitivity) && stick.sensitivity > 0.0f;
  };
  const auto valid_trigger = [&](const TriggerProfile& trigger) {
    return trigger.calibration.minimum < trigger.calibration.maximum &&
           finite_profile_value(trigger.inner_deadzone) &&
           finite_profile_value(trigger.outer_deadzone) &&
           trigger.inner_deadzone < 1.0f && trigger.outer_deadzone < 1.0f &&
           trigger.inner_deadzone + trigger.outer_deadzone < 1.0f &&
           std::isfinite(trigger.response_exponent) && trigger.response_exponent > 0.0f &&
           std::isfinite(trigger.sensitivity) && trigger.sensitivity > 0.0f;
  };
  if (!valid_stick(profile.left_stick)) return fail("invalid left stick profile");
  if (!valid_stick(profile.right_stick)) return fail("invalid right stick profile");
  if (!valid_trigger(profile.left_trigger)) return fail("invalid left trigger profile");
  if (!valid_trigger(profile.right_trigger)) return fail("invalid right trigger profile");
  if (out_error) out_error->clear();
  return true;
}

GamepadState apply_profile(const InputProfile& profile,
                           const GamepadState& state) {
  if (!profile.enabled || identity_profile(profile)) return state;
  auto result = state;
  apply_stick(profile.left_stick, result.thumb_lx, result.thumb_ly);
  apply_stick(profile.right_stick, result.thumb_rx, result.thumb_ry);
  result.left_trigger = apply_trigger(profile.left_trigger, result.left_trigger);
  result.right_trigger = apply_trigger(profile.right_trigger, result.right_trigger);
  return result;
}

ProfileStore::ProfileStore() { profiles_.emplace("default", InputProfile{}); }

bool ProfileStore::upsert(InputProfile profile) {
  std::string error;
  if (!validate_profile(profile, &error)) {
    std::scoped_lock lock(mutex_);
    set_error_locked(error);
    return false;
  }
  std::scoped_lock lock(mutex_);
  profiles_[profile.id] = std::move(profile);
  last_error_.clear();
  return true;
}

bool ProfileStore::erase(std::string_view id) {
  std::scoped_lock lock(mutex_);
  const std::string key(id);
  if (key == default_profile_id_) {
    set_error_locked("cannot erase the active default profile");
    return false;
  }
  if (!profiles_.erase(key)) return false;
  for (auto& user : user_bindings_) if (user == key) user.clear();
  for (auto it = device_bindings_.begin(); it != device_bindings_.end();) {
    if (it->second == key) it = device_bindings_.erase(it);
    else ++it;
  }
  return true;
}

std::optional<InputProfile> ProfileStore::profile(std::string_view id) const {
  std::scoped_lock lock(mutex_);
  const auto it = profiles_.find(id);
  return it == profiles_.end() ? std::nullopt
                               : std::optional<InputProfile>(it->second);
}

std::vector<InputProfile> ProfileStore::profiles() const {
  std::scoped_lock lock(mutex_);
  std::vector<InputProfile> result;
  result.reserve(profiles_.size());
  for (const auto& [id, profile] : profiles_) {
    static_cast<void>(id);
    result.push_back(profile);
  }
  std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
    if (lhs.name != rhs.name) return lhs.name < rhs.name;
    return lhs.id < rhs.id;
  });
  return result;
}

const InputProfile* ProfileStore::resolve_locked(
    std::uint32_t user_index, std::string_view device_identity) const {
  std::string_view profile_id = default_profile_id_;
  if (user_index < kMaxUsers && !user_bindings_[user_index].empty()) {
    profile_id = user_bindings_[user_index];
  } else if (const auto device = device_bindings_.find(device_identity);
             device != device_bindings_.end()) {
    profile_id = device->second;
  }
  const auto profile = profiles_.find(profile_id);
  return profile == profiles_.end() ? nullptr : &profile->second;
}

std::optional<InputProfile> ProfileStore::resolve(
    std::uint32_t user_index, std::string_view device_identity) const {
  std::scoped_lock lock(mutex_);
  const auto* profile = resolve_locked(user_index, device_identity);
  return profile ? std::optional<InputProfile>(*profile) : std::nullopt;
}

void ProfileStore::apply(std::uint32_t user_index,
                         std::string_view device_identity,
                         GamepadState& state) const {
  std::scoped_lock lock(mutex_);
  if (const auto* profile = resolve_locked(user_index, device_identity)) {
    state = apply_profile(*profile, state);
  }
}

bool ProfileStore::set_default(std::string_view profile_id) {
  std::scoped_lock lock(mutex_);
  const std::string id(profile_id);
  if (!profiles_.contains(id)) return false;
  default_profile_id_ = id;
  return true;
}

bool ProfileStore::bind_user(std::uint32_t user_index,
                             std::string_view profile_id) {
  if (user_index >= kMaxUsers) return false;
  std::scoped_lock lock(mutex_);
  const std::string id(profile_id);
  if (!profiles_.contains(id)) return false;
  user_bindings_[user_index] = id;
  return true;
}

bool ProfileStore::clear_user(std::uint32_t user_index) {
  if (user_index >= kMaxUsers) return false;
  std::scoped_lock lock(mutex_);
  user_bindings_[user_index].clear();
  return true;
}

bool ProfileStore::bind_device(std::string device_identity,
                               std::string_view profile_id) {
  if (device_identity.empty()) return false;
  std::scoped_lock lock(mutex_);
  const std::string id(profile_id);
  if (!profiles_.contains(id)) return false;
  device_bindings_[std::move(device_identity)] = id;
  return true;
}

bool ProfileStore::clear_device(std::string_view device_identity) {
  std::scoped_lock lock(mutex_);
  const auto it = device_bindings_.find(device_identity);
  if (it == device_bindings_.end()) return false;
  device_bindings_.erase(it);
  return true;
}

std::optional<std::string> ProfileStore::user_binding(
    std::uint32_t user_index) const {
  if (user_index >= kMaxUsers) return std::nullopt;
  std::scoped_lock lock(mutex_);
  if (user_bindings_[user_index].empty()) return std::nullopt;
  return user_bindings_[user_index];
}

std::optional<std::string> ProfileStore::device_binding(
    std::string_view device_identity) const {
  std::scoped_lock lock(mutex_);
  const auto it = device_bindings_.find(device_identity);
  return it == device_bindings_.end() ? std::nullopt
                                      : std::optional<std::string>(it->second);
}

std::string ProfileStore::serialize() const {
  std::scoped_lock lock(mutex_);
  std::ostringstream out;
  out << "XENON_INPUT_PROFILES 1\n";
  out << "DEFAULT " << std::quoted(default_profile_id_) << '\n';
  std::vector<std::string> profile_ids;
  profile_ids.reserve(profiles_.size());
  for (const auto& [id, profile] : profiles_) {
    static_cast<void>(profile);
    profile_ids.push_back(id);
  }
  std::sort(profile_ids.begin(), profile_ids.end());
  for (const auto& id : profile_ids) {
    const auto& profile = profiles_.at(id);
    out << "PROFILE " << std::quoted(id) << ' ' << std::quoted(profile.name)
        << ' ' << profile.enabled << '\n';
    write_stick(out, "LEFT", profile.left_stick);
    write_stick(out, "RIGHT", profile.right_stick);
    write_trigger(out, "LTRIGGER", profile.left_trigger);
    write_trigger(out, "RTRIGGER", profile.right_trigger);
    out << "END\n";
  }
  for (std::uint32_t i = 0; i < kMaxUsers; ++i) {
    if (!user_bindings_[i].empty())
      out << "USER " << i << ' ' << std::quoted(user_bindings_[i]) << '\n';
  }
  std::vector<std::string> device_ids;
  device_ids.reserve(device_bindings_.size());
  for (const auto& [device, profile] : device_bindings_) {
    static_cast<void>(profile);
    device_ids.push_back(device);
  }
  std::sort(device_ids.begin(), device_ids.end());
  for (const auto& device : device_ids)
    out << "DEVICE " << std::quoted(device) << ' '
        << std::quoted(device_bindings_.at(device)) << '\n';
  return out.str();
}

bool ProfileStore::deserialize(std::string_view text) {
  std::istringstream in{std::string(text)};
  std::string magic;
  int version{};
  if (!(in >> magic >> version) || magic != "XENON_INPUT_PROFILES" || version != 1) {
    std::scoped_lock lock(mutex_);
    set_error_locked("unsupported profile document");
    return false;
  }

  ProfileMap profiles;
  std::array<std::string, kMaxUsers> users{};
  BindingMap devices;
  std::string default_id{"default"};
  std::string token;
  while (in >> token) {
    if (token == "DEFAULT") {
      if (!(in >> std::quoted(default_id))) return false;
    } else if (token == "PROFILE") {
      InputProfile p{};
      int enabled{};
      if (!(in >> std::quoted(p.id) >> std::quoted(p.name) >> enabled)) return false;
      p.enabled = enabled != 0;
      std::string label;
      if (!(in >> label) || label != "LEFT" || !read_stick(in, p.left_stick)) return false;
      if (!(in >> label) || label != "RIGHT" || !read_stick(in, p.right_stick)) return false;
      if (!(in >> label) || label != "LTRIGGER" || !read_trigger(in, p.left_trigger)) return false;
      if (!(in >> label) || label != "RTRIGGER" || !read_trigger(in, p.right_trigger)) return false;
      if (!(in >> label) || label != "END") return false;
      std::string error;
      if (!validate_profile(p, &error)) return false;
      profiles[p.id] = std::move(p);
    } else if (token == "USER") {
      std::uint32_t user{};
      std::string profile_id;
      if (!(in >> user >> std::quoted(profile_id)) || user >= kMaxUsers) return false;
      users[user] = std::move(profile_id);
    } else if (token == "DEVICE") {
      std::string device, profile_id;
      if (!(in >> std::quoted(device) >> std::quoted(profile_id)) || device.empty()) return false;
      devices[std::move(device)] = std::move(profile_id);
    } else {
      return false;
    }
  }
  if (!profiles.contains(default_id)) return false;
  for (const auto& user : users) if (!user.empty() && !profiles.contains(user)) return false;
  for (const auto& [device, profile_id] : devices)
    if (!profiles.contains(profile_id)) return false;

  std::scoped_lock lock(mutex_);
  profiles_ = std::move(profiles);
  user_bindings_ = std::move(users);
  device_bindings_ = std::move(devices);
  default_profile_id_ = std::move(default_id);
  last_error_.clear();
  return true;
}

bool ProfileStore::save(const std::filesystem::path& path) const {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  const auto data = serialize();
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(out);
}

bool ProfileStore::load(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::scoped_lock lock(mutex_);
    set_error_locked("profile file could not be opened");
    return false;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return deserialize(buffer.str());
}

ProfileDiagnostics ProfileStore::diagnostics() const {
  std::scoped_lock lock(mutex_);
  ProfileDiagnostics result{};
  result.profile_count = profiles_.size();
  result.default_profile_id = default_profile_id_;
  result.device_binding_count = device_bindings_.size();
  result.last_error = last_error_;
  for (const auto& user : user_bindings_) if (!user.empty()) ++result.user_binding_count;
  return result;
}

void ProfileStore::set_error_locked(std::string message) const {
  last_error_ = std::move(message);
}

}  // namespace xenon::input

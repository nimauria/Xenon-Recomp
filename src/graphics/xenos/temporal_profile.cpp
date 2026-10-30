#include "xenon/gpu/temporal_profile.hpp"

#include <array>
#include <cmath>
#include <fstream>
#include <sstream>
#include <utility>

#include "xenon/core/json.hpp"
#include "xenon/gpu/edram_surface.hpp"

namespace xenon::gpu {
namespace {

using core::JsonValue;

struct FormatName {
  ColorRenderTargetFormat format;
  const char* name;
};

constexpr std::array<FormatName, 12> kColorFormats{{
    {ColorRenderTargetFormat::R8G8B8A8, "R8G8B8A8"},
    {ColorRenderTargetFormat::R8G8B8A8Gamma, "R8G8B8A8Gamma"},
    {ColorRenderTargetFormat::R10G10B10A2, "R10G10B10A2"},
    {ColorRenderTargetFormat::R10G10B10A2Float, "R10G10B10A2Float"},
    {ColorRenderTargetFormat::R16G16Fixed, "R16G16Fixed"},
    {ColorRenderTargetFormat::R16G16B16A16Fixed, "R16G16B16A16Fixed"},
    {ColorRenderTargetFormat::R16G16Float, "R16G16Float"},
    {ColorRenderTargetFormat::R16G16B16A16Float, "R16G16B16A16Float"},
    {ColorRenderTargetFormat::R10G10B10A2As10, "R10G10B10A2As10"},
    {ColorRenderTargetFormat::R10G10B10A2FloatAs16, "R10G10B10A2FloatAs16"},
    {ColorRenderTargetFormat::R32Float, "R32Float"},
    {ColorRenderTargetFormat::R32G32Float, "R32G32Float"},
}};

// The highest first constant a 4-constant matrix can start at in the 256
// vertex-shader float constants.
constexpr std::uint32_t kLastMatrixConstant = 252u;

bool fail(std::string* error, std::string message) {
  if (error) *error = "rendering profile: " + std::move(message);
  return false;
}

bool read_constant(const JsonValue& object, std::string_view key, std::uint32_t limit,
                   std::optional<std::uint32_t>& out, std::string* error) {
  const auto* value = object.find(key);
  if (!value) return true;
  const double number = value->as_number(-1.0);
  if (value->type() != JsonValue::Type::Number || number < 0.0 || number > limit ||
      std::floor(number) != number) {
    return fail(error, std::string(key) + " must be an integer from 0 to " + std::to_string(limit));
  }
  out = static_cast<std::uint32_t>(number);
  return true;
}

bool read_shader_hashes(const JsonValue& object, std::string_view key,
                        std::vector<std::uint64_t>& out, std::string* error) {
  const auto* value = object.find(key);
  if (!value) return true;
  const auto* items = value->as_array();
  if (!items) return fail(error, "hud." + std::string(key) + " must be an array of hex strings");
  for (const auto& item : *items) {
    const auto text = item.as_string();
    std::string_view digits = text;
    if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
      digits.remove_prefix(2);
    if (!item.is_string() || digits.empty() || digits.size() > 16) {
      return fail(error, "hud." + std::string(key) + " entry '" + text +
                             "' is not a 64-bit hex shader hash");
    }
    std::uint64_t hash = 0;
    for (const char c : digits) {
      std::uint64_t nibble = 0;
      if (c >= '0' && c <= '9') nibble = static_cast<std::uint64_t>(c - '0');
      else if (c >= 'a' && c <= 'f') nibble = static_cast<std::uint64_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') nibble = static_cast<std::uint64_t>(c - 'A' + 10);
      else return fail(error, "hud." + std::string(key) + " entry '" + text + "' is not hex");
      hash = (hash << 4u) | nibble;
    }
    out.push_back(hash);
  }
  return true;
}

const JsonValue* section(const JsonValue& object, std::string_view key, std::string* error,
                         bool& ok) {
  const auto* value = object.find(key);
  if (value && !value->is_object()) ok = fail(error, std::string(key) + " must be an object");
  return value && value->is_object() ? value : nullptr;
}

}  // namespace

std::optional<std::uint8_t> parse_color_render_target_format(std::string_view name) {
  for (const auto& entry : kColorFormats)
    if (name == entry.name) return static_cast<std::uint8_t>(entry.format);
  return std::nullopt;
}

const char* color_render_target_format_name(std::uint8_t format) noexcept {
  for (const auto& entry : kColorFormats)
    if (static_cast<std::uint8_t>(entry.format) == format) return entry.name;
  return "unknown";
}

std::optional<TemporalRenderProfile> parse_temporal_render_profile(std::string_view json,
                                                                   std::string* error) {
  if (error) error->clear();
  JsonValue root;
  std::string parse_error;
  if (!JsonValue::parse(json, root, &parse_error)) {
    fail(error, "invalid JSON: " + parse_error);
    return std::nullopt;
  }
  if (!root.is_object()) {
    fail(error, "the document must be an object");
    return std::nullopt;
  }
  if (root.get_string("schema") != kRenderingProfileSchema) {
    fail(error, "schema must be \"" + std::string(kRenderingProfileSchema) + "\"");
    return std::nullopt;
  }
  const auto* temporal = root.find("temporal");
  if (!temporal) return std::nullopt;
  if (!temporal->is_object()) {
    fail(error, "temporal must be an object");
    return std::nullopt;
  }

  bool ok = true;
  TemporalRenderProfile profile{};
  const auto* camera = section(*temporal, "camera", error, ok);
  if (!ok) return std::nullopt;
  if (!camera) {
    fail(error, "temporal.camera is required");
    return std::nullopt;
  }
  std::optional<std::uint32_t> camera_constant;
  if (!read_constant(*camera, "vertexConstant", kLastMatrixConstant, camera_constant, error))
    return std::nullopt;
  if (!camera_constant) {
    fail(error, "temporal.camera.vertexConstant is required");
    return std::nullopt;
  }
  profile.camera_constant = *camera_constant;
  const auto layout = camera->get_string("layout", "rows");
  if (layout == "rows") {
    profile.camera_layout = temporal::MatrixLayout::Rows;
  } else if (layout == "columns") {
    profile.camera_layout = temporal::MatrixLayout::Columns;
  } else {
    fail(error, "temporal.camera.layout must be \"rows\" or \"columns\", not \"" + layout + "\"");
    return std::nullopt;
  }
  if (!read_constant(*camera, "positionConstant", 255u, profile.camera_position_constant, error))
    return std::nullopt;

  if (const auto* scene = section(*temporal, "scene", error, ok)) {
    std::optional<std::uint32_t> depth_tile;
    // EDRAM holds 2048 tiles; RB_DEPTH_INFO's base field is 11 bits.
    if (!read_constant(*scene, "depthTile", 2047u, depth_tile, error)) return std::nullopt;
    if (depth_tile) profile.scene_depth_tile = static_cast<std::uint16_t>(*depth_tile);
    if (const auto* formats = scene->find("colorFormats")) {
      const auto* items = formats->as_array();
      if (!items) {
        fail(error, "temporal.scene.colorFormats must be an array of format names");
        return std::nullopt;
      }
      for (const auto& item : *items) {
        const auto format = parse_color_render_target_format(item.as_string());
        if (!item.is_string() || !format) {
          fail(error, "temporal.scene.colorFormats: unknown colour render target format '" +
                          item.as_string() + "'");
          return std::nullopt;
        }
        profile.scene_color_formats.push_back(*format);
      }
    }
  }
  if (!ok) return std::nullopt;

  if (const auto* jitter = section(*temporal, "jitter", error, ok))
    profile.jitter_late_scene_draws = jitter->get_bool("lateSceneDraws", false);
  if (!ok) return std::nullopt;

  if (const auto* hud = section(*temporal, "hud", error, ok)) {
    if (!read_shader_hashes(*hud, "vertexShaders", profile.hud_vertex_shaders, error) ||
        !read_shader_hashes(*hud, "pixelShaders", profile.hud_pixel_shaders, error)) {
      return std::nullopt;
    }
  }
  if (!ok) return std::nullopt;

  if (const auto* cut = section(*temporal, "cameraCut", error, ok)) {
    if (const auto* degrees = cut->find("degrees")) {
      const double value = degrees->as_number(-1.0);
      if (degrees->type() != JsonValue::Type::Number || !(value > 0.0) || value > 180.0) {
        fail(error, "temporal.cameraCut.degrees must be a number in (0, 180]");
        return std::nullopt;
      }
      profile.camera_cut_degrees = static_cast<float>(value);
    }
    if (const auto* distance = cut->find("distance")) {
      const double value = distance->as_number(-1.0);
      if (distance->type() != JsonValue::Type::Number || !(value > 0.0) || !std::isfinite(value)) {
        fail(error, "temporal.cameraCut.distance must be a positive number");
        return std::nullopt;
      }
      profile.camera_cut_distance = static_cast<float>(value);
    }
  }
  if (!ok) return std::nullopt;
  return profile;
}

TemporalProfileLoad load_temporal_render_profile(const std::filesystem::path& package,
                                                 std::string_view effective_hash_hex) {
  TemporalProfileLoad result{};
  if (package.empty() || effective_hash_hex.empty()) return result;
  const auto path = package / "revisions" / std::string(effective_hash_hex) /
                    std::string(kRenderingProfileFile);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) return result;
  result.path = path;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    result.error = "rendering profile: cannot read " + path.string();
    return result;
  }
  std::ostringstream text;
  text << in.rdbuf();
  std::string error;
  result.profile = parse_temporal_render_profile(text.str(), &error);
  if (!error.empty()) {
    result.profile.reset();
    result.error = error + " (" + path.string() + ")";
  }
  return result;
}

}  // namespace xenon::gpu

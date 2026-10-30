#pragma once

// The game-specific half of temporal rendering (jitter, camera motion,
// upscaling, frame generation): where one guest executable revision keeps its
// camera matrix, which render target holds its lit scene, and which shaders
// begin its HUD. Everything else is Xenon's (gpu/temporal_frame.hpp).
//
// A module supplies it per revision in its hint package, read at launch:
//   <module>/xenon-analysis/revisions/<effective image hash>/rendering.json
//   {
//     "schema": "xenon.rendering.v1",
//     "temporal": {
//       "camera": { "vertexConstant": 0, "layout": "rows", "positionConstant": 5 },
//       "scene": { "colorFormats": ["R16G16B16A16Float"], "depthTile": 320 },
//       "jitter": { "lateSceneDraws": false },
//       "hud": { "vertexShaders": ["0x0123456789abcdef"], "pixelShaders": [] },
//       "cameraCut": { "degrees": 45, "distance": 500 }
//     }
//   }
// camera.vertexConstant: first of the four vertex-shader float constants that
//   hold the frame's view-projection matrix; layout "rows" or "columns"
//   (temporal::MatrixLayout). positionConstant (optional): a vertex constant
//   whose xyz is the camera's world position, for a game that renders
//   camera-relative.
// scene.colorFormats (optional): the formats (ColorRenderTargetFormat names)
//   the lit scene colour target may have, to tell it from other targets bound
//   with the scene depth.
// scene.depthTile (optional): the EDRAM base tile of the scene depth target.
//   Without it Xenon learns it: the frame's camera is first defined by a draw
//   that writes colour as well as depth, and a frame that reaches its scene
//   colour resolve fixes the depth target for the frames after it (so a
//   shadow map drawn through the same constants is never the camera).
// jitter.lateSceneDraws: camera draws into the scene depth after the scene
//   colour resolve (later lighting passes) are scene draws too.
// hud: Xenon shader hashes (as the temporal probe prints them); the first draw
//   after the scene colour resolve using one begins the HUD, and every later
//   draw of the frame is HUD.
// cameraCut: a frame whose view direction turns by more than `degrees`, or
//   whose camera moves by more than `distance` world units (optional), has no
//   usable history.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "xenon/gpu/temporal.hpp"

namespace xenon::gpu {

struct TemporalRenderProfile {
  std::uint32_t camera_constant{};
  temporal::MatrixLayout camera_layout{temporal::MatrixLayout::Rows};
  std::optional<std::uint32_t> camera_position_constant{};
  std::vector<std::uint8_t> scene_color_formats{};  // ColorRenderTargetFormat values; empty: any
  std::optional<std::uint16_t> scene_depth_tile{};  // EDRAM base tile; empty: learned per frame
  bool jitter_late_scene_draws{};
  std::vector<std::uint64_t> hud_vertex_shaders{};
  std::vector<std::uint64_t> hud_pixel_shaders{};
  float camera_cut_degrees{45.0f};
  std::optional<float> camera_cut_distance{};
};

inline constexpr std::string_view kRenderingProfileSchema = "xenon.rendering.v1";
inline constexpr std::string_view kRenderingProfileFile = "rendering.json";

// Parses rendering.json. A file without a "temporal" section is valid and
// declares no temporal profile (empty result, no error). Any malformed or
// out-of-range field is an error naming it.
[[nodiscard]] std::optional<TemporalRenderProfile> parse_temporal_render_profile(
    std::string_view json, std::string* error);

struct TemporalProfileLoad {
  std::optional<TemporalRenderProfile> profile{};
  std::filesystem::path path{};  // the file read, empty when there was none
  std::string error{};           // non-empty: the file exists and is invalid
};

// Reads <package>/revisions/<effective_hash_hex>/rendering.json. No package,
// or no file for this revision, is not an error: the game simply has no
// temporal profile.
[[nodiscard]] TemporalProfileLoad load_temporal_render_profile(
    const std::filesystem::path& package, std::string_view effective_hash_hex);

// The ColorRenderTargetFormat with this enumerator name (e.g.
// "R16G16B16A16Float"), or empty.
[[nodiscard]] std::optional<std::uint8_t> parse_color_render_target_format(std::string_view name);
[[nodiscard]] const char* color_render_target_format_name(std::uint8_t format) noexcept;

}  // namespace xenon::gpu

#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "xenon/gpu/temporal.hpp"
#include "xenon/gpu/temporal_frame.hpp"
#include "xenon/gpu/temporal_profile.hpp"

namespace {

using namespace xenon::gpu;
using temporal::Mat4;

bool near(float a, float b, float tolerance = 1e-4f) { return std::fabs(a - b) <= tolerance; }

bool matrices_near(const Mat4& a, const Mat4& b, float tolerance = 1e-4f) {
  for (std::size_t i = 0; i < 16; ++i)
    if (!near(a.m[i], b.m[i], tolerance)) return false;
  return true;
}

// Direct3D left-handed perspective: clip = P * view, depth 0 at zn, 1 at zf.
Mat4 perspective(float fov_y, float aspect, float zn, float zf) {
  Mat4 p{};
  const float ys = 1.0f / std::tan(fov_y * 0.5f);
  p(0, 0) = ys / aspect;
  p(1, 1) = ys;
  p(2, 2) = zf / (zf - zn);
  p(2, 3) = -zn * zf / (zf - zn);
  p(3, 2) = 1.0f;
  p(3, 3) = 0.0f;
  return p;
}

// A camera at `eye`, turned `yaw` radians about +y (left-handed, +z forward).
Mat4 view(std::array<float, 3> eye, float yaw) {
  const float c = std::cos(yaw), s = std::sin(yaw);
  Mat4 rotation{};
  rotation(0, 0) = c;
  rotation(0, 2) = -s;
  rotation(2, 0) = s;
  rotation(2, 2) = c;
  return temporal::multiply(rotation, temporal::translation(-eye[0], -eye[1], -eye[2]));
}

constexpr float kFov = 1.0f;
constexpr float kAspect = 16.0f / 9.0f;

void test_halton_and_jitter() {
  assert(near(temporal::halton(1, 2), 0.5f) && near(temporal::halton(2, 2), 0.25f) &&
         near(temporal::halton(3, 2), 0.75f) && near(temporal::halton(4, 2), 0.125f));
  assert(near(temporal::halton(1, 3), 1.0f / 3.0f) && near(temporal::halton(2, 3), 2.0f / 3.0f) &&
         near(temporal::halton(3, 3), 1.0f / 9.0f));
  assert(temporal::jitter_phase_count(1280, 1280) == 8u);
  assert(temporal::jitter_phase_count(1280, 1920) == 18u);
  assert(temporal::jitter_phase_count(1280, 2560) == 32u);
  const auto first = temporal::jitter_offset(0, 8);
  assert(near(first.x, 0.0f) && near(first.y, 1.0f / 3.0f - 0.5f));
  // The sequence repeats after its phase count and stays in [-0.5, 0.5).
  for (std::uint64_t frame = 0; frame < 64; ++frame) {
    const auto j = temporal::jitter_offset(frame, 8);
    assert(j.x >= -0.5f && j.x < 0.5f && j.y >= -0.5f && j.y < 0.5f);
    const auto again = temporal::jitter_offset(frame + 8, 8);
    assert(j.x == again.x && j.y == again.y);
  }
}

void test_matrix_inverse_and_layout() {
  const auto vp = temporal::multiply(perspective(kFov, kAspect, 0.5f, 5000.0f), view({10, 2, -30}, 0.3f));
  const auto inverse = temporal::invert(vp);
  assert(inverse && matrices_near(temporal::multiply(vp, *inverse), Mat4{}, 1e-3f));
  Mat4 singular{};
  singular(3, 3) = 0.0f;
  singular(3, 0) = 0.0f;
  singular(2, 2) = 0.0f;
  assert(!temporal::invert(singular));

  std::array<float, 16> constants{};
  for (std::size_t i = 0; i < 16; ++i) constants[i] = vp.m[i];
  assert(temporal::matrix_from_constants(constants, temporal::MatrixLayout::Rows) == vp);
  const auto transposed = temporal::matrix_from_constants(constants, temporal::MatrixLayout::Columns);
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) assert(transposed(r, c) == vp(c, r));
}

void test_perspective_parameters() {
  const auto vp = temporal::multiply(perspective(kFov, kAspect, 0.5f, 5000.0f), view({10, 2, -30}, 0.7f));
  const auto params = temporal::perspective_parameters(vp);
  assert(params && near(params->vertical_fov, kFov, 1e-3f));
  assert(near(params->near_plane, 0.5f, 1e-3f) && near(params->far_plane, 5000.0f, 1.0f));
  assert(!params->infinite_far && !params->depth_inverted);
  const auto eye = temporal::camera_position(vp);
  assert(eye && near((*eye)[0], 10.0f, 1e-2f) && near((*eye)[1], 2.0f, 1e-2f) &&
         near((*eye)[2], -30.0f, 1e-2f));

  // Infinite far plane: depth -> 1 at infinity.
  Mat4 infinite = perspective(kFov, kAspect, 1.0f, 1.0f);
  infinite(2, 2) = 1.0f;
  infinite(2, 3) = -1.0f;
  const auto inf = temporal::perspective_parameters(infinite);
  assert(inf && inf->infinite_far && near(inf->near_plane, 1.0f));

  // Inverted depth: 1 at the near plane, 0 at the far plane.
  Mat4 inverted = perspective(kFov, kAspect, 1.0f, 1.0f);
  inverted(2, 2) = 1.0f / (1.0f - 1000.0f);  // zn / (zn - zf)
  inverted(2, 3) = 1000.0f / (1000.0f - 1.0f);
  const auto inv = temporal::perspective_parameters(inverted);
  assert(inv && inv->depth_inverted && near(inv->near_plane, 1.0f, 1e-3f) &&
         near(inv->far_plane, 1000.0f, 0.5f));

  // An orthographic / screen-space transform is not a camera.
  Mat4 ortho{};
  ortho(0, 0) = 2.0f / 1280.0f;
  ortho(1, 1) = -2.0f / 720.0f;
  assert(!temporal::perspective_parameters(ortho));
  assert(!temporal::perspective_parameters(Mat4{}));
}

void test_camera_motion() {
  const auto projection = perspective(kFov, kAspect, 0.5f, 5000.0f);
  const auto vp = temporal::multiply(projection, view({0, 0, 0}, 0.0f));
  // A still camera: no motion anywhere.
  const auto still = temporal::reprojection_matrix(vp, vp);
  assert(still);
  for (const float u : {0.1f, 0.5f, 0.9f}) {
    const auto m = temporal::camera_motion(*still, u, 0.3f, 0.97f, 1280, 720);
    assert(m && near(m->x, 0.0f, 1e-2f) && near(m->y, 0.0f, 1e-2f));
  }
  // A moved and turned camera: the motion is where last frame's camera saw
  // the same world point, minus where this frame's sees it.
  const auto previous = temporal::multiply(projection, view({-3, 1, -2}, 0.05f));
  const std::array<float, 4> world{4.0f, -1.0f, 40.0f, 1.0f};
  const auto project = [&](const Mat4& m) {
    float clip[4]{};
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c) clip[r] += m(r, c) * world[c];
    return std::array<float, 3>{clip[0] / clip[3] * 0.5f + 0.5f, 0.5f - clip[1] / clip[3] * 0.5f,
                                clip[2] / clip[3]};
  };
  const auto now = project(vp);
  const auto before = project(previous);
  const auto reprojection = temporal::reprojection_matrix(vp, previous);
  const auto motion = temporal::camera_motion(*reprojection, now[0], now[1], now[2], 1280, 720);
  assert(motion && near(motion->x, (before[0] - now[0]) * 1280.0f, 0.05f) &&
         near(motion->y, (before[1] - now[1]) * 720.0f, 0.05f));

  // Camera-relative rendering: the matrices hold no translation; the move
  // comes separately and gives the same motion.
  const auto rel_now = temporal::multiply(projection, view({0, 0, 0}, 0.0f));
  const auto rel_before = temporal::multiply(projection, view({0, 0, 0}, 0.05f));
  const std::array<float, 3> move{3.0f, -1.0f, 2.0f};  // (0,0,0) - (-3,1,-2)
  const auto rel = temporal::reprojection_matrix(rel_now, rel_before, move);
  const auto rel_motion = temporal::camera_motion(*rel, now[0], now[1], now[2], 1280, 720);
  assert(rel_motion && near(rel_motion->x, motion->x, 0.05f) && near(rel_motion->y, motion->y, 0.05f));

  // A point that was behind last frame's camera has no motion.
  const auto behind = temporal::multiply(projection, view({0, 0, 100}, 0.0f));
  const auto behind_reprojection = temporal::reprojection_matrix(vp, behind);
  assert(!temporal::camera_motion(*behind_reprojection, now[0], now[1], now[2], 1280, 720));

  assert(near(temporal::view_direction_change_degrees(vp, vp), 0.0f, 1e-2f));
  const auto turned = temporal::multiply(projection, view({0, 0, 0}, 3.14159265f / 2.0f));
  assert(near(temporal::view_direction_change_degrees(vp, turned), 90.0f, 1e-2f));
}

void test_profile_parsing() {
  std::string error;
  const auto profile = parse_temporal_render_profile(R"({
    "schema": "xenon.rendering.v1",
    "temporal": {
      "camera": { "vertexConstant": 4, "layout": "columns", "positionConstant": 9 },
      "scene": { "colorFormats": ["R16G16B16A16Float", "R10G10B10A2"] },
      "jitter": { "lateSceneDraws": true },
      "hud": { "vertexShaders": ["0x00000000000000AB"], "pixelShaders": ["ffffffffffffffff"] },
      "cameraCut": { "degrees": 30, "distance": 250.5 }
    }
  })", &error);
  assert(profile && error.empty());
  assert(profile->camera_constant == 4u && profile->camera_layout == temporal::MatrixLayout::Columns);
  assert(profile->camera_position_constant == 9u);
  assert((profile->scene_color_formats ==
          std::vector<std::uint8_t>{static_cast<std::uint8_t>(ColorRenderTargetFormat::R16G16B16A16Float),
                                    static_cast<std::uint8_t>(ColorRenderTargetFormat::R10G10B10A2)}));
  assert(profile->jitter_late_scene_draws);
  assert((profile->hud_vertex_shaders == std::vector<std::uint64_t>{0xABu}));
  assert((profile->hud_pixel_shaders == std::vector<std::uint64_t>{~0ull}));
  assert(profile->camera_cut_degrees == 30.0f && profile->camera_cut_distance == 250.5f);

  // No temporal section: valid, and no profile.
  assert(!parse_temporal_render_profile(R"({"schema":"xenon.rendering.v1"})", &error) && error.empty());

  const auto rejects = [](const char* json, const char* expected) {
    std::string message;
    const auto parsed = parse_temporal_render_profile(json, &message);
    if (parsed || message.find(expected) == std::string::npos) {
      std::cerr << "expected rejection containing '" << expected << "', got '" << message << "'\n";
      assert(false);
    }
  };
  rejects(R"({"schema":"other","temporal":{}})", "schema");
  rejects(R"({"schema":"xenon.rendering.v1","temporal":{}})", "temporal.camera is required");
  rejects(R"({"schema":"xenon.rendering.v1","temporal":{"camera":{}}})", "vertexConstant is required");
  rejects(R"({"schema":"xenon.rendering.v1","temporal":{"camera":{"vertexConstant":253}}})",
          "vertexConstant");
  rejects(R"({"schema":"xenon.rendering.v1","temporal":{"camera":{"vertexConstant":1.5}}})",
          "vertexConstant");
  rejects(R"({"schema":"xenon.rendering.v1","temporal":{"camera":{"vertexConstant":0,"layout":"diagonal"}}})",
          "layout");
  rejects(R"({"schema":"xenon.rendering.v1","temporal":{"camera":{"vertexConstant":0},"scene":{"colorFormats":["k_8_8_8_8"]}}})",
          "unknown colour render target format");
  rejects(R"({"schema":"xenon.rendering.v1","temporal":{"camera":{"vertexConstant":0},"hud":{"pixelShaders":["0xZZ"]}}})",
          "not hex");
  rejects(R"({"schema":"xenon.rendering.v1","temporal":{"camera":{"vertexConstant":0},"hud":{"pixelShaders":["0x11112222333344445"]}}})",
          "64-bit");
  rejects(R"({"schema":"xenon.rendering.v1","temporal":{"camera":{"vertexConstant":0},"cameraCut":{"degrees":0}}})",
          "degrees");
  rejects(R"({"schema":"xenon.rendering.v1","temporal":{"camera":{"vertexConstant":0},"jitter":true}})",
          "jitter must be an object");
  rejects("{not json", "invalid JSON");
}

void test_profile_loading() {
  const auto root = std::filesystem::temp_directory_path() / "xenon_temporal_profile_test";
  std::filesystem::remove_all(root);
  const std::string hash = "464905ef5d5a80d8b52697f72f16e8c93c4b81ba";
  // No package, or no file for the revision: no profile, no error.
  auto load = load_temporal_render_profile({}, hash);
  assert(!load.profile && load.error.empty() && load.path.empty());
  load = load_temporal_render_profile(root, hash);
  assert(!load.profile && load.error.empty() && load.path.empty());

  const auto dir = root / "revisions" / hash;
  std::filesystem::create_directories(dir);
  {
    std::ofstream out(dir / "rendering.json", std::ios::binary);
    out << R"({"schema":"xenon.rendering.v1","temporal":{"camera":{"vertexConstant":0}}})";
  }
  load = load_temporal_render_profile(root, hash);
  assert(load.profile && load.error.empty() && load.path == dir / "rendering.json");
  // Another revision's package entry is not this one's.
  assert(!load_temporal_render_profile(root, "cca65debbbdf02863111cd9f8ea1fde1c2f34555").profile);
  {
    std::ofstream out(dir / "rendering.json", std::ios::binary | std::ios::trunc);
    out << R"({"schema":"xenon.rendering.v1","temporal":{"camera":{"layout":"rows"}}})";
  }
  load = load_temporal_render_profile(root, hash);
  assert(!load.profile && load.error.find("vertexConstant is required") != std::string::npos &&
         load.error.find("rendering.json") != std::string::npos);
  std::filesystem::remove_all(root);
}

// ---- The frame analyzer, driven by graphics IR as the command processor emits it ----

constexpr std::uint32_t kSceneDepthTile = 0x140;
constexpr std::uint32_t kSceneColorTile = 0x000;
constexpr std::uint32_t kShadowDepthTile = 0x300;
constexpr std::uint32_t kDepthLessEqualWrite = (1u << 1) | (1u << 2) | (3u << 4);
constexpr std::uint32_t kDepthLessEqualTest = (1u << 1) | (3u << 4);

struct ShaderPair {
  std::uint64_t vertex{};
  std::uint64_t pixel{};
};

class Frames {
 public:
  explicit Frames(TemporalFrameAnalyzer& analyzer) : analyzer_(analyzer) {}

  void reg(std::uint32_t index, std::uint32_t value) {
    analyzer_.observe(ir::RegisterWrite{index, value});
  }
  void constants(std::uint32_t first, const Mat4& rows) {
    for (std::uint32_t i = 0; i < 16; ++i)
      reg(0x4000u + first * 4u + i, std::bit_cast<std::uint32_t>(rows.m[i]));
  }
  // Loads a vertex shader that reads `reads` float constants; returns its hash.
  std::uint64_t vertex_shader(std::uint32_t seed, std::vector<std::uint16_t> reads) {
    const std::array<std::uint32_t, 3> dwords{seed, seed * 7u + 1u, seed * 13u + 5u};
    ir::ShaderLoad load{};
    load.program = ShaderProgram(ShaderStage::Vertex, dwords);
    load.decoded.stage = ShaderStage::Vertex;
    load.decoded.reflection.float_constants = std::move(reads);
    analyzer_.observe(load);
    return load.program.hash();
  }
  std::uint64_t pixel_shader(std::uint32_t seed) {
    const std::array<std::uint32_t, 3> dwords{seed, seed + 3u, seed + 9u};
    ir::ShaderLoad load{};
    load.program = ShaderProgram(ShaderStage::Pixel, dwords);
    load.decoded.stage = ShaderStage::Pixel;
    analyzer_.observe(load);
    return load.program.hash();
  }
  void targets(std::uint32_t color_tile, std::uint32_t color_format, std::uint32_t color_mask,
               std::uint32_t depth_tile, std::uint32_t depth_control) {
    reg(0x2208, color_mask ? 4u : 5u);  // RB_MODECONTROL: colour+depth or depth only
    reg(0x2001, color_tile | (color_format << 16));
    reg(0x2104, color_mask);
    reg(0x2002, depth_tile);
    reg(0x2200, depth_control);
  }
  void viewport(float width, float height) {
    reg(0x2206, 0x3Fu);  // PA_CL_VTE_CNTL: all scales and offsets enabled
    reg(0x210F, std::bit_cast<std::uint32_t>(width * 0.5f));
    reg(0x2110, std::bit_cast<std::uint32_t>(width * 0.5f));
    reg(0x2111, std::bit_cast<std::uint32_t>(-height * 0.5f));
    reg(0x2112, std::bit_cast<std::uint32_t>(height * 0.5f));
  }
  TemporalPass draw(ShaderPair shaders) {
    ir::DrawPacket draw{};
    draw.vertex_shader = {true, shaders.vertex, 0};
    draw.pixel_shader = {true, shaders.pixel, 0};
    return analyzer_.observe(draw);
  }
  TemporalPass resolve(std::uint32_t color_tile, std::uint32_t source_select = 0) {
    reg(0x2208, 6u);  // RB_MODECONTROL: copy
    reg(0x2001, color_tile | (static_cast<std::uint32_t>(ColorRenderTargetFormat::R16G16B16A16Float) << 16));
    reg(0x2318, source_select);
    ir::DrawPacket draw{};
    return analyzer_.observe(draw);
  }

 private:
  TemporalFrameAnalyzer& analyzer_;
};

struct GameShaders {
  ShaderPair world{};      // reads the camera at c0-c3 and a world matrix at c4-c7
  ShaderPair shadow{};     // reads the light's matrix at c0-c3
  ShaderPair fullscreen{}; // reads nothing: a screen-space quad
  ShaderPair hud{};
};

GameShaders load_game_shaders(Frames& f) {
  GameShaders s{};
  const auto ps = f.pixel_shader(100);
  s.world = {f.vertex_shader(1, {0, 1, 2, 3, 4, 5, 6, 7}), ps};
  s.shadow = {f.vertex_shader(2, {0, 1, 2, 3}), f.pixel_shader(101)};
  s.fullscreen = {f.vertex_shader(3, {}), f.pixel_shader(102)};
  s.hud = {f.vertex_shader(4, {0, 1}), f.pixel_shader(103)};
  return s;
}

// One frame shaped like a typical deferred/forward console renderer: a shadow
// map, the depth prepass and opaque geometry with the frame's camera, a
// screen-space lighting quad into the scene, the scene colour resolve, a
// tone-map pass, then the HUD.
void draw_frame(Frames& f, const GameShaders& s, const Mat4& camera, const Mat4& light) {
  // Shadow map: depth only, the light's (perspective) matrix in c0-c3.
  f.constants(0, light);
  f.targets(0, 0, 0, kShadowDepthTile, kDepthLessEqualWrite);
  assert(f.draw(s.shadow) == TemporalPass::Offscreen);
  // Scene: the camera.
  f.constants(0, camera);
  f.viewport(1280.0f, 720.0f);
  f.targets(kSceneColorTile, static_cast<std::uint32_t>(ColorRenderTargetFormat::R16G16B16A16Float), 0xF,
            kSceneDepthTile, kDepthLessEqualWrite);
  assert(f.draw(s.world) == TemporalPass::Scene);
  // A different world matrix in c4-c7 is still the same camera.
  f.reg(0x4010, 0x3F000000u);
  assert(f.draw(s.world) == TemporalPass::Scene);
  // Screen-space lighting quad tested against the scene depth: not jittered.
  f.targets(kSceneColorTile, static_cast<std::uint32_t>(ColorRenderTargetFormat::R16G16B16A16Float), 0xF,
            kSceneDepthTile, kDepthLessEqualTest);
  assert(f.draw(s.fullscreen) == TemporalPass::SceneScreenSpace);
  assert(f.resolve(kSceneColorTile) == TemporalPass::SceneColorResolve);
  // Post-processing into the back buffer.
  f.targets(0x200, 0, 0xF, kSceneDepthTile, 0);
  assert(f.draw(s.fullscreen) == TemporalPass::PostProcess);
  assert(f.draw(s.hud) == TemporalPass::Hud);
  // Everything after the first HUD draw is HUD, whatever it is.
  assert(f.draw(s.fullscreen) == TemporalPass::Hud);
  assert(f.resolve(0x200) == TemporalPass::Resolve);
}

TemporalRenderProfile game_profile(const GameShaders& s) {
  TemporalRenderProfile profile{};
  profile.camera_constant = 0;
  profile.camera_layout = temporal::MatrixLayout::Rows;
  profile.scene_color_formats = {static_cast<std::uint8_t>(ColorRenderTargetFormat::R16G16B16A16Float)};
  profile.hud_pixel_shaders = {s.hud.pixel};
  profile.camera_cut_degrees = 30.0f;
  return profile;
}

void test_analyzer_classifies_a_frame() {
  TemporalFrameAnalyzer analyzer;
  Frames f(analyzer);
  const auto shaders = load_game_shaders(f);
  analyzer.set_profile(game_profile(shaders));
  const auto projection = perspective(kFov, kAspect, 0.5f, 5000.0f);
  const auto light = temporal::multiply(perspective(0.8f, 1.0f, 1.0f, 800.0f), view({50, 80, 0}, 1.2f));
  const auto camera0 = temporal::multiply(projection, view({0, 2, 0}, 0.0f));
  draw_frame(f, shaders, camera0, light);
  const auto& first = analyzer.end_frame();
  assert(first.frame == 0u && first.profiled && first.draws == 9u);
  assert(first.has_camera && first.view_projection == camera0);
  assert(first.perspective && near(first.perspective->vertical_fov, kFov, 1e-3f));
  assert(first.scene_depth_base_tile == kSceneDepthTile && first.scene_surface_pitch == 0u);
  assert(near(std::fabs(first.scene_viewport.x_scale) * 2.0f, 1280.0f));
  assert(first.scene_color_base_tile == kSceneColorTile);
  assert(first.scene_color_format == static_cast<std::uint8_t>(ColorRenderTargetFormat::R16G16B16A16Float));
  assert(first.scene_color_resolve == 4u && first.hud_begin == 6u);
  assert(first.hud_pixel_shader == shaders.hud.pixel);
  assert(first.pass_draws[static_cast<std::size_t>(TemporalPass::Scene)] == 2u);
  assert(first.pass_draws[static_cast<std::size_t>(TemporalPass::Hud)] == 2u);
  assert(first.pass_draws[static_cast<std::size_t>(TemporalPass::Offscreen)] == 1u);
  assert(first.reset && !first.has_previous && "the first frame has no history");

  // A small camera move: history is usable, and the previous matrix is the
  // last frame's camera.
  const auto camera1 = temporal::multiply(projection, view({0.5f, 2, 0.2f}, 0.05f));
  draw_frame(f, shaders, camera1, light);
  const auto& second = analyzer.end_frame();
  assert(!second.reset && second.has_previous && second.previous_view_projection == camera0);
  assert(near(second.view_turn_degrees, 0.05f * 57.29578f, 1e-2f));
  assert(near(second.camera_distance, std::sqrt(0.25f + 0.04f), 1e-2f));

  // A cut: the camera turns past the profile's limit.
  const auto camera2 = temporal::multiply(projection, view({0.5f, 2, 0.2f}, 1.0f));
  draw_frame(f, shaders, camera2, light);
  assert(analyzer.end_frame().reset);

  // A frame with no 3D scene (a menu) breaks the history too.
  f.targets(0x200, 0, 0xF, kSceneDepthTile, 0);
  assert(f.draw(shaders.hud) == TemporalPass::Offscreen);
  const auto& menu = analyzer.end_frame();
  assert(!menu.has_camera && menu.reset && !menu.scene_color_resolve);
  draw_frame(f, shaders, camera2, light);
  assert(analyzer.end_frame().reset);
  assert(analyzer.frames() == 5u);
}

void test_analyzer_late_scene_draws_and_camera_relative_profiles() {
  TemporalFrameAnalyzer analyzer;
  Frames f(analyzer);
  const auto shaders = load_game_shaders(f);
  auto profile = game_profile(shaders);
  profile.jitter_late_scene_draws = true;
  profile.camera_position_constant = 12;
  profile.camera_cut_distance = 10.0f;
  analyzer.set_profile(profile);
  const auto projection = perspective(kFov, kAspect, 0.5f, 5000.0f);
  const auto camera = temporal::multiply(projection, view({0, 0, 0}, 0.0f));
  const auto draw_scene = [&](float x) {
    f.reg(0x4030, std::bit_cast<std::uint32_t>(x));  // c12.x: camera position
    f.reg(0x4031, 0u);
    f.reg(0x4032, 0u);
    f.constants(0, camera);
    f.targets(kSceneColorTile, static_cast<std::uint32_t>(ColorRenderTargetFormat::R16G16B16A16Float), 0xF,
              kSceneDepthTile, kDepthLessEqualWrite);
    assert(f.draw(shaders.world) == TemporalPass::Scene);
    assert(f.resolve(kSceneColorTile) == TemporalPass::SceneColorResolve);
    // A later lighting pass with the camera into the scene depth.
    f.targets(0x200, 0, 0xF, kSceneDepthTile, kDepthLessEqualTest);
    assert(f.draw(shaders.world) == TemporalPass::Scene);
    return analyzer.end_frame();
  };
  draw_scene(100.0f);
  const auto moved = draw_scene(103.0f);
  assert(!moved.reset && near(moved.camera_move[0], 3.0f) && near(moved.camera_distance, 3.0f));
  // Teleporting further than the cut distance resets.
  assert(draw_scene(200.0f).reset);
}

void test_analyzer_scene_colour_needs_a_profile_format() {
  TemporalFrameAnalyzer analyzer;
  Frames f(analyzer);
  const auto shaders = load_game_shaders(f);
  auto profile = game_profile(shaders);
  profile.scene_color_formats = {static_cast<std::uint8_t>(ColorRenderTargetFormat::R10G10B10A2)};
  analyzer.set_profile(profile);
  const auto camera = temporal::multiply(perspective(kFov, kAspect, 0.5f, 5000.0f), view({0, 0, 0}, 0.0f));
  f.constants(0, camera);
  // A camera draw into a 16-bit float target: not the scene colour this
  // profile names, so its resolve is an ordinary one.
  f.targets(kSceneColorTile, static_cast<std::uint32_t>(ColorRenderTargetFormat::R16G16B16A16Float), 0xF,
            kSceneDepthTile, kDepthLessEqualWrite);
  assert(f.draw(shaders.world) == TemporalPass::Scene);
  assert(f.resolve(kSceneColorTile) == TemporalPass::Resolve);
  const auto& report = analyzer.end_frame();
  assert(!report.scene_color_base_tile && !report.scene_color_resolve);
}

void test_discovery_finds_the_camera_and_hud_candidates() {
  TemporalFrameAnalyzer analyzer;
  Frames f(analyzer);
  const auto shaders = load_game_shaders(f);
  analyzer.set_discovery(true);
  const auto projection = perspective(kFov, kAspect, 0.5f, 5000.0f);
  const auto light = temporal::multiply(perspective(0.8f, 1.0f, 1.0f, 800.0f), view({50, 80, 0}, 1.2f));
  const auto camera = temporal::multiply(projection, view({0, 2, 0}, 0.0f));
  // No profile: every draw is unclassified, but the camera shows up as the
  // candidate with the most depth-writing draws.
  f.constants(0, light);
  f.targets(0, 0, 0, kShadowDepthTile, kDepthLessEqualWrite);
  assert(f.draw(shaders.shadow) == TemporalPass::Unclassified);
  f.constants(0, camera);
  f.targets(kSceneColorTile, 7, 0xF, kSceneDepthTile, kDepthLessEqualWrite);
  for (int i = 0; i < 3; ++i) assert(f.draw(shaders.world) == TemporalPass::Unclassified);
  const auto& report = analyzer.end_frame();
  assert(!report.profiled && !report.camera_candidates.empty());
  const auto& best = report.camera_candidates.front();
  assert(best.constant == 0u && best.layout == temporal::MatrixLayout::Rows &&
         best.depth_base_tile == kSceneDepthTile && best.draws == 3u);
  assert(near(best.aspect, kAspect, 1e-3f));
  assert(best.matrix == camera);
  const auto text = format_temporal_frame_report(report);
  assert(text.find("camera candidate: c0 rows depth tile 320 draws 3") != std::string::npos);

  // With the camera in a profile, discovery lists the shaders drawn after the
  // scene, in order: where a module author finds the HUD's.
  auto profile = game_profile(shaders);
  profile.hud_pixel_shaders.clear();
  analyzer.set_profile(profile);
  const auto light2 = light;
  f.constants(0, light2);
  f.targets(0, 0, 0, kShadowDepthTile, kDepthLessEqualWrite);
  f.draw(shaders.shadow);
  f.constants(0, camera);
  f.targets(kSceneColorTile, 7, 0xF, kSceneDepthTile, kDepthLessEqualWrite);
  f.draw(shaders.world);
  f.resolve(kSceneColorTile);
  f.targets(0x200, 0, 0xF, kSceneDepthTile, 0);
  f.draw(shaders.fullscreen);
  f.draw(shaders.hud);
  f.draw(shaders.hud);
  const auto& late = analyzer.end_frame();
  assert(late.late_shaders.size() == 2u);
  assert(late.late_shaders[0].pixel_shader == shaders.fullscreen.pixel && late.late_shaders[0].first_draw == 3u);
  assert(late.late_shaders[1].pixel_shader == shaders.hud.pixel && late.late_shaders[1].draws == 2u);
  assert(format_temporal_frame_report(late).find("after scene: draw 4") != std::string::npos);
}

// A shadow pass that writes colour too (a variance shadow map) through the
// same constants, before the scene: with the scene depth pinned by the
// profile it is never the camera.
void test_analyzer_pinned_scene_depth_ignores_a_colour_shadow_pass() {
  TemporalFrameAnalyzer analyzer;
  Frames f(analyzer);
  const auto shaders = load_game_shaders(f);
  auto profile = game_profile(shaders);
  profile.scene_depth_tile = static_cast<std::uint16_t>(kSceneDepthTile);
  analyzer.set_profile(profile);
  const auto camera = temporal::multiply(perspective(kFov, kAspect, 0.5f, 5000.0f), view({0, 0, 0}, 0.0f));
  const auto light = temporal::multiply(perspective(0.8f, 1.0f, 1.0f, 800.0f), view({50, 80, 0}, 1.2f));
  f.constants(0, light);
  f.targets(0x500, static_cast<std::uint32_t>(ColorRenderTargetFormat::R16G16Float), 0xF, kShadowDepthTile,
            kDepthLessEqualWrite);
  assert(f.draw(shaders.shadow) == TemporalPass::Offscreen);
  f.constants(0, camera);
  f.targets(kSceneColorTile, static_cast<std::uint32_t>(ColorRenderTargetFormat::R16G16B16A16Float), 0xF,
            kSceneDepthTile, kDepthLessEqualWrite);
  assert(f.draw(shaders.world) == TemporalPass::Scene);
  const auto& report = analyzer.end_frame();
  assert(report.has_camera && report.view_projection == camera);

  std::string error;
  const auto parsed = parse_temporal_render_profile(
      R"({"schema":"xenon.rendering.v1","temporal":{"camera":{"vertexConstant":0},"scene":{"depthTile":320}}})",
      &error);
  assert(parsed && parsed->scene_depth_tile == 320u);
  assert(!parse_temporal_render_profile(
             R"({"schema":"xenon.rendering.v1","temporal":{"camera":{"vertexConstant":0},"scene":{"depthTile":2048}}})",
             &error) &&
         error.find("depthTile") != std::string::npos);
}

void test_analyzer_reset_forgets_history() {
  TemporalFrameAnalyzer analyzer;
  Frames f(analyzer);
  const auto shaders = load_game_shaders(f);
  analyzer.set_profile(game_profile(shaders));
  const auto camera = temporal::multiply(perspective(kFov, kAspect, 0.5f, 5000.0f), view({0, 0, 0}, 0.0f));
  const auto light = temporal::multiply(perspective(0.8f, 1.0f, 1.0f, 800.0f), view({50, 80, 0}, 1.2f));
  draw_frame(f, shaders, camera, light);
  analyzer.end_frame();
  analyzer.reset();
  assert(analyzer.frames() == 0u);
  // Shaders loaded before the reset are forgotten with the rest of the GPU
  // state, so the same draw is no longer known to read the camera.
  f.constants(0, camera);
  f.targets(kSceneColorTile, 7, 0xF, kSceneDepthTile, kDepthLessEqualWrite);
  assert(f.draw(shaders.world) == TemporalPass::Offscreen);
  assert(analyzer.end_frame().reset);
}

}  // namespace

int main() {
  test_halton_and_jitter();
  test_matrix_inverse_and_layout();
  test_perspective_parameters();
  test_camera_motion();
  test_profile_parsing();
  test_profile_loading();
  test_analyzer_classifies_a_frame();
  test_analyzer_late_scene_draws_and_camera_relative_profiles();
  test_analyzer_scene_colour_needs_a_profile_format();
  test_discovery_finds_the_camera_and_hud_candidates();
  test_analyzer_pinned_scene_depth_ignores_a_colour_shadow_pass();
  test_analyzer_reset_forgets_history();
  std::cout << "xenon_temporal_tests passed\n";
  return 0;
}

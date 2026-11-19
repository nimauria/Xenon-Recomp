#pragma once

// Classifies a guest frame's draws for temporal rendering, from the decoded
// graphics IR alone (so for every backend): which draws render the 3D scene
// with the frame's camera (they get sub-pixel jitter and camera motion),
// which resolve copies out the lit scene colour (where a temporal upscaler
// runs), and which draws are post-processing and HUD (they stay at output
// resolution, unjittered, and are what frame generation must not
// interpolate). The game-specific facts come from a TemporalRenderProfile.
//
// Without a profile it can still run in discovery mode, reporting the vertex
// constants that hold a perspective matrix on depth-writing draws and the
// shaders drawn after the scene, which is how a module author finds the
// values for a profile (the temporal probe, docs/runtime/RUNTIME_HOST.md).
//
// Not thread-safe: observe() and end_frame() run on the thread that executes
// the graphics IR (GraphicsSystem::execute_ir and the front-buffer swap it
// precedes).

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "xenon/gpu/ir.hpp"
#include "xenon/gpu/resource_ir.hpp"
#include "xenon/gpu/temporal.hpp"
#include "xenon/gpu/temporal_profile.hpp"

namespace xenon::gpu {

enum class TemporalPass : std::uint8_t {
  None,               // not a draw
  Unclassified,       // a draw, with no profile to classify it by
  Offscreen,          // before the scene colour resolve, not into the scene depth
  Scene,              // the frame's camera geometry into the scene depth: jittered
  SceneScreenSpace,   // other draws into the scene depth before its resolve: not jittered
  SceneColorResolve,  // the resolve of the lit scene colour
  PostProcess,        // after the scene colour resolve, before the HUD
  Hud,                // from the frame's first HUD draw on
  Resolve,            // any other resolve
};
inline constexpr std::size_t kTemporalPassCount = 9;

[[nodiscard]] const char* temporal_pass_name(TemporalPass pass) noexcept;

// A vertex-constant range holding a perspective matrix on depth-writing draws.
struct TemporalCameraCandidate {
  std::uint32_t constant{};
  temporal::MatrixLayout layout{temporal::MatrixLayout::Rows};
  std::uint16_t depth_base_tile{};
  std::uint32_t draws{};
  temporal::Mat4 matrix{};
  temporal::PerspectiveParameters perspective{};
  float aspect{};  // the matrix's x / y scale ratio inverted: y scale / x scale
};

// A vertex/pixel shader pair drawn after the scene colour resolve, in order
// of first use: the candidates for a profile's HUD shaders.
struct TemporalShaderUse {
  std::uint32_t first_draw{};
  std::uint64_t vertex_shader{};
  std::uint64_t pixel_shader{};
  std::uint32_t draws{};
  std::uint16_t color_base_tile{};
};

struct TemporalFrameReport {
  std::uint64_t frame{};
  bool profiled{};
  std::uint32_t draws{};  // draws and resolves, in order; the indices below count them
  std::array<std::uint32_t, kTemporalPassCount> pass_draws{};

  bool has_camera{};
  temporal::Mat4 view_projection{};
  std::optional<temporal::PerspectiveParameters> perspective{};
  std::optional<std::array<float, 3>> camera_position{};  // profile positionConstant
  bool has_previous{};
  temporal::Mat4 previous_view_projection{};
  // This frame's camera position minus the previous one's, when the profile
  // names a position constant (camera-relative rendering); zero otherwise.
  std::array<float, 3> camera_move{};
  float view_turn_degrees{};
  float camera_distance{};  // how far the camera moved, when it is known
  bool reset{true};         // no usable history: first frame, no camera, or a cut

  std::uint16_t scene_depth_base_tile{};
  std::uint16_t scene_surface_pitch{};
  ViewportState scene_viewport{};
  std::optional<std::uint16_t> scene_color_base_tile{};
  std::uint8_t scene_color_format{};
  std::optional<std::uint32_t> scene_color_resolve{};
  std::optional<std::uint32_t> hud_begin{};
  std::uint64_t hud_vertex_shader{};
  std::uint64_t hud_pixel_shader{};

  // Discovery (TemporalFrameAnalyzer::set_discovery).
  std::vector<TemporalCameraCandidate> camera_candidates{};  // most draws first
  std::vector<TemporalShaderUse> late_shaders{};
};

// A readable, multi-line summary of a report for the temporal probe log.
[[nodiscard]] std::string format_temporal_frame_report(const TemporalFrameReport& report);

class TemporalFrameAnalyzer {
 public:
  static constexpr std::size_t kMaxCameraCandidates = 8;
  static constexpr std::size_t kMaxLateShaders = 32;

  void set_profile(std::optional<TemporalRenderProfile> profile);
  [[nodiscard]] const std::optional<TemporalRenderProfile>& profile() const noexcept {
    return profile_;
  }
  void set_discovery(bool enabled) noexcept { discovery_ = enabled; }
  [[nodiscard]] bool discovery() const noexcept { return discovery_; }
  [[nodiscard]] bool active() const noexcept { return profile_.has_value() || discovery_; }
  // Called with each closed frame's report.
  void set_frame_callback(std::function<void(const TemporalFrameReport&)> callback) {
    frame_callback_ = std::move(callback);
  }

  // Forgets register state, shaders and camera history (the GPU was reset).
  void reset();
  // Tracks one IR command in execution order; for a draw or resolve, returns
  // its classification (TemporalPass::None for anything else).
  TemporalPass observe(const ir::Command& command);
  // Closes the frame at a front-buffer swap.
  const TemporalFrameReport& end_frame();

  [[nodiscard]] const TemporalFrameReport& last_frame() const noexcept { return last_; }
  [[nodiscard]] const TemporalFrameReport& current_frame() const noexcept { return current_; }
  [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }

 private:
  struct CandidateKey {
    std::uint32_t constant{};
    temporal::MatrixLayout layout{};
    std::uint16_t depth_base_tile{};
    std::array<std::uint32_t, 16> bits{};
    friend auto operator<=>(const CandidateKey&, const CandidateKey&) = default;
  };

  TemporalPass classify_draw(const ir::DrawPacket& draw);
  [[nodiscard]] const std::vector<std::uint16_t>* vertex_constants(std::uint64_t hash) const;
  [[nodiscard]] bool reads_matrix(const std::vector<std::uint16_t>& constants,
                                  std::uint32_t first) const;
  [[nodiscard]] std::array<std::uint32_t, 16> matrix_bits(std::uint32_t first) const;
  void discover(const ir::DrawPacket& draw, const DrawResourceState& state);
  void note_late_shader(const ir::DrawPacket& draw, const DrawResourceState& state);

  std::optional<TemporalRenderProfile> profile_{};
  bool discovery_{};
  std::function<void(const TemporalFrameReport&)> frame_callback_{};

  ResourceStateTracker registers_{};
  // Float constants each vertex shader reads, by Xenon shader hash.
  std::unordered_map<std::uint64_t, std::vector<std::uint16_t>> vertex_constants_{};

  TemporalFrameReport current_{};
  TemporalFrameReport last_{};
  std::uint64_t frames_{};
  std::array<std::uint32_t, 16> camera_bits_{};
  bool hud_started_{};
  std::map<CandidateKey, std::uint32_t> candidate_draws_{};

  bool previous_valid_{};
  temporal::Mat4 previous_view_projection_{};
  std::optional<std::array<float, 3>> previous_position_{};
  // The scene depth target of recent frames that reached their scene colour
  // resolve, forgotten after kRelearnFrames frames without one.
  static constexpr std::uint32_t kRelearnFrames = 8;
  std::optional<std::uint16_t> learned_depth_tile_{};
  std::uint32_t frames_without_scene_{};
};

}  // namespace xenon::gpu

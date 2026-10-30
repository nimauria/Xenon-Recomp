#include "xenon/gpu/temporal_frame.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace xenon::gpu {
namespace {

// Vertex-shader float constant c[i] is registers 0x4000 + 4i .. +3 (the pixel
// shader bank starts at 0x4400).
constexpr std::uint32_t kVertexFloatConstantBase = 0x4000u;
// Distinct (constant, layout, depth target, matrix) combinations tracked per
// frame in discovery mode; a frame with more drops the rest.
constexpr std::size_t kMaxCandidateKeys = 256;
// A perspective matrix whose field of view is outside this range is not a
// game camera.
constexpr float kMinCameraFovRadians = 5.0f * 3.14159265f / 180.0f;
constexpr float kMaxCameraFovRadians = 170.0f * 3.14159265f / 180.0f;

bool is_draw_mode(EdramMode mode) {
  return mode == EdramMode::ColorDepth || mode == EdramMode::DepthOnly;
}

std::string hex64(std::uint64_t value) {
  char text[24];
  std::snprintf(text, sizeof(text), "0x%016llx", static_cast<unsigned long long>(value));
  return text;
}

float row_length(const temporal::Mat4& m, int row) {
  return std::sqrt(m(row, 0) * m(row, 0) + m(row, 1) * m(row, 1) + m(row, 2) * m(row, 2));
}

bool contains(const std::vector<std::uint64_t>& hashes, std::uint64_t hash) {
  return std::find(hashes.begin(), hashes.end(), hash) != hashes.end();
}

}  // namespace

const char* temporal_pass_name(TemporalPass pass) noexcept {
  switch (pass) {
    case TemporalPass::None: return "none";
    case TemporalPass::Unclassified: return "unclassified";
    case TemporalPass::Offscreen: return "offscreen";
    case TemporalPass::Scene: return "scene";
    case TemporalPass::SceneScreenSpace: return "scene-screen-space";
    case TemporalPass::SceneColorResolve: return "scene-color-resolve";
    case TemporalPass::PostProcess: return "post-process";
    case TemporalPass::Hud: return "hud";
    case TemporalPass::Resolve: return "resolve";
  }
  return "unknown";
}

void TemporalFrameAnalyzer::set_profile(std::optional<TemporalRenderProfile> profile) {
  profile_ = std::move(profile);
  current_ = {};
  hud_started_ = false;
  previous_valid_ = false;
  previous_position_.reset();
  learned_depth_tile_.reset();
  frames_without_scene_ = 0;
}

void TemporalFrameAnalyzer::reset() {
  registers_.reset();
  vertex_constants_.clear();
  current_ = {};
  last_ = {};
  frames_ = 0;
  hud_started_ = false;
  candidate_draws_.clear();
  previous_valid_ = false;
  previous_position_.reset();
  learned_depth_tile_.reset();
  frames_without_scene_ = 0;
}

TemporalPass TemporalFrameAnalyzer::observe(const ir::Command& command) {
  if (const auto* write = std::get_if<ir::RegisterWrite>(&command)) {
    registers_.apply(*write);
    return TemporalPass::None;
  }
  if (const auto* load = std::get_if<ir::ShaderLoad>(&command)) {
    if (load->program.stage() == ShaderStage::Vertex)
      vertex_constants_[load->program.hash()] = load->decoded.reflection.float_constants;
    return TemporalPass::None;
  }
  if (const auto* draw = std::get_if<ir::DrawPacket>(&command)) {
    const auto pass = classify_draw(*draw);
    ++current_.pass_draws[static_cast<std::size_t>(pass)];
    return pass;
  }
  return TemporalPass::None;
}

const std::vector<std::uint16_t>* TemporalFrameAnalyzer::vertex_constants(
    std::uint64_t hash) const {
  const auto it = vertex_constants_.find(hash);
  return it == vertex_constants_.end() ? nullptr : &it->second;
}

bool TemporalFrameAnalyzer::reads_matrix(const std::vector<std::uint16_t>& constants,
                                         std::uint32_t first) const {
  // constants is sorted (ShaderDecoder reflection).
  for (std::uint32_t i = 0; i < 4; ++i) {
    if (!std::binary_search(constants.begin(), constants.end(),
                            static_cast<std::uint16_t>(first + i))) {
      return false;
    }
  }
  return true;
}

std::array<std::uint32_t, 16> TemporalFrameAnalyzer::matrix_bits(std::uint32_t first) const {
  std::array<std::uint32_t, 16> bits{};
  for (std::uint32_t i = 0; i < 16; ++i)
    bits[i] = registers_.register_value(kVertexFloatConstantBase + first * 4u + i);
  return bits;
}

TemporalPass TemporalFrameAnalyzer::classify_draw(const ir::DrawPacket& draw) {
  const auto index = current_.draws++;
  const auto state = registers_.snapshot_targets();
  const bool resolve = state.edram_mode == EdramMode::Copy;
  if (!resolve && !is_draw_mode(state.edram_mode)) return TemporalPass::Unclassified;

  if (!resolve && discovery_) discover(draw, state);
  if (!profile_) return resolve ? TemporalPass::Resolve : TemporalPass::Unclassified;
  current_.profiled = true;
  const auto& profile = *profile_;

  if (resolve) {
    const auto slot = state.copy.source_select;
    if (!current_.scene_color_resolve && current_.scene_color_base_tile &&
        !state.copy.copies_depth() && slot < state.color_targets.size() &&
        state.color_targets[slot].base_tile == *current_.scene_color_base_tile) {
      current_.scene_color_resolve = index;
      return TemporalPass::SceneColorResolve;
    }
    return TemporalPass::Resolve;
  }

  // Does the draw transform by this frame's camera?
  const auto* constants = draw.vertex_shader.valid ? vertex_constants(draw.vertex_shader.hash)
                                                   : nullptr;
  const bool reads_camera = constants && reads_matrix(*constants, profile.camera_constant);
  const auto& depth = state.depth_target;
  // The scene depth target, once known (the profile's, or the one the last
  // frames with a scene colour resolve used), keeps a shadow map rendered
  // through the same constants from being taken for the camera; until then
  // only a draw that also writes colour can define it.
  const auto scene_depth = profile.scene_depth_tile ? profile.scene_depth_tile : learned_depth_tile_;
  const bool may_define_camera =
      scene_depth ? depth.base_tile == *scene_depth
                  : state.edram_mode == EdramMode::ColorDepth && state.color_targets[0].enabled;
  if (reads_camera && !current_.has_camera && !current_.scene_color_resolve &&
      depth.write_enabled && may_define_camera) {
    const auto bits = matrix_bits(profile.camera_constant);
    std::array<float, 16> values{};
    for (std::size_t i = 0; i < 16; ++i) values[i] = std::bit_cast<float>(bits[i]);
    const auto matrix = temporal::matrix_from_constants(values, profile.camera_layout);
    if (const auto perspective = temporal::perspective_parameters(matrix)) {
      // The frame's first depth-writing perspective draw defines its camera
      // and its scene depth target.
      current_.has_camera = true;
      current_.view_projection = matrix;
      current_.perspective = perspective;
      current_.scene_depth_base_tile = depth.base_tile;
      current_.scene_surface_pitch = state.raster.surface_pitch;
      current_.scene_viewport = state.raster.viewport;
      camera_bits_ = bits;
      if (profile.camera_position_constant) {
        const auto base = kVertexFloatConstantBase + *profile.camera_position_constant * 4u;
        current_.camera_position = std::array<float, 3>{
            std::bit_cast<float>(registers_.register_value(base)),
            std::bit_cast<float>(registers_.register_value(base + 1u)),
            std::bit_cast<float>(registers_.register_value(base + 2u))};
      }
    }
  }
  const bool into_scene_depth = current_.has_camera && depth.test_enabled &&
                                depth.base_tile == current_.scene_depth_base_tile;
  const bool camera_draw = into_scene_depth && reads_camera &&
                           matrix_bits(profile.camera_constant) == camera_bits_;

  if (!current_.scene_color_resolve) {
    if (camera_draw) {
      const auto& color = state.color_targets[0];
      if (!current_.scene_color_base_tile && state.edram_mode == EdramMode::ColorDepth &&
          color.enabled &&
          (profile.scene_color_formats.empty() ||
           std::find(profile.scene_color_formats.begin(), profile.scene_color_formats.end(),
                     color.format) != profile.scene_color_formats.end())) {
        current_.scene_color_base_tile = color.base_tile;
        current_.scene_color_format = color.format;
      }
      return TemporalPass::Scene;
    }
    return into_scene_depth ? TemporalPass::SceneScreenSpace : TemporalPass::Offscreen;
  }

  if (discovery_) note_late_shader(draw, state);
  const auto vs = draw.vertex_shader.valid ? draw.vertex_shader.hash : 0u;
  const auto ps = draw.pixel_shader.valid ? draw.pixel_shader.hash : 0u;
  if (!hud_started_ && ((draw.vertex_shader.valid && contains(profile.hud_vertex_shaders, vs)) ||
                        (draw.pixel_shader.valid && contains(profile.hud_pixel_shaders, ps)))) {
    hud_started_ = true;
    current_.hud_begin = index;
    current_.hud_vertex_shader = vs;
    current_.hud_pixel_shader = ps;
  }
  if (hud_started_) return TemporalPass::Hud;
  if (camera_draw && profile.jitter_late_scene_draws) return TemporalPass::Scene;
  return TemporalPass::PostProcess;
}

void TemporalFrameAnalyzer::discover(const ir::DrawPacket& draw, const DrawResourceState& state) {
  if (!state.depth_target.write_enabled || !draw.vertex_shader.valid) return;
  const auto* constants = vertex_constants(draw.vertex_shader.hash);
  if (!constants) return;
  for (const auto first : *constants) {
    if (first > 252u || !reads_matrix(*constants, first)) continue;
    const auto bits = matrix_bits(first);
    for (const auto layout : {temporal::MatrixLayout::Rows, temporal::MatrixLayout::Columns}) {
      const CandidateKey key{first, layout, state.depth_target.base_tile, bits};
      if (auto it = candidate_draws_.find(key); it != candidate_draws_.end()) {
        ++it->second;
        continue;
      }
      if (candidate_draws_.size() >= kMaxCandidateKeys) continue;
      std::array<float, 16> values{};
      for (std::size_t i = 0; i < 16; ++i) values[i] = std::bit_cast<float>(bits[i]);
      const auto matrix = temporal::matrix_from_constants(values, layout);
      const auto perspective = temporal::perspective_parameters(matrix);
      if (!perspective || perspective->vertical_fov < kMinCameraFovRadians ||
          perspective->vertical_fov > kMaxCameraFovRadians) {
        continue;
      }
      candidate_draws_.emplace(key, 1u);
    }
  }
}

void TemporalFrameAnalyzer::note_late_shader(const ir::DrawPacket& draw,
                                             const DrawResourceState& state) {
  const auto vs = draw.vertex_shader.valid ? draw.vertex_shader.hash : 0u;
  const auto ps = draw.pixel_shader.valid ? draw.pixel_shader.hash : 0u;
  for (auto& use : current_.late_shaders) {
    if (use.vertex_shader == vs && use.pixel_shader == ps) {
      ++use.draws;
      return;
    }
  }
  if (current_.late_shaders.size() >= kMaxLateShaders) return;
  current_.late_shaders.push_back(
      {current_.draws - 1u, vs, ps, 1u, state.color_targets[0].base_tile});
}

const TemporalFrameReport& TemporalFrameAnalyzer::end_frame() {
  current_.frame = frames_++;
  if (current_.has_camera && previous_valid_) {
    current_.has_previous = true;
    current_.previous_view_projection = previous_view_projection_;
    current_.view_turn_degrees =
        temporal::view_direction_change_degrees(previous_view_projection_, current_.view_projection);
    std::optional<std::array<float, 3>> move;
    if (current_.camera_position && previous_position_) {
      move = std::array<float, 3>{(*current_.camera_position)[0] - (*previous_position_)[0],
                                  (*current_.camera_position)[1] - (*previous_position_)[1],
                                  (*current_.camera_position)[2] - (*previous_position_)[2]};
      current_.camera_move = *move;
    } else if (!profile_ || !profile_->camera_position_constant) {
      // The view-projection carries the camera's translation itself.
      const auto now = temporal::camera_position(current_.view_projection);
      const auto before = temporal::camera_position(previous_view_projection_);
      if (now && before)
        move = std::array<float, 3>{(*now)[0] - (*before)[0], (*now)[1] - (*before)[1],
                                    (*now)[2] - (*before)[2]};
    }
    if (move)
      current_.camera_distance =
          std::sqrt((*move)[0] * (*move)[0] + (*move)[1] * (*move)[1] + (*move)[2] * (*move)[2]);
    const bool turned = profile_ && current_.view_turn_degrees > profile_->camera_cut_degrees;
    const bool jumped = profile_ && profile_->camera_cut_distance && move &&
                        current_.camera_distance > *profile_->camera_cut_distance;
    // A camera-relative game without its position this frame cannot be
    // reprojected.
    const bool position_missing =
        profile_ && profile_->camera_position_constant && !move;
    current_.reset = turned || jumped || position_missing;
  } else {
    current_.reset = true;
  }
  if (profile_) {
    if (current_.scene_color_resolve) {
      learned_depth_tile_ = current_.scene_depth_base_tile;
      frames_without_scene_ = 0;
    } else if (learned_depth_tile_ && ++frames_without_scene_ >= kRelearnFrames) {
      // The game moved its scene depth (or has been in a menu a while):
      // learn it again from the next scene.
      learned_depth_tile_.reset();
      frames_without_scene_ = 0;
    }
  }
  previous_valid_ = current_.has_camera;
  if (current_.has_camera) {
    previous_view_projection_ = current_.view_projection;
    previous_position_ = current_.camera_position;
  } else {
    previous_position_.reset();
  }

  if (discovery_) {
    for (const auto& [key, draws] : candidate_draws_) {
      std::array<float, 16> values{};
      for (std::size_t i = 0; i < 16; ++i) values[i] = std::bit_cast<float>(key.bits[i]);
      TemporalCameraCandidate candidate{};
      candidate.constant = key.constant;
      candidate.layout = key.layout;
      candidate.depth_base_tile = key.depth_base_tile;
      candidate.draws = draws;
      candidate.matrix = temporal::matrix_from_constants(values, key.layout);
      candidate.perspective = *temporal::perspective_parameters(candidate.matrix);
      const float x_scale = row_length(candidate.matrix, 0);
      candidate.aspect = x_scale > 0.0f ? row_length(candidate.matrix, 1) / x_scale : 0.0f;
      current_.camera_candidates.push_back(candidate);
    }
    std::stable_sort(current_.camera_candidates.begin(), current_.camera_candidates.end(),
                     [](const auto& a, const auto& b) { return a.draws > b.draws; });
    if (current_.camera_candidates.size() > kMaxCameraCandidates)
      current_.camera_candidates.resize(kMaxCameraCandidates);
  }
  candidate_draws_.clear();

  last_ = std::move(current_);
  current_ = {};
  hud_started_ = false;
  if (frame_callback_) frame_callback_(last_);
  return last_;
}

std::string format_temporal_frame_report(const TemporalFrameReport& report) {
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(3);
  out << "frame " << report.frame << ": " << report.draws << " draws";
  for (std::size_t i = 1; i < kTemporalPassCount; ++i) {
    if (report.pass_draws[i] == 0u) continue;
    out << ", " << temporal_pass_name(static_cast<TemporalPass>(i)) << " " << report.pass_draws[i];
  }
  out << "\n";
  if (report.profiled) {
    if (report.has_camera) {
      out << "  camera: depth tile " << report.scene_depth_base_tile << " pitch "
          << report.scene_surface_pitch << " viewport " << std::fabs(report.scene_viewport.x_scale) * 2.0f
          << "x" << std::fabs(report.scene_viewport.y_scale) * 2.0f;
      if (report.perspective)
        out << " fov " << report.perspective->vertical_fov * 57.29578f << "deg near "
            << report.perspective->near_plane << (report.perspective->depth_inverted ? " inverted" : "")
            << (report.perspective->infinite_far ? " infinite-far" : "");
      out << " turn " << report.view_turn_degrees << "deg move " << report.camera_distance
          << (report.reset ? " RESET" : "") << "\n";
    } else {
      out << "  camera: none this frame (no history)\n";
    }
    if (report.scene_color_base_tile) {
      out << "  scene colour: tile " << *report.scene_color_base_tile << " "
          << color_render_target_format_name(report.scene_color_format) << ", resolve at draw ";
      if (report.scene_color_resolve) out << *report.scene_color_resolve;
      else out << "(none)";
      out << "\n";
    }
    if (report.hud_begin)
      out << "  hud: from draw " << *report.hud_begin << " vs " << hex64(report.hud_vertex_shader)
          << " ps " << hex64(report.hud_pixel_shader) << "\n";
  }
  for (const auto& candidate : report.camera_candidates) {
    out << "  camera candidate: c" << candidate.constant << " "
        << (candidate.layout == temporal::MatrixLayout::Rows ? "rows" : "columns") << " depth tile "
        << candidate.depth_base_tile << " draws " << candidate.draws << " fov "
        << candidate.perspective.vertical_fov * 57.29578f << "deg near "
        << candidate.perspective.near_plane << " aspect " << candidate.aspect
        << (candidate.perspective.depth_inverted ? " inverted" : "") << "\n";
  }
  for (const auto& use : report.late_shaders) {
    out << "  after scene: draw " << use.first_draw << " vs " << hex64(use.vertex_shader) << " ps "
        << hex64(use.pixel_shader) << " x" << use.draws << " colour tile " << use.color_base_tile
        << "\n";
  }
  return out.str();
}

}  // namespace xenon::gpu

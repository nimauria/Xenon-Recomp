#pragma once

// Arithmetic shared by every temporal consumer of a guest frame: sub-pixel
// jitter, the camera matrices it is measured against, and per-pixel camera
// motion reconstructed from depth. Game-independent; a game module only says
// where its camera matrix lives (gpu/temporal_profile.hpp).
//
// Conventions (temporal upscalers are configured to match them):
//   - Jitter: the offset the rendered image is shifted by, in render-resolution
//     pixels, +x right, +y down, each component in [-0.5, 0.5). A sample lands
//     at pixel centre + jitter.
//   - Motion: previous minus current screen position of the surface seen at a
//     pixel, in render-resolution pixels, +x right, +y down, jitter excluded.
//   - Matrices: row-major; clip = M * (x, y, z, 1) as a column vector.
//   - Depth: Direct3D's 0..1 as the guest wrote it (inverted or not, see
//     PerspectiveParameters::depth_inverted).

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace xenon::gpu::temporal {

struct Jitter {
  float x{};
  float y{};
};

// Radical inverse of `index` (1-based) in `base`: base 2 gives 0.5, 0.25,
// 0.75, 0.125, ...
[[nodiscard]] float halton(std::uint32_t index, std::uint32_t base) noexcept;
// Length of the Halton(2, 3) jitter sequence: ceil(8 * (output / render)^2),
// the phase count FSR documents (8 at native resolution) and DLSS and XeSS
// accept.
[[nodiscard]] std::uint32_t jitter_phase_count(std::uint32_t render_width,
                                               std::uint32_t output_width) noexcept;
// The offset for `frame`: Halton(2, 3) at (frame % phase_count) + 1, minus 0.5.
[[nodiscard]] Jitter jitter_offset(std::uint64_t frame, std::uint32_t phase_count) noexcept;

struct Mat4 {
  std::array<float, 16> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  [[nodiscard]] float operator()(int row, int column) const noexcept { return m[row * 4 + column]; }
  [[nodiscard]] float& operator()(int row, int column) noexcept { return m[row * 4 + column]; }
  friend bool operator==(const Mat4&, const Mat4&) = default;
};

// a * b: b is applied first.
[[nodiscard]] Mat4 multiply(const Mat4& a, const Mat4& b) noexcept;
// Empty when `a` is singular.
[[nodiscard]] std::optional<Mat4> invert(const Mat4& a) noexcept;
[[nodiscard]] Mat4 translation(float x, float y, float z) noexcept;

// How four consecutive float4 shader constants hold a matrix. Rows: the shader
// computes clip.i = dot(c[first + i], p) (dp4 per row). Columns: it computes
// clip = p.x * c[first] + p.y * c[first + 1] + p.z * c[first + 2] + p.w * c[first + 3]
// (a row vector times the matrix), so the constants are the transpose.
enum class MatrixLayout : std::uint8_t { Rows, Columns };

[[nodiscard]] Mat4 matrix_from_constants(std::span<const float, 16> constants,
                                         MatrixLayout layout) noexcept;

// What a perspective view-projection matrix says about its camera. Depth at
// view distance w is depth_at_infinity + c / w; the near plane is where depth
// reaches 0 (1 when inverted), the far plane where it reaches 1 (0 when
// inverted).
struct PerspectiveParameters {
  float vertical_fov{};  // radians
  float near_plane{};
  float far_plane{};     // meaningful when !infinite_far
  float depth_at_infinity{};
  bool infinite_far{};
  bool depth_inverted{};
};

// Empty when the matrix is not a perspective projection (an orthographic or
// screen-space transform, or a degenerate one).
[[nodiscard]] std::optional<PerspectiveParameters> perspective_parameters(
    const Mat4& view_projection) noexcept;
// The camera's position: the point every clip-space ray passes through.
// Empty when the matrix is not a perspective projection.
[[nodiscard]] std::optional<std::array<float, 3>> camera_position(
    const Mat4& view_projection) noexcept;
// Angle, in degrees, between the viewing directions of two perspective
// view-projection matrices.
[[nodiscard]] float view_direction_change_degrees(const Mat4& a, const Mat4& b) noexcept;

// The matrix a camera-motion pass applies to (u, v, depth, 1) of a pixel of
// this frame (u, v in [0, 1] with v down, depth as the guest wrote it) to get
// the previous frame's clip position of the surface there:
//   previous_view_projection * translate(camera_move) * inverse(view_projection) * screen_to_ndc
// camera_move is this frame's camera position minus the previous one's, for a
// game that renders camera-relative (its view-projection has no translation
// and object positions are relative to the camera); zero otherwise. Empty when
// view_projection is singular.
[[nodiscard]] std::optional<Mat4> reprojection_matrix(
    const Mat4& view_projection, const Mat4& previous_view_projection,
    std::array<float, 3> camera_move = {}) noexcept;

struct Motion {
  float x{};
  float y{};
};

// What the motion pass computes for one pixel of a width x height image.
// Empty when the previous position is behind the camera or not finite.
[[nodiscard]] std::optional<Motion> camera_motion(const Mat4& reprojection, float u, float v,
                                                  float depth, std::uint32_t width,
                                                  std::uint32_t height) noexcept;

}  // namespace xenon::gpu::temporal

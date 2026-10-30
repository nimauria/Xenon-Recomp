#include "xenon/gpu/temporal.hpp"

#include <algorithm>
#include <cmath>

namespace xenon::gpu::temporal {
namespace {

constexpr double kPi = 3.14159265358979323846;

struct Vec3 {
  double x{}, y{}, z{};
};

Vec3 row_xyz(const Mat4& m, int row) {
  return {m(row, 0), m(row, 1), m(row, 2)};
}
double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double length(Vec3 v) { return std::sqrt(dot(v, v)); }

}  // namespace

float halton(std::uint32_t index, std::uint32_t base) noexcept {
  if (base < 2u) return 0.0f;
  double result = 0.0;
  double fraction = 1.0;
  while (index > 0u) {
    fraction /= base;
    result += fraction * (index % base);
    index /= base;
  }
  return static_cast<float>(result);
}

std::uint32_t jitter_phase_count(std::uint32_t render_width, std::uint32_t output_width) noexcept {
  if (render_width == 0u || output_width == 0u) return 8u;
  const double ratio = static_cast<double>(output_width) / render_width;
  return std::max(1u, static_cast<std::uint32_t>(std::ceil(8.0 * ratio * ratio - 1e-9)));
}

Jitter jitter_offset(std::uint64_t frame, std::uint32_t phase_count) noexcept {
  const auto index = static_cast<std::uint32_t>(frame % std::max(1u, phase_count)) + 1u;
  return {halton(index, 2u) - 0.5f, halton(index, 3u) - 0.5f};
}

Mat4 multiply(const Mat4& a, const Mat4& b) noexcept {
  Mat4 out{};
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      double sum = 0.0;
      for (int k = 0; k < 4; ++k) sum += static_cast<double>(a(r, k)) * b(k, c);
      out(r, c) = static_cast<float>(sum);
    }
  }
  return out;
}

std::optional<Mat4> invert(const Mat4& a) noexcept {
  // Gauss-Jordan elimination with partial pivoting, in double precision.
  double w[4][8]{};
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) w[r][c] = a(r, c);
    w[r][4 + r] = 1.0;
  }
  double scale = 0.0;
  for (const float v : a.m) scale = std::max(scale, std::fabs(static_cast<double>(v)));
  if (!(scale > 0.0) || !std::isfinite(scale)) return std::nullopt;
  for (int column = 0; column < 4; ++column) {
    int pivot = column;
    for (int r = column + 1; r < 4; ++r)
      if (std::fabs(w[r][column]) > std::fabs(w[pivot][column])) pivot = r;
    if (std::fabs(w[pivot][column]) <= scale * 1e-12) return std::nullopt;
    if (pivot != column)
      for (int c = 0; c < 8; ++c) std::swap(w[pivot][c], w[column][c]);
    const double divisor = w[column][column];
    for (int c = 0; c < 8; ++c) w[column][c] /= divisor;
    for (int r = 0; r < 4; ++r) {
      if (r == column) continue;
      const double factor = w[r][column];
      if (factor == 0.0) continue;
      for (int c = 0; c < 8; ++c) w[r][c] -= factor * w[column][c];
    }
  }
  Mat4 out{};
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out(r, c) = static_cast<float>(w[r][4 + c]);
  return out;
}

Mat4 translation(float x, float y, float z) noexcept {
  Mat4 out{};
  out(0, 3) = x;
  out(1, 3) = y;
  out(2, 3) = z;
  return out;
}

Mat4 matrix_from_constants(std::span<const float, 16> constants, MatrixLayout layout) noexcept {
  Mat4 out{};
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      const float value = constants[static_cast<std::size_t>(i * 4 + j)];
      if (layout == MatrixLayout::Rows) {
        out(i, j) = value;
      } else {
        out(j, i) = value;
      }
    }
  }
  return out;
}

std::optional<PerspectiveParameters> perspective_parameters(const Mat4& view_projection) noexcept {
  for (const float v : view_projection.m)
    if (!std::isfinite(v)) return std::nullopt;
  // clip.w = dot(row 3, p) is the view distance (times a scale that the
  // divide cancels); normalise every row by its xyz length.
  const auto forward = row_xyz(view_projection, 3);
  const double forward_length = length(forward);
  if (forward_length < 1e-8) return std::nullopt;  // w constant: not perspective
  const auto row = [&](int r) {
    return Vec3{view_projection(r, 0) / forward_length, view_projection(r, 1) / forward_length,
                view_projection(r, 2) / forward_length};
  };
  const auto w_row = row(3);
  const double w_offset = view_projection(3, 3) / forward_length;
  const auto z_row = row(2);
  const double z_offset = view_projection(2, 3) / forward_length;
  // A perspective projection's clip z is k * w + c: its xyz coefficients are
  // proportional to clip w's.
  const double k = dot(z_row, w_row);
  const Vec3 residual{z_row.x - k * w_row.x, z_row.y - k * w_row.y, z_row.z - k * w_row.z};
  if (length(residual) > 1e-3 * std::max(1.0, std::fabs(k))) return std::nullopt;
  const double c = z_offset - k * w_offset;
  if (std::fabs(c) < 1e-9) return std::nullopt;
  const double y_scale = length(row(1));
  const double x_scale = length(row(0));
  if (y_scale < 1e-6 || x_scale < 1e-6) return std::nullopt;

  PerspectiveParameters out{};
  out.vertical_fov = static_cast<float>(2.0 * std::atan(1.0 / y_scale));
  out.depth_at_infinity = static_cast<float>(k);
  out.depth_inverted = c > 0.0;
  double near_plane = 0.0;
  double far_plane = 0.0;
  if (!out.depth_inverted) {
    // depth 0 at the near plane, 1 at the far plane.
    if (k <= 0.0) return std::nullopt;
    near_plane = -c / k;
    out.infinite_far = std::fabs(1.0 - k) < 1e-6;
    if (!out.infinite_far) far_plane = c / (1.0 - k);
  } else {
    // depth 1 at the near plane, 0 at the far plane.
    if (k >= 1.0) return std::nullopt;
    near_plane = c / (1.0 - k);
    out.infinite_far = std::fabs(k) < 1e-6;
    if (!out.infinite_far) far_plane = -c / k;
  }
  if (!(near_plane > 0.0)) return std::nullopt;
  if (!out.infinite_far && !(far_plane > near_plane)) return std::nullopt;
  out.near_plane = static_cast<float>(near_plane);
  out.far_plane = static_cast<float>(far_plane);
  return out;
}

std::optional<std::array<float, 3>> camera_position(const Mat4& view_projection) noexcept {
  if (!perspective_parameters(view_projection)) return std::nullopt;
  // The camera centre C has clip x = y = w = 0 (only clip z, a constant, is
  // left), so (C, 1) is proportional to inverse * (0, 0, 1, 0).
  const auto inverse = invert(view_projection);
  if (!inverse) return std::nullopt;
  const double h[4] = {(*inverse)(0, 2), (*inverse)(1, 2), (*inverse)(2, 2), (*inverse)(3, 2)};
  if (std::fabs(h[3]) < 1e-12) return std::nullopt;
  return std::array<float, 3>{static_cast<float>(h[0] / h[3]), static_cast<float>(h[1] / h[3]),
                              static_cast<float>(h[2] / h[3])};
}

float view_direction_change_degrees(const Mat4& a, const Mat4& b) noexcept {
  const auto fa = row_xyz(a, 3);
  const auto fb = row_xyz(b, 3);
  const double la = length(fa);
  const double lb = length(fb);
  if (la < 1e-12 || lb < 1e-12) return 180.0f;
  const double cosine = std::clamp(dot(fa, fb) / (la * lb), -1.0, 1.0);
  return static_cast<float>(std::acos(cosine) * 180.0 / kPi);
}

std::optional<Mat4> reprojection_matrix(const Mat4& view_projection,
                                        const Mat4& previous_view_projection,
                                        std::array<float, 3> camera_move) noexcept {
  const auto inverse = invert(view_projection);
  if (!inverse) return std::nullopt;
  Mat4 screen_to_ndc{};
  screen_to_ndc(0, 0) = 2.0f;
  screen_to_ndc(0, 3) = -1.0f;
  screen_to_ndc(1, 1) = -2.0f;
  screen_to_ndc(1, 3) = 1.0f;
  const auto move = translation(camera_move[0], camera_move[1], camera_move[2]);
  return multiply(multiply(previous_view_projection, move), multiply(*inverse, screen_to_ndc));
}

std::optional<Motion> camera_motion(const Mat4& reprojection, float u, float v, float depth,
                                    std::uint32_t width, std::uint32_t height) noexcept {
  const double p[4] = {u, v, depth, 1.0};
  double clip[4]{};
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) clip[r] += static_cast<double>(reprojection(r, c)) * p[c];
  }
  if (!(clip[3] > 1e-9) || !std::isfinite(clip[0]) || !std::isfinite(clip[1])) return std::nullopt;
  const double previous_u = clip[0] / clip[3] * 0.5 + 0.5;
  const double previous_v = 0.5 - clip[1] / clip[3] * 0.5;
  return Motion{static_cast<float>((previous_u - u) * width),
                static_cast<float>((previous_v - v) * height)};
}

}  // namespace xenon::gpu::temporal

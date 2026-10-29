#pragma once

#include <cstdint>
#include <string>

#include "xenon/gpu/presentation.hpp"
#include "xenon/gpu/shader_translation.hpp"

namespace xenon::gpu {

// How the scan-out shader turns one stored front-buffer texel into RGBA. One
// value per host format prepare_presentation_frame() can convert.
enum class ScanoutDecode : std::uint32_t {
  R8,
  R8G8,
  R8G8B8A8,
  B5G5R5A1,
  B5G6R5,
  B4G4R4A4,
  R10G10B10A2,
  R16,
  R16G16,
  R16G16B16A16,
  R16Float,
  R16G16Float,
  R16G16B16A16Float,
  R32Float,
  R32G32Float,
  R32G32B32Float,
  R32G32B32A32Float,
};

// Root/push constants of the scan-out shader. Layout is shared with the HLSL
// in make_scanout_shader(); keep both in step.
struct ScanoutConstants {
  std::uint32_t guest_base{};       // physical address of the base level
  std::uint32_t pitch_blocks{};     // storage pitch, in texels
  std::uint32_t bytes_per_block{};
  std::uint32_t flags{};            // bit 0 tiled, bits 1-2 endian, bit 3 sRGB target
  std::uint32_t decode{};           // ScanoutDecode
  std::uint32_t swizzle{};          // fetch-constant swizzle, 3 bits per component
  std::uint32_t source_width{};
  std::uint32_t source_height{};
  std::int32_t offset_x{};          // letterboxed image rectangle in the target
  std::int32_t offset_y{};
  std::uint32_t draw_width{};
  std::uint32_t draw_height{};
  std::uint32_t target_width{};
  std::uint32_t target_height{};
  std::uint32_t reserved0{};
  std::uint32_t reserved1{};
};
static_assert(sizeof(ScanoutConstants) == 64);

inline constexpr std::uint32_t kScanoutTiled = 1u << 0u;
inline constexpr std::uint32_t kScanoutEndianShift = 1u;
inline constexpr std::uint32_t kScanoutSrgbTarget = 1u << 3u;

// Everything a backend needs to draw one front buffer into a host target:
// the shader constants and the guest bytes the shader reads, which must be
// current in the GPU guest-memory mirror first.
struct ScanoutPlan {
  ScanoutConstants constants{};
  std::uint32_t guest_address{};
  std::uint32_t guest_size{};
  bool valid{};
  std::string error{};
};

// Plans scan-out of a 2D front buffer to a target_width x target_height host
// image, letterboxed when preserve_aspect_ratio is set. srgb_target tells the
// shader the target re-encodes its output, so stored bytes come out unchanged.
[[nodiscard]] ScanoutPlan plan_scanout(const PresentationFrame& frame,
                                       std::uint32_t target_width,
                                       std::uint32_t target_height,
                                       bool preserve_aspect_ratio,
                                       bool srgb_target = false);

// The scan-out pixel shader. It reads the front buffer straight from the
// guest-memory mirror (t0, a raw buffer of physical memory), untiles it,
// applies the fetch-constant endian swap, format conversion and swizzle, and
// filters it to the target with a Catmull-Rom bicubic. Pair it with
// make_transfer_fullscreen_vertex_shader().
[[nodiscard]] LoweredShader make_scanout_shader();

// The scan-out filter's taps and weights for one target coordinate, shared by
// the shader text above and the CPU reference in prepare_presentation_frame().
struct ScanoutTaps {
  std::int32_t first{};  // source index of the first of four taps
  float weights[4]{};
};
[[nodiscard]] ScanoutTaps scanout_taps(std::int32_t target, std::int32_t offset,
                                       std::uint32_t draw_extent,
                                       std::uint32_t source_extent) noexcept;

}  // namespace xenon::gpu

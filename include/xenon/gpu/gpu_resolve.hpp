#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "xenon/gpu/edram_surface.hpp"
#include "xenon/gpu/resource_ir.hpp"
#include "xenon/gpu/shader_translation.hpp"

namespace xenon::gpu {

// A color resolve done on the GPU: a compute pass reads the resolve
// rectangle from the native image that owns its EDRAM tiles, through the same
// EDRAM address mapping a direct tile transfer uses, converts each pixel to
// the destination format and writes it, tiled and endian-swapped, into the
// guest-memory mirror. The CPU never reads, converts or writes the pixels.
//
// Root/push constants; layout shared with make_gpu_resolve_shader().
struct GpuResolveConstants {
  // The EDRAM mapping from the resolve's surface (destination side) to the
  // owner image (source side), as make_edram_transfer_constants() builds it.
  EdramTransferConstants transfer{};
  std::uint32_t left{};
  std::uint32_t top{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t destination_base{};
  std::uint32_t destination_pitch{};   // tiled storage pitch, 32-aligned
  std::uint32_t destination_format{};  // GpuResolvePack
  std::uint32_t source_storage{};      // GpuResolveSource
  std::uint32_t sample_mask{};         // guest samples of the resolve surface
  std::uint32_t flags{};               // see kGpuResolve* below
  std::int32_t exponent_bias{};
  std::uint32_t reserved{};
};
static_assert(sizeof(GpuResolveConstants) == 96);

inline constexpr std::uint32_t kGpuResolveConvert = 1u << 0u;
inline constexpr std::uint32_t kGpuResolveRedBlueSwap = 1u << 1u;
inline constexpr std::uint32_t kGpuResolveEndianShift = 2u;  // three bits, Endian128
inline constexpr std::uint32_t kGpuResolveNativeAverage = 1u << 5u;

// How the shader reads the owner image: its host storage.
enum class GpuResolveSource : std::uint32_t {
  R8G8B8A8Unorm,
  R10G10B10A2Unorm,
  R16G16Float,
  R16G16B16A16Float,
  R32Float,
  R32G32Float,
};

// What the shader writes per pixel.
enum class GpuResolvePack : std::uint32_t {
  RawHost,       // the host storage bits, for a Raw copy
  R8G8B8A8,      // k_8_8_8_8 and its _A / _AS_16_16_16_16 variants
  R10G10B10A2,   // k_2_10_10_10 and _AS_16_16_16_16
  R16G16,        // k_16_16 unorm
  R16G16B16A16,  // k_16_16_16_16 unorm
  R32Float,
  R32G32Float,
  R32G32B32A32Float,
};

struct GpuResolvePlan {
  GpuResolveConstants constants{};
  std::uint32_t bytes_per_pixel{};
  // Guest bytes the pass may write: whole 4 KiB tile rows covering the
  // rectangle's 32x32 macro tiles.
  std::uint32_t range_address{};
  std::uint32_t range_size{};
  bool valid{};
  std::string error{};
};

// Plans a GPU color resolve of `plan` (a planned color resolve of a surface
// whose format is `format`) out of `owner`, the native image that owns every
// EDRAM tile the rectangle covers. Returns an invalid plan, with the reason,
// when the GPU pass cannot reproduce the resolve exactly; the caller then
// resolves through canonical EDRAM.
[[nodiscard]] GpuResolvePlan plan_gpu_color_resolve(
    const ResolvePlan& plan, ColorRenderTargetFormat format,
    const EdramSurfaceLayout& resolve_surface, const EdramSurfaceLayout& owner,
    MsaaSamples owner_host_samples, ColorRenderTargetFormat owner_format);

// The resolve compute shader for an owner image with `owner_host_samples`
// host samples (8x8 threads, one per destination pixel).
[[nodiscard]] LoweredShader make_gpu_resolve_shader(MsaaSamples owner_host_samples);

// A resolve's color clear done on the owner image instead of in canonical
// EDRAM: a sample-frequency draw into the owner that writes the clear color
// to each sample whose EDRAM word lies in the resolve rectangle (of the
// resolve surface) and leaves the rest. Layout shared with
// make_edram_clear_shader().
struct GpuClearConstants {
  // Owner image (destination side) to resolve surface (source side).
  EdramTransferConstants transfer{};
  std::uint32_t left{};
  std::uint32_t top{};
  std::uint32_t right{};
  std::uint32_t bottom{};
  float color[4]{};  // the clear words decoded in the owner's format
};
static_assert(sizeof(GpuClearConstants) == 80);

// Plans the clear; invalid when the owner's host storage can't hold the
// clear words exactly as canonical EDRAM would.
[[nodiscard]] bool plan_gpu_color_clear(const ResolvePlan& plan, ColorRenderTargetFormat format,
                                        const EdramSurfaceLayout& resolve_surface,
                                        const EdramSurfaceLayout& owner,
                                        MsaaSamples owner_host_samples,
                                        GpuClearConstants& constants);

[[nodiscard]] LoweredShader make_edram_clear_shader();

}  // namespace xenon::gpu

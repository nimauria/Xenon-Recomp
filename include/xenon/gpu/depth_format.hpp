#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "xenon/gpu/edram_surface.hpp"

namespace xenon::gpu {

// Xenos D24FS8 uses a positive 24-bit floating depth value with a 20-bit
// mantissa and a 4-bit exponent (20e4). The representable range is [0, 2).
enum class Float20e4Rounding : std::uint8_t {
  Truncate,
  NearestEven,
};

inline constexpr std::uint32_t kFloat20e4Mask = 0x00FFFFFFu;

struct DepthStencilSample {
  float depth{};
  std::uint8_t stencil{};
};

[[nodiscard]] std::uint32_t float32_to_20e4(
    float value,
    Float20e4Rounding rounding = Float20e4Rounding::NearestEven) noexcept;
[[nodiscard]] float float20e4_to_float32(std::uint32_t value) noexcept;
[[nodiscard]] float quantize_float20e4(
    float value,
    Float20e4Rounding rounding = Float20e4Rounding::NearestEven) noexcept;

// Converts between the canonical 32-bit Xenos EDRAM representation and a
// native depth/stencil value. Keeping the packed word authoritative makes both
// D24S8 and D24FS8 reversible even when a host API exposes float32 depth.
//
// The word is the hardware's: depth (unorm 24 or 20e4) in bits 31..8 and
// stencil in bits 7..0. RB_DEPTH_CLEAR, EDRAM depth tiles, depth resolves and
// sampled 24_8 / 24_8_FLOAT textures all use it (xenia's depth loads take
// depth as word >> 8), so a clear word copied straight from the register
// unpacks to the depth and stencil the title asked for.
[[nodiscard]] std::uint32_t pack_depth_stencil(
    DepthRenderTargetFormat format, float depth, std::uint8_t stencil,
    Float20e4Rounding rounding = Float20e4Rounding::NearestEven) noexcept;
[[nodiscard]] DepthStencilSample unpack_depth_stencil(
    DepthRenderTargetFormat format, std::uint32_t packed) noexcept;

// How a depth written by a shader (SV_Depth: clears, loads and transfers into
// a native D24S8 image) must encode a 24-bit UNORM code for the attachment to
// store that exact code. D3D and Vulkan leave the FLOAT -> UNORM depth
// conversion loose, and real devices differ:
// - WARP (CI's Windows runners) truncates on the 2^24 - 1 scale: code / 2^24,
//   the sampled value, stores code - 1 (a post-resolve clear of 0x556677
//   came back 0x556676);
// - NVIDIA (measured on Vulkan) rounds on a 2^24 scale below depth 1/16 and
//   on the 2^24 - 1 scale above it: the middle of a code's interval stores
//   code + 1 for every code under 2^20 (0 came back 1), and code / 2^24
//   stores code - 1 for every code from 2^23 up.
// No one value is exact on all of them, so each device is measured when its
// backend starts (calibrate_unorm24_depth_write) and every D24 depth write
// uses the first encoding it stores exactly. Between them the three cover
// truncation and round to nearest on either scale, and NVIDIA's mix.
enum class Unorm24DepthWrite : std::uint8_t {
  // (2 * code + 1 + (code >> 23)) / 2^25: strictly inside (code, code + 0.5)
  // on the 2^24 - 1 scale, so truncating and rounding both give the code.
  Midpoint,
  // code / 2^24: exactly the code on a 2^24 scale (0xFFFFFF stays below 1.0).
  PowerOfTwo,
  // PowerOfTwo below 2^23 and Midpoint from it up: within half a code of the
  // code on either scale, so rounding to nearest on either - or on each over
  // part of the range - gives the code.
  Split,
};
inline constexpr std::array<Unorm24DepthWrite, 3> kUnorm24DepthWrites{
    Unorm24DepthWrite::Midpoint, Unorm24DepthWrite::PowerOfTwo, Unorm24DepthWrite::Split};

[[nodiscard]] std::string_view unorm24_depth_write_name(Unorm24DepthWrite write) noexcept;
// The depth a shader writes for `code` (its low 24 bits) under `write`: an
// exactly representable float, the same value the HLSL of
// unorm24_depth_write_hlsl() computes.
[[nodiscard]] float unorm24_depth_for_write(std::uint32_t code, Unorm24DepthWrite write) noexcept;

// The canonical 24_8 words a device's D24 depth writes are measured with:
// codes spread evenly over the whole range (both halves, where the scales
// part ways), led by the codes at each end and around 2^23, each with its own
// stencil. `count` is at least 16.
[[nodiscard]] std::vector<std::uint32_t> unorm24_depth_calibration_words(std::uint32_t count);

struct Unorm24DepthCalibration {
  Unorm24DepthWrite write{Unorm24DepthWrite::Midpoint};
  // Whether `write` stored every calibration word exactly. When no encoding
  // does, `write` is the one with the fewest wrong words.
  bool exact{};
  std::uint32_t mismatches{};
  std::uint32_t first_expected{};
  std::uint32_t first_stored{};
};

// Measures each encoding in kUnorm24DepthWrites order with
// `store(write, words, stored)` - writing `words` into a D24S8 image as the
// device's depth writes do under `write` and reading the 24_8 words back into
// `stored` - and keeps the first that is exact. Returns nullopt when `store`
// fails (the device could not be measured).
template <class Store>
[[nodiscard]] std::optional<Unorm24DepthCalibration> calibrate_unorm24_depth_write(
    std::span<const std::uint32_t> words, Store&& store) {
  std::optional<Unorm24DepthCalibration> best;
  std::vector<std::uint32_t> stored;
  for (const auto write : kUnorm24DepthWrites) {
    stored.clear();
    if (!store(write, words, stored) || stored.size() != words.size()) return std::nullopt;
    Unorm24DepthCalibration result{write};
    for (std::size_t i = 0; i < words.size(); ++i) {
      if (stored[i] == words[i]) continue;
      if (result.mismatches++ == 0u) {
        result.first_expected = words[i];
        result.first_stored = stored[i];
      }
    }
    result.exact = result.mismatches == 0u;
    if (result.exact) return result;
    if (!best || result.mismatches < best->mismatches) best = result;
  }
  return best;
}

// Maps canonical Xenos depth to the normalized range accepted by native depth
// attachments. D24FS8 uses [0,2), so it is scaled to [0,1) without losing any
// representable bits; the inverse restores the original 20e4 lattice value.
// D24S8 depth is the value a depth write stores as the word's code on this
// device (unorm24_depth_for_write).
[[nodiscard]] DepthStencilSample depth_stencil_to_host(
    DepthRenderTargetFormat format, std::uint32_t packed,
    Unorm24DepthWrite unorm24_write) noexcept;
[[nodiscard]] std::uint32_t host_to_depth_stencil(
    DepthRenderTargetFormat format, float host_depth, std::uint8_t stencil,
    Float20e4Rounding rounding = Float20e4Rounding::NearestEven) noexcept;

// A guest viewport depth bound (RB viewport Z, already clamped to [0, 1]) as
// the host viewport must use it. Draws have to land on the same scale as
// clears, loads and readbacks: for D24FS8 that is half the guest value, and
// the bound is first rounded onto the 20e4 lattice as the reference does, so
// the post-shader clamp cannot produce a depth the format cannot hold.
[[nodiscard]] float host_viewport_depth(DepthRenderTargetFormat format,
                                        float guest_depth) noexcept;

}  // namespace xenon::gpu

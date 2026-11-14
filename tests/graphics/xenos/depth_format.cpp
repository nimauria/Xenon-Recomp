#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "xenon/gpu/depth_format.hpp"

namespace {

void test_known_values() {
  using namespace xenon::gpu;
  assert(float32_to_20e4(0.0f) == 0x000000u);
  assert(float32_to_20e4(-0.0f) == 0x000000u);
  assert(float32_to_20e4(-1.0f) == 0x000000u);
  assert(float32_to_20e4(std::numeric_limits<float>::quiet_NaN()) == 0x000000u);
  assert(float32_to_20e4(0.5f) == 0xE00000u);
  assert(float32_to_20e4(1.0f) == 0xF00000u);
  assert(float32_to_20e4(1.5f) == 0xF80000u);
  assert(float32_to_20e4(2.0f) == 0xFFFFFFu);
  assert(float32_to_20e4(std::numeric_limits<float>::infinity()) == 0xFFFFFFu);

  assert(float20e4_to_float32(0x000000u) == 0.0f);
  assert(float20e4_to_float32(0xE00000u) == 0.5f);
  assert(float20e4_to_float32(0xF00000u) == 1.0f);
  assert(float20e4_to_float32(0xF80000u) == 1.5f);
  assert(float20e4_to_float32(0xFFFFFFu) < 2.0f);
  assert(float20e4_to_float32(0xFFFFFFu) > 1.99999f);
}

void test_denormal_boundaries() {
  using namespace xenon::gpu;
  assert(float32_to_20e4(0x1p-14f) == 0x100000u);
  assert(float32_to_20e4(0x1p-15f) == 0x080000u);
  assert(float32_to_20e4(0x1p-16f) == 0x040000u);
  assert(float20e4_to_float32(0x100000u) == 0x1p-14f);
  assert(float20e4_to_float32(0x080000u) == 0x1p-15f);
  assert(float20e4_to_float32(0x040000u) == 0x1p-16f);
}

void test_rounding_modes() {
  using namespace xenon::gpu;
  // This value lies on a discarded-bit tie where nearest-even advances to the
  // next 20e4 value while truncation stays on the lower lattice point.
  constexpr float kTie = 0.500000298023223876953125f;
  assert(float32_to_20e4(kTie, Float20e4Rounding::Truncate) == 0xE00000u);
  assert(float32_to_20e4(kTie, Float20e4Rounding::NearestEven) == 0xE00001u);
  assert(quantize_float20e4(kTie, Float20e4Rounding::NearestEven) >
         quantize_float20e4(kTie, Float20e4Rounding::Truncate));
}

void test_round_trip_lattice_samples() {
  using namespace xenon::gpu;
  // Sample every 257th code plus the maximum. Every representable 20e4 value
  // must survive expand -> nearest-even encode exactly.
  for (std::uint32_t code = 0; code <= kFloat20e4Mask; code += 257u) {
    const auto expanded = float20e4_to_float32(code);
    assert(float32_to_20e4(expanded) == code);
  }
  assert(float32_to_20e4(float20e4_to_float32(kFloat20e4Mask)) ==
         kFloat20e4Mask);
}

void test_packed_depth_stencil() {
  using namespace xenon::gpu;
  constexpr std::uint8_t kStencil = 0xA7;
  for (const auto code : {0u, 1u, 0x7FFFFFu, 0xFFFFFEu, 0xFFFFFFu}) {
    const auto packed = (code << 8u) | kStencil;
    const auto value = unpack_depth_stencil(DepthRenderTargetFormat::D24S8,
                                             packed);
    assert(value.stencil == kStencil);
    assert(pack_depth_stencil(DepthRenderTargetFormat::D24S8, value.depth,
                              value.stencil) == packed);
  }
  for (const auto code : {0u, 1u, 0x0FFFFFu, 0xE00000u, 0xFFFFFFu}) {
    const auto packed = (code << 8u) | kStencil;
    const auto value = unpack_depth_stencil(DepthRenderTargetFormat::D24FS8,
                                             packed);
    assert(value.stencil == kStencil);
    assert(pack_depth_stencil(DepthRenderTargetFormat::D24FS8, value.depth,
                              value.stencil) == packed);
  }
  assert(pack_depth_stencil(DepthRenderTargetFormat::D24S8, -1.0f, 3) ==
         0x00000003u);
  assert(pack_depth_stencil(DepthRenderTargetFormat::D24S8, 2.0f, 3) ==
         0xFFFFFF03u);
  for (const auto depth : {0.0f, 0.5f, 1.0f, 1.25f, 1.5f, 1.99999f}) {
    const auto packed = pack_depth_stencil(DepthRenderTargetFormat::D24FS8,
                                           depth, 0x5Au);
    const auto unpacked = unpack_depth_stencil(
        DepthRenderTargetFormat::D24FS8, packed);
    assert(unpacked.depth >= 0.0f && unpacked.depth < 2.0f);
    assert(pack_depth_stencil(DepthRenderTargetFormat::D24FS8,
                              unpacked.depth, unpacked.stencil) == packed);
    const auto host = depth_stencil_to_host(
        DepthRenderTargetFormat::D24FS8, packed, Unorm24DepthWrite::Midpoint);
    assert(host.depth >= 0.0f && host.depth < 1.0f);
    // A float attachment's depth does not depend on the D24 encoding.
    assert(depth_stencil_to_host(DepthRenderTargetFormat::D24FS8, packed,
                                 Unorm24DepthWrite::PowerOfTwo).depth == host.depth);
    assert(host_to_depth_stencil(DepthRenderTargetFormat::D24FS8,
                                 host.depth, host.stencil) == packed);
  }
  for (const auto write : kUnorm24DepthWrites) {
    for (const auto code : {0u, 1u, 0x7FFFFFu, 0x800000u, 0xFFFFFFu}) {
      const auto packed = (code << 8u) | 0xC3u;
      const auto host = depth_stencil_to_host(DepthRenderTargetFormat::D24S8, packed, write);
      assert(host.depth == unorm24_depth_for_write(code, write) && host.stencil == 0xC3u);
    }
  }
}

// The ways a device may convert a written depth to a 24-bit UNORM code:
// truncating or rounding to nearest, on the 2^24 - 1 scale the APIs specify
// or the 2^24 scale some devices use - or, as NVIDIA was measured to, on the
// 2^24 scale (half up) below depth 1/16 and the specified one above.
enum class D24Conversion {
  TruncateSpec, RoundSpec, TruncatePow2, RoundPow2, RoundHalfUpPow2, RoundMixedScale
};
std::uint32_t d24_convert(D24Conversion conversion, float depth) {
  const double spec = double(depth) * 16777215.0, pow2 = double(depth) * 16777216.0;
  double code = 0.0;
  switch (conversion) {
    case D24Conversion::TruncateSpec: code = std::floor(spec); break;
    case D24Conversion::RoundSpec: code = std::nearbyint(spec); break;
    case D24Conversion::TruncatePow2: code = std::floor(pow2); break;
    case D24Conversion::RoundPow2: code = std::nearbyint(pow2); break;
    case D24Conversion::RoundHalfUpPow2: code = std::floor(pow2 + 0.5); break;
    case D24Conversion::RoundMixedScale:
      code = depth < 0.0625f ? std::floor(pow2 + 0.5) : std::nearbyint(spec);
      break;
  }
  return std::uint32_t(std::min(code, 16777215.0));
}

// Regression (CI: WARP truncated a post-resolve depth clear of 0x556677 to
// 0x556676; NVIDIA turned the midpoint of code 0 into 1): each encoding
// stores every one of the 2^24 codes exactly on the conversions it is for,
// so a device converting either way has an exact encoding to calibrate to.
void test_unorm24_depth_writes_store_their_codes() {
  using namespace xenon::gpu;
  for (std::uint32_t code = 0; code <= 0xFFFFFFu; ++code) {
    const float midpoint = unorm24_depth_for_write(code, Unorm24DepthWrite::Midpoint);
    assert(d24_convert(D24Conversion::TruncateSpec, midpoint) == code);
    assert(d24_convert(D24Conversion::RoundSpec, midpoint) == code);
    const float pow2 = unorm24_depth_for_write(code, Unorm24DepthWrite::PowerOfTwo);
    assert(d24_convert(D24Conversion::TruncatePow2, pow2) == code);
    assert(d24_convert(D24Conversion::RoundPow2, pow2) == code);
    assert(d24_convert(D24Conversion::RoundHalfUpPow2, pow2) == code);
    const float split = unorm24_depth_for_write(code, Unorm24DepthWrite::Split);
    assert(d24_convert(D24Conversion::RoundSpec, split) == code);
    assert(d24_convert(D24Conversion::RoundMixedScale, split) == code);
    assert(split == (code < 0x800000u ? pow2 : midpoint));
    assert(pow2 < 1.0f && midpoint <= 1.0f);
  }
  // On NVIDIA's mix neither of the others is exact, where it was measured.
  assert(d24_convert(D24Conversion::RoundMixedScale,
                     unorm24_depth_for_write(0u, Unorm24DepthWrite::Midpoint)) == 1u);
  assert(d24_convert(D24Conversion::RoundMixedScale,
                     unorm24_depth_for_write(0x800001u, Unorm24DepthWrite::PowerOfTwo)) ==
         0x800000u);
  // Only the code's low 24 bits count.
  assert(unorm24_depth_for_write(0x1556677u, Unorm24DepthWrite::Midpoint) ==
         unorm24_depth_for_write(0x556677u, Unorm24DepthWrite::Midpoint));
  // The encodings the CI failures came from fail where described.
  const float sampled = float(0x556677u) * (1.0f / 16777216.0f);
  assert(d24_convert(D24Conversion::TruncateSpec, sampled) == 0x556676u);
  assert(d24_convert(D24Conversion::RoundHalfUpPow2,
                     unorm24_depth_for_write(0u, Unorm24DepthWrite::Midpoint)) == 1u);
}

// The calibration words span the whole range - both ends, both sides of 2^23
// - and the selection keeps the first exact encoding, or the least wrong one
// with its first mismatch when none is exact, and reports a failed store.
void test_unorm24_calibration_chooses_the_exact_encoding() {
  using namespace xenon::gpu;
  const auto words = unorm24_depth_calibration_words(66560u);
  assert(words.size() == 66560u);
  bool low = false, high = false, zero = false, full = false;
  std::uint32_t stencils = 0u;
  for (std::size_t i = 0; i < words.size(); ++i) {
    const auto code = words[i] >> 8u;
    low |= code < 0x800000u && code > 0x400000u;
    high |= code >= 0x800000u && code < 0xFFFFFFu;
    zero |= code == 0u;
    full |= code == 0xFFFFFFu;
    if (i > 0 && (words[i] & 0xFFu) != (words[i - 1] & 0xFFu)) ++stencils;
  }
  assert(low && high && zero && full && stencils > words.size() / 2);
  assert(unorm24_depth_calibration_words(3u).size() == 16u);

  const auto device = [&](D24Conversion conversion) {
    return [conversion](Unorm24DepthWrite write, std::span<const std::uint32_t> source,
                        std::vector<std::uint32_t>& stored) {
      for (const auto word : source)
        stored.push_back((d24_convert(conversion, unorm24_depth_for_write(word >> 8u, write))
                          << 8u) |
                         (word & 0xFFu));
      return true;
    };
  };
  for (const auto [conversion, expected] :
       {std::pair{D24Conversion::TruncateSpec, Unorm24DepthWrite::Midpoint},
        std::pair{D24Conversion::RoundSpec, Unorm24DepthWrite::Midpoint},
        std::pair{D24Conversion::TruncatePow2, Unorm24DepthWrite::PowerOfTwo},
        std::pair{D24Conversion::RoundPow2, Unorm24DepthWrite::PowerOfTwo},
        std::pair{D24Conversion::RoundHalfUpPow2, Unorm24DepthWrite::PowerOfTwo},
        std::pair{D24Conversion::RoundMixedScale, Unorm24DepthWrite::Split}}) {
    const auto result = calibrate_unorm24_depth_write(words, device(conversion));
    assert(result && result->exact && result->write == expected && result->mismatches == 0u);
  }
  // A device storing every code one high: nothing is exact; the result is
  // the encoding with the fewest wrong codes, with its first mismatch.
  const auto off_by_one = calibrate_unorm24_depth_write(
      words, [](Unorm24DepthWrite, std::span<const std::uint32_t> source,
                std::vector<std::uint32_t>& stored) {
        for (const auto word : source)
          stored.push_back(((std::min((word >> 8u) + 1u, 0xFFFFFFu)) << 8u) | (word & 0xFFu));
        return true;
      });
  assert(off_by_one && !off_by_one->exact && off_by_one->mismatches > 0u);
  assert(off_by_one->mismatches < words.size());
  assert((off_by_one->first_stored >> 8u) == (off_by_one->first_expected >> 8u) + 1u);
  // A store that fails, or returns the wrong count, is no measurement.
  assert(!calibrate_unorm24_depth_write(
      words, [](Unorm24DepthWrite, std::span<const std::uint32_t>,
                std::vector<std::uint32_t>&) { return false; }));
  assert(!calibrate_unorm24_depth_write(
      words, [](Unorm24DepthWrite, std::span<const std::uint32_t>,
                std::vector<std::uint32_t>& stored) {
        stored.push_back(0u);
        return true;
      }));
}

// Regression (AC6 campaign opening rendered black with no failed draws): a
// post-resolve depth clear copies RB_DEPTH_CLEAR into EDRAM as is, and the
// register holds depth in bits 31..8 and stencil in 7..0. The packed word
// used depth in the low 24 bits and stencil in the top 8, so the common
// "depth 1.0, stencil 0" clear (0xFFFFFF00) read back as stencil 0xFF and
// every stencil-tested pass after it drew nothing.
void test_clear_register_words_unpack_as_written() {
  using namespace xenon::gpu;
  auto value = unpack_depth_stencil(DepthRenderTargetFormat::D24S8, 0xFFFFFF00u);
  assert(value.depth == 1.0f && value.stencil == 0u);
  value = unpack_depth_stencil(DepthRenderTargetFormat::D24S8, 0x00000080u);
  assert(value.depth == 0.0f && value.stencil == 0x80u);
  value = unpack_depth_stencil(DepthRenderTargetFormat::D24S8, 0x80000001u);
  assert(value.stencil == 1u);
  assert(value.depth > 0.5f && value.depth < 0.5001f);
  // 20e4 depth 1.0 is exponent 15 (0xF00000): the word is 0xF0000000 | stencil.
  value = unpack_depth_stencil(DepthRenderTargetFormat::D24FS8, 0xF0000042u);
  assert(value.depth == 1.0f && value.stencil == 0x42u);
  assert(pack_depth_stencil(DepthRenderTargetFormat::D24S8, 1.0f, 0) == 0xFFFFFF00u);
  assert(pack_depth_stencil(DepthRenderTargetFormat::D24FS8, 1.0f, 0x42) == 0xF0000042u);
  // As the reference's load shader decodes sampled 24_8: (d + (d >> 23)) / 2^24
  // with d = word >> 8.
  for (const std::uint32_t word : {0x00000000u, 0x12345600u, 0x7FFFFF00u, 0xFFFFFFFFu}) {
    const auto d = word >> 8u;
    const float expected = float(d + (d >> 23u)) * (1.0f / 16777216.0f);
    assert(unpack_depth_stencil(DepthRenderTargetFormat::D24S8, word).depth == expected);
  }
}

// Regression (AC6 black campaign opening, D24FS8 depth copies all zero):
// clears and loads stored half the guest depth on the host while draws used
// the full guest viewport range, so geometry was tested against a clear at the
// wrong scale. A guest depth must reach the host at the same value whether it
// arrives as a clear/load or as the viewport bound of a draw.
void test_viewport_depth_matches_the_host_scale() {
  using namespace xenon::gpu;
  for (const float guest : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
    const auto cleared = depth_stencil_to_host(
        DepthRenderTargetFormat::D24FS8,
        pack_depth_stencil(DepthRenderTargetFormat::D24FS8, guest, 0),
        Unorm24DepthWrite::Midpoint).depth;
    assert(host_viewport_depth(DepthRenderTargetFormat::D24FS8, guest) == cleared);
    assert(host_viewport_depth(DepthRenderTargetFormat::D24S8, guest) == guest);
  }
  assert(host_viewport_depth(DepthRenderTargetFormat::D24FS8, 1.0f) == 0.5f);
  // A bound between lattice points is rounded onto the 20e4 lattice first.
  const float between = 0.3f;
  assert(host_viewport_depth(DepthRenderTargetFormat::D24FS8, between) ==
         quantize_float20e4(between) * 0.5f);
}

}  // namespace

int main() {
  test_known_values();
  test_denormal_boundaries();
  test_rounding_modes();
  test_round_trip_lattice_samples();
  test_packed_depth_stencil();
  test_clear_register_words_unpack_as_written();
  test_unorm24_depth_writes_store_their_codes();
  test_unorm24_calibration_chooses_the_exact_encoding();
  test_viewport_depth_matches_the_host_scale();
  std::cout << "xenon_depth_format_tests: ok\n";
}

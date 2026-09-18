#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>

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
    const auto packed = code | (std::uint32_t(kStencil) << 24u);
    const auto value = unpack_depth_stencil(DepthRenderTargetFormat::D24S8,
                                             packed);
    assert(value.stencil == kStencil);
    assert(pack_depth_stencil(DepthRenderTargetFormat::D24S8, value.depth,
                              value.stencil) == packed);
  }
  for (const auto code : {0u, 1u, 0x0FFFFFu, 0xE00000u, 0xFFFFFFu}) {
    const auto packed = code | (std::uint32_t(kStencil) << 24u);
    const auto value = unpack_depth_stencil(DepthRenderTargetFormat::D24FS8,
                                             packed);
    assert(value.stencil == kStencil);
    assert(pack_depth_stencil(DepthRenderTargetFormat::D24FS8, value.depth,
                              value.stencil) == packed);
  }
  assert(pack_depth_stencil(DepthRenderTargetFormat::D24S8, -1.0f, 3) ==
         0x03000000u);
  assert(pack_depth_stencil(DepthRenderTargetFormat::D24S8, 2.0f, 3) ==
         0x03FFFFFFu);
  for (const auto depth : {0.0f, 0.5f, 1.0f, 1.25f, 1.5f, 1.99999f}) {
    const auto packed = pack_depth_stencil(DepthRenderTargetFormat::D24FS8,
                                           depth, 0x5Au);
    const auto unpacked = unpack_depth_stencil(
        DepthRenderTargetFormat::D24FS8, packed);
    assert(unpacked.depth >= 0.0f && unpacked.depth < 2.0f);
    assert(pack_depth_stencil(DepthRenderTargetFormat::D24FS8,
                              unpacked.depth, unpacked.stencil) == packed);
    const auto host = depth_stencil_to_host(
        DepthRenderTargetFormat::D24FS8, packed);
    assert(host.depth >= 0.0f && host.depth < 1.0f);
    assert(host_to_depth_stencil(DepthRenderTargetFormat::D24FS8,
                                 host.depth, host.stencil) == packed);
  }
  for (const auto code : {0u, 1u, 0x7FFFFFu, 0x800000u, 0xFFFFFFu}) {
    const auto packed = code | 0xC3000000u;
    const auto host = depth_stencil_to_host(
        DepthRenderTargetFormat::D24S8, packed);
    assert(host_to_depth_stencil(DepthRenderTargetFormat::D24S8,
                                 host.depth, host.stencil) == packed);
  }
}

}  // namespace

int main() {
  test_known_values();
  test_denormal_boundaries();
  test_rounding_modes();
  test_round_trip_lattice_samples();
  test_packed_depth_stencil();
  std::cout << "xenon_depth_format_tests: ok\n";
}

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <set>
#include <vector>

#include "xenon/gpu/edram_surface.hpp"

using namespace xenon::gpu;

int main() {
  EdramSurfaceLayout single{2040, 1280, 720, MsaaSamples::X1, false, false};
  assert(single.sample_width() == 1280 && single.sample_height() == 720);
  assert(single.pitch_tiles() == 16 && single.tile_rows() == 45);
  assert(single.tile_count() == 720);
  auto shorter = single;
  shorter.height_pixels = 360;
  assert(single.identity_hash() == shorter.identity_hash());
  assert(single.hash() != shorter.hash());
  shorter.base_tile++;
  assert(single.identity_hash() != shorter.identity_hash());

  EdramSurfaceLayout msaa2{1008, 1120, 720, MsaaSamples::X2, false, true};
  assert(msaa2.sample_width() == 1120 && msaa2.sample_height() == 1440);
  assert(msaa2.pitch_tiles() == 14 && msaa2.tile_rows() == 90);
  assert(msaa2.tile_count() == 1260);

  EdramSurfaceLayout msaa4{0, 1280, 720, MsaaSamples::X4, true, false};
  assert(msaa4.sample_width() == 2560 && msaa4.sample_height() == 1440);
  assert(msaa4.pitch_tiles() == 64 && msaa4.tile_rows() == 90);

  const auto sample_1x = map_guest_sample_to_host(MsaaSamples::X1, 0);
  assert(sample_1x && sample_1x->sample == 0 && sample_1x->sample_count == 1);
  const auto sample_2x_0 = map_guest_sample_to_host(MsaaSamples::X2, 0);
  const auto sample_2x_1 = map_guest_sample_to_host(MsaaSamples::X2, 1);
  assert(sample_2x_0 && sample_2x_0->sample == 1 &&
         sample_2x_0->sample_count == 2);
  assert(sample_2x_1 && sample_2x_1->sample == 0 &&
         sample_2x_1->sample_count == 2);
  const auto sample_2x_fallback_0 =
      map_guest_sample_to_host(MsaaSamples::X2, 0, false);
  const auto sample_2x_fallback_1 =
      map_guest_sample_to_host(MsaaSamples::X2, 1, false);
  assert(sample_2x_fallback_0 && sample_2x_fallback_0->sample == 0 &&
         sample_2x_fallback_0->sample_count == 4);
  assert(sample_2x_fallback_1 && sample_2x_fallback_1->sample == 3 &&
         sample_2x_fallback_1->sample_count == 4);
  for (std::uint32_t sample = 0; sample < 4; ++sample) {
    const auto mapped = map_guest_sample_to_host(MsaaSamples::X4, sample);
    assert(mapped && mapped->sample == sample && mapped->sample_count == 4);
  }

  assert(storage_color_format(ColorRenderTargetFormat::R10G10B10A2As10) ==
         ColorRenderTargetFormat::R10G10B10A2);
  assert(storage_color_format(
             ColorRenderTargetFormat::R10G10B10A2FloatAs16) ==
         ColorRenderTargetFormat::R10G10B10A2Float);
  for (std::uint32_t value = 0; value < 256; ++value) {
    const auto packed = value | (value << 8u) | (value << 16u) | (value << 24u);
    const auto repacked = pack_color_sample(
        ColorRenderTargetFormat::R8G8B8A8Gamma,
        unpack_color_sample(ColorRenderTargetFormat::R8G8B8A8Gamma,
                            {packed, 0}))[0];
    for (unsigned channel = 0; channel < 4; ++channel) {
      const auto expected = int((packed >> (channel * 8u)) & 0xFFu);
      const auto actual = int((repacked >> (channel * 8u)) & 0xFFu);
      assert(std::abs(expected - actual) <= 1);
    }
  }
  for (std::uint32_t code = 0; code < 1024; ++code)
    assert(float32_to_7e3(float7e3_to_float32(code)) == code);
  const std::array<std::uint32_t, 2> fixed_bits{0x7FFFFFFFu, 0xFC000400u};
  assert(pack_color_sample(ColorRenderTargetFormat::R16G16B16A16Fixed,
                           unpack_color_sample(
                               ColorRenderTargetFormat::R16G16B16A16Fixed,
                               fixed_bits)) == fixed_bits);
  const std::array<std::uint32_t, 2> half_bits{0xBC003C00u, 0x40003800u};
  assert(pack_color_sample(ColorRenderTargetFormat::R16G16B16A16Float,
                           unpack_color_sample(
                               ColorRenderTargetFormat::R16G16B16A16Float,
                               half_bits)) == half_bits);
  const std::uint32_t float7e3_bits =
      0xC0000000u | 0x3FFu | (0x155u << 10u) | (0x2AAu << 20u);
  assert(pack_color_sample(ColorRenderTargetFormat::R10G10B10A2Float,
                           unpack_color_sample(
                               ColorRenderTargetFormat::R10G10B10A2Float,
                               {float7e3_bits, 0}))[0] == float7e3_bits);
  assert(color_host_storage(ColorRenderTargetFormat::R8G8B8A8Gamma) ==
         ColorHostStorage::R16G16B16A16Unorm);
  assert(color_host_storage(ColorRenderTargetFormat::R16G16Fixed) ==
         ColorHostStorage::R16G16Float);
  assert(color_host_storage(ColorRenderTargetFormat::R10G10B10A2Float) ==
         ColorHostStorage::R16G16B16A16Float);
  for (const auto format : {
           ColorRenderTargetFormat::R8G8B8A8,
           ColorRenderTargetFormat::R8G8B8A8Gamma,
           ColorRenderTargetFormat::R10G10B10A2,
           ColorRenderTargetFormat::R10G10B10A2Float,
           ColorRenderTargetFormat::R16G16Fixed,
           ColorRenderTargetFormat::R16G16B16A16Fixed,
           ColorRenderTargetFormat::R16G16Float,
           ColorRenderTargetFormat::R16G16B16A16Float,
           ColorRenderTargetFormat::R32Float,
           ColorRenderTargetFormat::R32G32Float}) {
    const auto wide_sample = color_render_target_is_64bpp(format);
    const auto canonical_bytes = wide_sample ? 8u : 4u;
    const auto host_bytes = color_host_bytes_per_pixel(format);
    const ColorSample source_sample{{0.25f, 0.5f, 0.75f, 1.0f}};
    const auto packed = pack_color_sample(format, source_sample);
    std::array<std::byte, 16> canonical{};
    std::memcpy(canonical.data(), packed.data(), canonical_bytes);
    std::array<std::byte, 16> host{};
    std::array<std::byte, 16> returned{};
    assert(edram_color_to_host(format, 1, 1, canonical, canonical_bytes,
                               host, host_bytes));
    assert(host_color_to_edram(format, 1, 1, host, host_bytes, returned,
                               canonical_bytes));
    assert(std::memcmp(canonical.data(), returned.data(), canonical_bytes) == 0);
  }

  // Selected Xenos color resolves average Samples01 / Samples23 only. Verify
  // the common host-storage averaging path without relying on a full native
  // MSAA resolve, including a widened transport format.
  for (const auto format : {ColorRenderTargetFormat::R8G8B8A8,
                            ColorRenderTargetFormat::R8G8B8A8Gamma,
                            ColorRenderTargetFormat::R16G16Float,
                            ColorRenderTargetFormat::R32G32Float}) {
    const auto bytes_per_pixel = color_host_bytes_per_pixel(format);
    std::vector<std::byte> first(bytes_per_pixel * 2u);
    std::vector<std::byte> second(bytes_per_pixel * 2u);
    const ColorSample first_left{{0.0f, 0.25f, 0.5f, 1.0f}};
    const ColorSample second_left{{1.0f, 0.75f, 0.5f, 0.0f}};
    const ColorSample first_right{{0.25f, 0.0f, 1.0f, 0.5f}};
    const ColorSample second_right{{0.75f, 1.0f, 0.0f, 0.5f}};
    assert(encode_host_color_sample(
        format, first_left,
        std::span(first).subspan(0, bytes_per_pixel)));
    assert(encode_host_color_sample(
        format, first_right,
        std::span(first).subspan(bytes_per_pixel, bytes_per_pixel)));
    assert(encode_host_color_sample(
        format, second_left,
        std::span(second).subspan(0, bytes_per_pixel)));
    assert(encode_host_color_sample(
        format, second_right,
        std::span(second).subspan(bytes_per_pixel, bytes_per_pixel)));

    std::vector<std::byte> averaged;
    std::uint32_t averaged_pitch{};
    assert(average_host_color_samples(
        format, 2, 1, first, bytes_per_pixel * 2u, second,
        bytes_per_pixel * 2u, averaged, averaged_pitch));
    assert(averaged_pitch == bytes_per_pixel * 2u);
    ColorSample left{};
    ColorSample right{};
    assert(decode_host_color_sample(
        format, std::span(averaged).subspan(0, bytes_per_pixel), left));
    assert(decode_host_color_sample(
        format,
        std::span(averaged).subspan(bytes_per_pixel, bytes_per_pixel), right));
    const auto close = [](float actual, float expected) {
      return std::abs(actual - expected) <= 0.01f;
    };
    assert(close(left.components[0], 0.5f));
    assert(close(left.components[1], 0.5f));
    assert(close(right.components[0], 0.5f));
    assert(close(right.components[1], 0.5f));
    if (format == ColorRenderTargetFormat::R8G8B8A8 ||
        format == ColorRenderTargetFormat::R8G8B8A8Gamma) {
      assert(close(left.components[2], 0.5f));
      assert(close(left.components[3], 0.5f));
      assert(close(right.components[2], 0.5f));
      assert(close(right.components[3], 0.5f));
    } else {
      assert(close(left.components[2], 0.0f));
      assert(close(left.components[3], 1.0f));
      assert(close(right.components[2], 0.0f));
      assert(close(right.components[3], 1.0f));
    }
  }
  assert(!map_guest_sample_to_host(MsaaSamples::X1, 1));
  assert(!map_guest_sample_to_host(MsaaSamples::X4, 4));

  Edram edram;
  EdramSurfaceLayout layout{2047, 80, 16, MsaaSamples::X4, false, false};
  std::set<std::uint32_t> addresses;
  for (std::uint32_t sample = 0; sample < 4; ++sample) {
    const auto address = edram_sample_address(layout, 0, 0, sample);
    assert(address && addresses.insert(address->low).second);
    assert(write_edram_sample(edram, layout, 0, 0,
                              {0xA0000000u | sample, 0}, sample));
    assert(read_edram_sample(edram, layout, 0, 0, sample)[0] ==
           (0xA0000000u | sample));
  }

  EdramSurfaceLayout wide{2047, 80, 16, MsaaSamples::X1, true, false};
  const auto wide_address = edram_sample_address(wide, 0, 0);
  assert(wide_address && wide_address->has_high);
  assert(wide_address->low / Edram::kTileBytes == 2047);
  assert(wide_address->high / Edram::kTileBytes == 0);
  assert(write_edram_sample(edram, wide, 0, 0,
                            {0x11223344u, 0x55667788u}));
  assert((read_edram_sample(edram, wide, 0, 0) ==
          std::array<std::uint32_t, 2>{0x11223344u, 0x55667788u}));

  EdramSurfaceLayout depth{0, 80, 16, MsaaSamples::X1, false, true};
  const auto color_address = edram_sample_address(
      EdramSurfaceLayout{0, 80, 16, MsaaSamples::X1, false, false}, 0, 0);
  const auto depth_address = edram_sample_address(depth, 0, 0);
  assert(color_address && depth_address);
  assert(depth_address->low == color_address->low + 40u * 4u);

  EdramSurfaceLayout resolve_layout{3, 8, 2, MsaaSamples::X1, false, false};
  for (std::uint32_t y = 0; y < 2; ++y)
    for (std::uint32_t x = 0; x < 8; ++x)
      assert(write_edram_sample(edram, resolve_layout, x, y,
                                {0xFF000000u | (y << 8) | x, 0}));
  std::vector<std::byte> resolved(2 * 40, std::byte{0xCD});
  assert(resolve_edram_raw(edram, resolve_layout, 0, 0, 8, 2, 0,
                           resolved, 40));
  std::uint32_t value{};
  std::memcpy(&value, resolved.data() + 40 + 7 * 4, 4);
  assert(value == 0xFF000107u);
  assert(resolved[32] == std::byte{0xCD});

  for (std::uint32_t y = 0; y < 2; ++y)
    for (std::uint32_t x = 0; x < 8; ++x) {
      const std::uint32_t replacement = 0xAB000000u | (y << 8) | x;
      std::memcpy(resolved.data() + y * 40 + x * 4, &replacement, 4);
    }
  assert(store_edram_raw(edram, resolve_layout, 0, 0, 8, 2, 0,
                         resolved, 40));
  assert(read_edram_sample(edram, resolve_layout, 7, 1)[0] == 0xAB000107u);

  EdramSurfaceLayout clear_layout{7, 4, 3, MsaaSamples::X4, false, false};
  for (std::uint32_t y = 0; y < 3; ++y)
    for (std::uint32_t x = 0; x < 4; ++x)
      for (std::uint32_t sample = 0; sample < 4; ++sample)
        assert(write_edram_sample(edram, clear_layout, x, y,
                                  {0x10000000u | (sample << 16) | (y << 8) | x,
                                   0}, sample));
  assert(clear_edram_surface_region(edram, clear_layout, 1, 1, 3, 5,
                                    {0xCAFEBABEu, 0}));
  for (std::uint32_t sample = 0; sample < 4; ++sample) {
    assert(read_edram_sample(edram, clear_layout, 1, 1, sample)[0] ==
           0xCAFEBABEu);
    assert(read_edram_sample(edram, clear_layout, 2, 2, sample)[0] ==
           0xCAFEBABEu);
    assert(read_edram_sample(edram, clear_layout, 0, 1, sample)[0] !=
           0xCAFEBABEu);
    assert(read_edram_sample(edram, clear_layout, 3, 2, sample)[0] !=
           0xCAFEBABEu);
  }
    std::vector<std::byte> guarded(16, std::byte{0xCD});
    assert(!resolve_edram_raw(edram, resolve_layout, UINT32_MAX, 0, 1, 1, 0,
                              guarded, 4));
    assert(!store_edram_raw(edram, resolve_layout, UINT32_MAX, 0, 1, 1, 0,
                            guarded, 4));

  std::cout << "xenon_edram_surface_tests: ok\n";
}

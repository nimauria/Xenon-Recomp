#include <algorithm>
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

namespace {

// Every tile a sample of the region touches, by walking each sample: the
// definition covered_edram_tiles() must match.
std::vector<std::uint16_t> tiles_by_sample(const EdramSurfaceLayout& layout,
                                           EdramSurfaceRegion region) {
  std::set<std::uint16_t> tiles;
  const auto samples = 1u << static_cast<unsigned>(layout.msaa);
  for (auto y = std::max(region.top, 0); y < std::min(region.bottom, int(layout.height_pixels)); ++y)
    for (auto x = std::max(region.left, 0); x < std::min(region.right, int(layout.pitch_pixels)); ++x)
      for (std::uint32_t s = 0; s < samples; ++s) {
        const auto address = edram_sample_address(layout, std::uint32_t(x), std::uint32_t(y), s);
        assert(address);
        tiles.insert(static_cast<std::uint16_t>(address->low / Edram::kTileBytes));
        if (address->has_high)
          tiles.insert(static_cast<std::uint16_t>(address->high / Edram::kTileBytes));
      }
  return {tiles.begin(), tiles.end()};
}

// Regression (AC6 spent hundreds of ms per draw planning EDRAM ownership):
// the tile set is computed from tile rows and columns, and must equal the
// per-sample definition for every sample count, format width, depth swizzle,
// partial region and EDRAM wrap-around.
void test_covered_tiles_match_the_per_sample_definition() {
  const EdramSurfaceRegion regions[] = {
      {0, 0, 1280, 720}, {0, 0, 1, 1}, {39, 7, 41, 9}, {79, 15, 81, 17}, {100, 3, 333, 77}};
  for (const auto msaa : {MsaaSamples::X1, MsaaSamples::X2, MsaaSamples::X4})
    for (const bool wide : {false, true})
      for (const bool depth : {false, true})
        for (const std::uint32_t base : {0u, 5u, 2040u})
          for (const std::uint32_t pitch : {1280u, 640u, 200u}) {
            EdramSurfaceLayout layout{base, pitch, 1u, msaa, wide, depth};
            layout.height_pixels = std::min(720u, layout.max_resident_height_pixels());
            for (const auto& region : regions)
              assert(covered_edram_tiles(layout, region) == tiles_by_sample(layout, region));
          }
}

// An ownership change moves only the tiles changing hands: the rectangles
// for a tile set cover exactly that set, and sets that do not map to whole
// pixels are refused.
void test_tile_regions_cover_exactly_the_tiles() {
  for (const auto msaa : {MsaaSamples::X1, MsaaSamples::X2, MsaaSamples::X4})
    for (const bool wide : {false, true})
      for (const std::uint32_t base : {0u, 1500u}) {
        EdramSurfaceLayout layout{base, 640u, 1u, msaa, wide, false};
        layout.height_pixels = std::min(720u, layout.max_resident_height_pixels());
        const auto all = covered_edram_tiles(
            layout, {0, 0, int(layout.pitch_pixels), int(layout.height_pixels)});
        // The whole surface, a band of rows, and a scattered pattern that keeps
        // 64bpp column pairs together.
        std::vector<std::vector<std::uint16_t>> sets{all, {}, {}};
        for (std::size_t i = 0; i < all.size(); ++i) {
          const auto relative = (all[i] + Edram::kTileCount - base) % Edram::kTileCount;
          const auto row = relative / layout.pitch_tiles();
          const auto column = (relative % layout.pitch_tiles()) / (wide ? 2u : 1u);
          if (row >= 3 && row < 9) sets[1].push_back(all[i]);
          if ((row + column) % 3 == 0) sets[2].push_back(all[i]);
        }
        for (const auto& set : sets) {
          std::vector<EdramSurfaceRegion> regions;
          assert(edram_tile_regions(layout, set, regions));
          std::set<std::uint16_t> covered;
          for (const auto& region : regions)
            for (const auto tile : covered_edram_tiles(layout, region)) covered.insert(tile);
          assert(covered == std::set<std::uint16_t>(set.begin(), set.end()));
        }
        if (wide) {
          // One half of a 64bpp column is not a whole pixel.
          std::vector<EdramSurfaceRegion> regions;
          const std::uint16_t half[] = {all.front()};
          assert(!edram_tile_regions(layout, half, regions));
        }
      }
  // The whole of a 1280x720 surface is one rectangle.
  EdramSurfaceLayout frame{0u, 1280u, 720u, MsaaSamples::X1, false, false};
  std::vector<EdramSurfaceRegion> regions;
  assert(edram_tile_regions(frame, covered_edram_tiles(frame, {0, 0, 1280, 720}), regions));
  assert(regions.size() == 1u && regions[0].right == 1280 && regions[0].bottom == 720);
}

// Regression (AC6's color and depth targets of one draw each covered all of
// EDRAM, so acquiring one evicted the other within the same draw): a bound
// surface ends where the next bound surface begins, cyclically.
void test_bound_surfaces_do_not_overlap() {
  const EdramSurfaceLayout color{0u, 640u, 1u, MsaaSamples::X4, false, false};
  const EdramSurfaceLayout depth{720u, 640u, 1u, MsaaSamples::X4, false, true};
  const std::uint32_t bases[] = {0u, 720u};
  assert(rows_before_next_surface(color, bases) == 360u);  // 720 tiles / 16 per band * 8 rows
  assert(rows_before_next_surface(depth, bases) == 664u);  // wraps round to tile 0
  assert(rows_before_next_surface(color, std::span<const std::uint32_t>{}) ==
         color.max_resident_height_pixels());
  // A base inside the first tile row is not a bound.
  const std::uint32_t adjacent[] = {0u, 3u};
  assert(rows_before_next_surface(color, adjacent) == color.max_resident_height_pixels());
}

// The direct GPU transfer between native images (AC6 renders 640x360 4x and
// resolves the same tiles as 1280x720 1x every frame): every destination host
// sample takes the source sample holding the same EDRAM word, for every
// sample count pair, host 2x layout, format width, depth swizzle and base.
void test_direct_transfer_reads_the_sample_holding_the_same_word() {
  struct Case {
    EdramSurfaceLayout source, destination;
    MsaaSamples source_host, destination_host;
  };
  std::vector<Case> cases;
  for (const auto source_msaa : {MsaaSamples::X1, MsaaSamples::X2, MsaaSamples::X4})
    for (const auto destination_msaa : {MsaaSamples::X1, MsaaSamples::X2, MsaaSamples::X4})
      for (const bool wide : {false, true})
        for (const bool source_depth : {false, true})
          for (const bool destination_depth : {false, true})
            for (const bool emulated_2x : {false, true}) {
              // Both span the same 4 tile rows of an 80-sample-wide strip,
              // whatever their sample counts.
              const auto pitch = [](MsaaSamples m) { return m == MsaaSamples::X4 ? 80u : 160u; };
              const auto height = [](MsaaSamples m) { return m == MsaaSamples::X1 ? 64u : 32u; };
              const auto host = [&](MsaaSamples m) {
                return m == MsaaSamples::X2 && emulated_2x ? MsaaSamples::X4 : m;
              };
              cases.push_back({{2046u, pitch(source_msaa), height(source_msaa), source_msaa, wide,
                                source_depth},
                               {2046u, pitch(destination_msaa), height(destination_msaa),
                                destination_msaa, wide, destination_depth},
                               host(source_msaa), host(destination_msaa)});
            }
  for (const auto& c : cases) {
    const auto tiles = covered_edram_tiles(
        c.destination,
        {0, 0, int(c.destination.pitch_pixels), int(c.destination.height_pixels)});
    assert(edram_direct_transfer_supported(c.source, c.destination, tiles));
    const auto constants =
        make_edram_transfer_constants(c.source, c.source_host, c.destination, c.destination_host);
    const auto host_samples = 1u << static_cast<unsigned>(c.destination_host);
    for (std::uint32_t y = 0; y < c.destination.height_pixels; ++y)
      for (std::uint32_t x = 0; x < c.destination.pitch_pixels; ++x)
        for (std::uint32_t host_sample = 0; host_sample < host_samples; ++host_sample) {
          const auto source = edram_transfer_source(constants, x, y, host_sample);
          // Host samples of 2x held in 4x that carry no guest sample.
          const bool unused = c.destination.msaa == MsaaSamples::X2 &&
                              c.destination_host == MsaaSamples::X4 && host_sample != 0u &&
                              host_sample != 3u;
          if (unused) {
            assert(!source);
            continue;
          }
          assert(source);
          // Which guest sample each host sample is, by the forward mapping.
          const auto guest_of = [](MsaaSamples guest, MsaaSamples host_msaa,
                                   std::uint32_t host_index) {
            const auto count = 1u << static_cast<unsigned>(guest);
            for (std::uint32_t g = 0; g < count; ++g)
              if (map_guest_sample_to_host(guest, g, host_msaa == MsaaSamples::X2)->sample ==
                  host_index)
                return g;
            assert(false);
            return 0u;
          };
          const auto destination_guest = guest_of(c.destination.msaa, c.destination_host, host_sample);
          const auto source_guest = guest_of(c.source.msaa, c.source_host, source->host_sample);
          const auto expected = edram_sample_address(c.destination, x, y, destination_guest);
          const auto actual = edram_sample_address(c.source, source->x, source->y, source_guest);
          assert(expected && actual && expected->low == actual->low);
        }
  }
  // Refused: a source that does not hold the whole tile, and 64bpp tile
  // pairs that start on different parities.
  const EdramSurfaceLayout short_source{0u, 80u, 8u, MsaaSamples::X1, false, false};
  const EdramSurfaceLayout full{0u, 80u, 16u, MsaaSamples::X1, false, false};
  const std::uint16_t first_tile[] = {0u};
  assert(!edram_direct_transfer_supported(short_source, full, first_tile));
  const EdramSurfaceLayout wide_even{0u, 80u, 16u, MsaaSamples::X1, true, false};
  const EdramSurfaceLayout wide_odd{1u, 80u, 16u, MsaaSamples::X1, true, false};
  const std::uint16_t pair[] = {1u, 2u};
  assert(!edram_direct_transfer_supported(wide_even, wide_odd, pair));
  assert(!edram_direct_transfer_supported(full, wide_even, first_tile));
}

}  // namespace

int main() {
  test_direct_transfer_reads_the_sample_holding_the_same_word();
  test_bound_surfaces_do_not_overlap();
  test_covered_tiles_match_the_per_sample_definition();
  test_tile_regions_cover_exactly_the_tiles();
  EdramSurfaceLayout single{2040, 1280, 720, MsaaSamples::X1, false, false};
  assert(single.sample_width() == 1280 && single.sample_height() == 720);
  assert(single.pitch_tiles() == 16 && single.tile_rows() == 45);
  assert(single.tile_count() == 720);
  // The most rows a pitch can hold before tile addresses wrap onto itself
  // (AC6 sized a 640-pitch 4x surface 8192 rows tall: eight times EDRAM).
  assert(single.max_resident_height_pixels() == 2048u);
  assert((EdramSurfaceLayout{0, 640, 1, MsaaSamples::X4, false, false}
              .max_resident_height_pixels() == 1024u));
  assert((EdramSurfaceLayout{0, 1280, 1, MsaaSamples::X1, true, false}
              .max_resident_height_pixels() == 1024u));
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

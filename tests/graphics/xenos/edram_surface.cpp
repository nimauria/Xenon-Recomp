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

  EdramSurfaceLayout msaa2{1008, 1120, 720, MsaaSamples::X2, false, true};
  assert(msaa2.sample_width() == 1120 && msaa2.sample_height() == 1440);
  assert(msaa2.pitch_tiles() == 14 && msaa2.tile_rows() == 90);
  assert(msaa2.tile_count() == 1260);

  EdramSurfaceLayout msaa4{0, 1280, 720, MsaaSamples::X4, true, false};
  assert(msaa4.sample_width() == 2560 && msaa4.sample_height() == 1440);
  assert(msaa4.pitch_tiles() == 64 && msaa4.tile_rows() == 90);

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

  std::cout << "xenon_edram_surface_tests: ok\n";
}

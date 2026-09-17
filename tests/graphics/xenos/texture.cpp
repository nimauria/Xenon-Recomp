#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <set>
#include <vector>

#include "xenon/gpu/texture.hpp"

namespace {

using namespace xenon::gpu;

TextureDescriptor rgba8(std::uint32_t width, std::uint32_t height, bool tiled) {
  TextureDescriptor descriptor{};
  descriptor.base_address = 0x10000;
  descriptor.mip_address = 0x20000;
  descriptor.width = width;
  descriptor.height = height;
  descriptor.depth = 1;
  descriptor.pitch = (width + 31u) & ~31u;
  descriptor.format = 6;
  descriptor.dimension = TextureDimension::TwoDOrStacked;
  descriptor.tiled = tiled;
  descriptor.valid = true;
  return descriptor;
}

void test_complete_format_catalogue() {
  std::size_t native = 0;
  for (unsigned i = 0; i < 64; ++i) {
    const auto& format = texture_format_info(static_cast<std::uint8_t>(i));
    assert(format.xenos_format == i);
    assert(!format.name.empty());
    assert(format.block_width && format.block_height && format.bits_per_pixel);
    assert(format.bytes_per_block());
    native += format.host_supported();
  }
  assert(native >= 40);
  assert(texture_format_info(18).bytes_per_block() == 8);
  assert(texture_format_info(20).bytes_per_block() == 16);
  assert(texture_format_info(49).host_format == TextureHostFormat::BC5Unorm);
  assert(texture_format_info(22).host_format == TextureHostFormat::Unsupported);
  assert(texture_format_info(23).host_format == TextureHostFormat::Unsupported);
}

void test_tiled_decode_and_endian() {
  auto descriptor = rgba8(32, 32, true);
  descriptor.endian = Endian::Swap8In32;
  const auto layout = build_texture_layout(descriptor);
  assert(layout.valid && layout.subresources.size() == 1);
  std::vector<std::byte> memory(0x40000);
  std::set<std::uint64_t> offsets;
  for (std::uint32_t y = 0; y < 32; ++y) {
    for (std::uint32_t x = 0; x < 32; ++x) {
      const auto offset = tiled_offset_2d(x, y, 32, 4);
      assert(offsets.insert(offset).second);
      const std::uint32_t value = ((x + y * 32u) << 24) | 0x00010203u;
      std::memcpy(memory.data() + descriptor.base_address + offset, &value, 4);
    }
  }
  const auto decoded = decode_texture(descriptor, memory);
  assert(decoded.valid && decoded.linear_data.size() == 32u * 32u * 4u);
  std::uint32_t first{};
  std::memcpy(&first, decoded.linear_data.data(), 4);
  assert(first == gpu_swap(0x00010203u, Endian::Swap8In32));
  std::uint32_t last{};
  std::memcpy(&last, decoded.linear_data.data() + (1023u * 4u), 4);
  assert(last == gpu_swap((1023u << 24) | 0x00010203u,
                          Endian::Swap8In32));
}

void test_mips_packing_and_dirty_ranges() {
  auto descriptor = rgba8(128, 64, false);
  descriptor.mip_max_level = 7;
  descriptor.packed_mips = true;
  const auto layout = build_texture_layout(descriptor);
  assert(layout.valid && layout.subresources.size() == 8);
  assert(layout.subresources.front().guest_address == descriptor.base_address);
  assert(layout.subresources[1].guest_address == descriptor.mip_address);
  bool saw_packed_origin = false;
  for (const auto& sub : layout.subresources) {
    saw_packed_origin |= sub.packed_x_blocks != 0 || sub.packed_y_blocks != 0;
  }
  assert(saw_packed_origin);
  // Every packed sublevel shares one guest tail allocation and storage pitch.
  const auto tail_address = layout.subresources[2].guest_address;
  const auto tail_pitch = layout.subresources[2].storage_pitch_blocks;
  for (std::size_t i = 2; i < layout.subresources.size(); ++i) {
    assert(layout.subresources[i].guest_address == tail_address);
    assert(layout.subresources[i].storage_pitch_blocks == tail_pitch);
  }

  TextureDirtyTracker dirty;
  dirty.track(descriptor.hash(), layout);
  assert(dirty.consume_dirty(descriptor.hash()));
  assert(!dirty.consume_dirty(descriptor.hash()));
  dirty.mark_dirty(descriptor.base_address + 16, 4);
  assert(dirty.is_dirty(descriptor.hash()));
  assert(dirty.consume_dirty(descriptor.hash()));
  dirty.mark_dirty(0x1F000000, 4);
  assert(!dirty.is_dirty(descriptor.hash()));
}

void test_3d_tiled_addressing() {
  std::set<std::uint64_t> offsets;
  for (std::uint32_t z = 0; z < 4; ++z)
    for (std::uint32_t y = 0; y < 32; ++y)
      for (std::uint32_t x = 0; x < 32; ++x)
        assert(offsets.insert(tiled_offset_3d(x, y, z, 32, 32, 4)).second);
  assert(offsets.size() == 4096);
}

}  // namespace

int main() {
  test_complete_format_catalogue();
  test_tiled_decode_and_endian();
  test_mips_packing_and_dirty_ranges();
  test_3d_tiled_addressing();
  std::cout << "xenon_texture_tests: ok\n";
}

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <set>
#include <vector>

#include "xenon/gpu/depth_format.hpp"
#include "xenon/gpu/texture.hpp"
#include "xenon/memory/address_space.hpp"

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

  std::vector<std::byte> encoded_memory(memory.size(), std::byte{0xCD});
  std::string encode_error;
  assert(encode_texture(descriptor, decoded.linear_data, encoded_memory,
                        &encode_error));
  assert(encode_error.empty());
  const auto round_trip = decode_texture(descriptor, encoded_memory);
  assert(round_trip.valid);
  assert(round_trip.linear_data == decoded.linear_data);
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

  // Memory V2 GPU consumers use Xenon-owned page epochs instead of a
  // synchronous callback on each CPU scalar store.
  xenon::memory::GuestMemoryCoherency coherency;
  const auto clean_epoch = coherency.current_epoch();
  dirty.track_clean(descriptor.hash(), layout, clean_epoch);
  assert(!dirty.consume_dirty(descriptor.hash(), coherency,
                              coherency.current_epoch()));
  coherency.mark_write(descriptor.base_address + 32u, 4u);
  const auto dirty_epoch = coherency.current_epoch();
  assert(dirty.consume_dirty(descriptor.hash(), coherency, dirty_epoch));
  assert(!dirty.consume_dirty(descriptor.hash(), coherency, dirty_epoch));
}

void test_3d_tiled_addressing() {
  std::set<std::uint64_t> offsets;
  for (std::uint32_t z = 0; z < 4; ++z)
    for (std::uint32_t y = 0; y < 32; ++y)
      for (std::uint32_t x = 0; x < 32; ++x)
        assert(offsets.insert(tiled_offset_3d(x, y, z, 32, 32, 4)).second);
  assert(offsets.size() == 4096);
}

void test_endian128_modes() {
  std::array<std::byte, 32> original{};
  for (std::size_t i = 0; i < original.size(); ++i)
    original[i] = static_cast<std::byte>(i);
  for (const auto mode : {Endian128::None, Endian128::Swap8In16,
                          Endian128::Swap8In32, Endian128::Swap16In32,
                          Endian128::Swap8In64, Endian128::Swap8In128}) {
    auto transformed = original;
    apply_endian128(transformed, mode);
    apply_endian128(transformed, mode);
    assert(transformed == original);
  }
  auto swapped = original;
  apply_endian128(swapped, Endian128::Swap8In128);
  assert(swapped[0] == std::byte{15} && swapped[15] == std::byte{0});
  assert(swapped[16] == std::byte{31} && swapped[31] == std::byte{16});
}

void test_raw_resolve_write() {
  assert(raw_resolve_texture_format(ColorRenderTargetFormat::R8G8B8A8) == 6);
  assert(raw_resolve_texture_format(
             ColorRenderTargetFormat::R16G16B16A16Float) == 32);
  assert(raw_resolve_texture_format(
             ColorRenderTargetFormat::R10G10B10A2Float) == 63);

  CopyResolveState copy{};
  copy.command = CopyCommand::Raw;
  copy.destination_base = 0x10000;
  copy.destination_pitch = 32;
  copy.destination_height = 32;
  copy.destination_format = 6;
  copy.destination_endian = Endian128::Swap8In128;
  ResolveRectangle rectangle{8, 8, 16, 16, true};
  std::vector<std::byte> source(8u * 8u * 4u);
  for (std::size_t i = 0; i < source.size(); ++i)
    source[i] = static_cast<std::byte>(i & 0xFFu);
  xenon::memory::AddressSpace memory;
  assert(memory.initialize());
  assert(memory.fill_physical(0, 0x20000, std::byte{0xCC}));
  const auto written = write_raw_resolve(copy, rectangle, source, 8u * 4u,
                                         memory);
  assert(written.valid && written.modified_size != 0);

  // Decode the affected storage as logical bytes using the same Endian128
  // group rule, then verify every tiled pixel landed at its expected address.
  std::vector<std::byte> logical(0x20000);
  assert(memory.copy_physical_range(0, logical));
  apply_endian128(std::span(logical).subspan(written.modified_address,
                                             written.modified_size),
                  copy.destination_endian);
  for (std::uint32_t y = 0; y < 8; ++y) {
    for (std::uint32_t x = 0; x < 8; ++x) {
      const auto offset = copy.destination_base +
          tiled_offset_2d(8 + x, 8 + y, 32, 4);
      assert(std::memcmp(logical.data() + offset,
                         source.data() + (y * 8u + x) * 4u, 4) == 0);
    }
  }

  // Pitch and height are storage strides, not clipping dimensions. This
  // models the offset resolve pattern used by real titles.
  copy.destination_base = 0x20000;
  copy.destination_pitch = 320;
  copy.destination_height = 192;
  copy.destination_endian = Endian128::None;
  rectangle = {640, 256, 648, 264, true};
  assert(memory.fill_physical(0, 0x400000, std::byte{}));
  const auto offset_written = write_raw_resolve(
      copy, rectangle, source, 8u * 4u, memory);
  assert(offset_written.valid);
  const auto* physical = memory.physical_data();
  assert(physical != nullptr);
  for (std::uint32_t y = 0; y < 8; ++y) {
    for (std::uint32_t x = 0; x < 8; ++x) {
      const auto offset = copy.destination_base +
          tiled_offset_2d(640u + x, 256u + y, 320u, 4u);
      assert(std::memcmp(physical + offset,
                         source.data() + (y * 8u + x) * 4u, 4) == 0);
    }
  }

  std::vector<std::byte> memory_before_empty(0x400000);
  assert(memory.copy_physical_range(0, memory_before_empty));
  const auto empty_written = write_raw_resolve(
      copy, {640, 256, 640, 256, true}, {}, 0, memory);
  assert(empty_written.valid && empty_written.modified_size == 0);
  std::vector<std::byte> memory_after_empty(0x400000);
  assert(memory.copy_physical_range(0, memory_after_empty));
  assert(memory_after_empty == memory_before_empty);

  // Array destinations use 3D tiling and the three-bit destination slice.
  copy.destination_array = true;
  copy.destination_slice = 3;
  rectangle = {328, 200, 336, 208, true};
  assert(memory.fill_physical(0, 0x800000, std::byte{}));
  const auto array_written = write_raw_resolve(
      copy, rectangle, source, 8u * 4u, memory);
  assert(array_written.valid);
  physical = memory.physical_data();
  for (std::uint32_t y = 0; y < 8; ++y) {
    for (std::uint32_t x = 0; x < 8; ++x) {
      const auto offset = copy.destination_base + tiled_offset_3d(
          328u + x, 200u + y, 3u, 320u, 192u, 4u);
      assert(std::memcmp(physical + offset,
                         source.data() + (y * 8u + x) * 4u, 4) == 0);
    }
  }
}

void test_depth_resolve_write() {
  assert(depth_resolve_texture_format(DepthRenderTargetFormat::D24S8) == 22);
  assert(depth_resolve_texture_format(DepthRenderTargetFormat::D24FS8) == 23);

  CopyResolveState copy{};
  // D3D9 programs a color-style destination format for depth resolves. The
  // common writer must ignore that field, conversion controls and component
  // swap while preserving the exact packed depth/stencil word.
  copy.command = CopyCommand::Convert;
  copy.destination_base = 0x10000;
  copy.destination_pitch = 32;
  copy.destination_height = 32;
  copy.destination_format = 6;
  copy.destination_endian = Endian128::Swap8In128;
  copy.destination_exponent_bias = 7;
  copy.destination_red_blue_swap = true;
  const ResolveRectangle rectangle{8, 8, 16, 16, true};

  std::vector<std::uint32_t> source(8u * 8u);
  for (std::uint32_t y = 0; y < 8; ++y) {
    for (std::uint32_t x = 0; x < 8; ++x) {
      const float depth = float((y * 8u + x) & 63u) / 63.0f;
      source[y * 8u + x] = pack_depth_stencil(
          DepthRenderTargetFormat::D24S8, depth,
          static_cast<std::uint8_t>(0x80u | x));
    }
  }
  xenon::memory::AddressSpace memory;
  assert(memory.initialize());
  assert(memory.fill_physical(0, 0x20000, std::byte{0xCC}));
  const auto written = write_depth_resolve(
      copy, DepthRenderTargetFormat::D24S8, rectangle, source, 8u * 4u,
      memory);
  assert(written.valid && written.modified_size != 0);

  std::vector<std::byte> logical(0x20000);
  assert(memory.copy_physical_range(0, logical));
  apply_endian128(std::span(logical).subspan(written.modified_address,
                                             written.modified_size),
                  copy.destination_endian);
  for (std::uint32_t y = 0; y < 8; ++y) {
    for (std::uint32_t x = 0; x < 8; ++x) {
      const auto offset = copy.destination_base +
          tiled_offset_2d(8 + x, 8 + y, 32, 4);
      std::uint32_t packed{};
      std::memcpy(&packed, logical.data() + offset, sizeof(packed));
      assert(packed == source[y * 8u + x]);
    }
  }

  // D24FS8 has a representable range above 1.0. Verify resolve transport is
  // bit-exact rather than re-quantizing through normalized host depth.
  copy.destination_base = 0x30000;
  copy.destination_endian = Endian128::None;
  const ResolveRectangle float_rectangle{0, 0, 1, 1, true};
  const std::array<std::uint32_t, 1> float_source{
      pack_depth_stencil(DepthRenderTargetFormat::D24FS8, 1.5f, 0xA5)};
  assert(memory.fill_physical(0, 0x40000, std::byte{}));
  const auto float_written = write_depth_resolve(
      copy, DepthRenderTargetFormat::D24FS8, float_rectangle, float_source, 4,
      memory);
  assert(float_written.valid);
  std::uint32_t packed_float{};
  std::memcpy(&packed_float, memory.physical_data(copy.destination_base),
              sizeof(packed_float));
  assert(packed_float == float_source[0]);
  assert(unpack_depth_stencil(DepthRenderTargetFormat::D24FS8, packed_float)
             .stencil == 0xA5);
}

void test_converted_resolve_write() {
  CopyResolveState copy{};
  copy.command = CopyCommand::Convert;
  copy.destination_base = 0x10000;
  copy.destination_pitch = 32;
  copy.destination_height = 32;
  copy.destination_format = 38;
  copy.destination_exponent_bias = 1;
  copy.destination_red_blue_swap = true;
  const ResolveRectangle rectangle{0, 0, 1, 1, true};
  const ColorSample source_sample{{1.5f, -2.0f, 0.0f, 1.0f}};
  std::array<std::byte, 4> source{};
  assert(encode_host_color_sample(ColorRenderTargetFormat::R16G16Fixed,
                                  source_sample, source));
  xenon::memory::AddressSpace memory;
  assert(memory.initialize());
  assert(memory.fill_physical(0, 0x20000, std::byte{}));
  const auto written = write_converted_resolve(
      copy, ColorRenderTargetFormat::R16G16Fixed, rectangle, source, 4,
      memory);
  assert(written.valid);
  const auto offset = copy.destination_base + tiled_offset_2d(0, 0, 32, 16);
  std::array<float, 4> converted{};
  std::memcpy(converted.data(), memory.physical_data(offset), sizeof(converted));
  assert(converted[0] == 0.0f && converted[1] == -4.0f &&
         converted[2] == 3.0f && converted[3] == 2.0f);

  copy.command = CopyCommand::Raw;
  copy.destination_format = 62;
  copy.destination_exponent_bias = 0;
  copy.destination_red_blue_swap = false;
  std::array<std::byte, 8> gamma_source{};
  const ColorSample gamma_sample{{0.125f, 0.5f, 1.0f, 0.25f}};
  assert(encode_host_color_sample(ColorRenderTargetFormat::R8G8B8A8Gamma,
                                  gamma_sample, gamma_source));
  assert(memory.fill_physical(0, 0x20000, std::byte{}));
  const auto gamma_written = write_converted_resolve(
      copy, ColorRenderTargetFormat::R8G8B8A8Gamma, rectangle,
      gamma_source, 8, memory);
  assert(gamma_written.valid);
  std::uint32_t gamma_bits{};
  std::memcpy(&gamma_bits, memory.physical_data(copy.destination_base), 4);
  assert(gamma_bits == pack_color_sample(
             ColorRenderTargetFormat::R8G8B8A8Gamma, gamma_sample)[0]);
}

}  // namespace

int main() {
  test_complete_format_catalogue();
  test_tiled_decode_and_endian();
  test_mips_packing_and_dirty_ranges();
  test_3d_tiled_addressing();
  test_endian128_modes();
  test_raw_resolve_write();
  test_depth_resolve_write();
  test_converted_resolve_write();
  std::cout << "xenon_texture_tests: ok\n";
}

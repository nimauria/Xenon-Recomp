#include <cassert>
#include <bit>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include "xenon/gpu/resource_ir.hpp"

namespace {

void write_fetch(xenon::gpu::ResourceStateTracker& tracker, unsigned slot,
                 unsigned dword, std::uint32_t value) {
  tracker.apply({xenon::gpu::ResourceStateTracker::kFetchConstantBase +
                     slot * 6u + dword,
                 value});
}

void test_texture_descriptor() {
  using namespace xenon::gpu;
  ResourceStateTracker tracker;
  constexpr unsigned slot = 5;
  const std::uint32_t w0 = 2u | (1u << 2) | (2u << 4) | (3u << 6) |
                           (1u << 10) | (2u << 13) | (3u << 16) |
                           (20u << 22) | (1u << 31);
  const std::uint32_t w1 = 38u | (std::uint32_t(Endian::Swap8In32) << 6) |
                           (1u << 10) | (0x12345u << 12);
  const std::uint32_t w2 = 127u | (63u << 13) | (7u << 26);
  const std::uint32_t w3 = (0x688u << 1) | (1u << 19) | (2u << 21) |
                           (3u << 23) | (4u << 25);
  const std::uint32_t w4 = (2u << 2) | (6u << 6) | (0x3F0u << 12);
  const std::uint32_t w5 = 2u | (std::uint32_t(TextureDimension::TwoDOrStacked) << 9) |
                           (1u << 11) | (0x34567u << 12);
  write_fetch(tracker, slot, 0, w0);
  write_fetch(tracker, slot, 1, w1);
  write_fetch(tracker, slot, 2, w2);
  write_fetch(tracker, slot, 3, w3);
  write_fetch(tracker, slot, 4, w4);
  write_fetch(tracker, slot, 5, w5);
  const auto texture = tracker.texture(slot);
  assert(texture && texture->valid);
  assert(texture->base_address == 0x12345000u);
  assert(texture->mip_address == 0x34567000u);
  assert(texture->width == 128 && texture->height == 64 && texture->depth == 8);
  assert(texture->pitch == 640 && texture->tiled && texture->stacked);
  assert(texture->format == 38 && texture->mip_min_level == 2 &&
         texture->mip_max_level == 6 && texture->lod_bias == -16);
  assert(texture->hash() == tracker.texture(slot)->hash());
}

void test_vertex_and_render_state() {
  using namespace xenon::gpu;
  ResourceStateTracker tracker;
  constexpr unsigned slot = 3;
  constexpr unsigned select = 2;
  const auto offset = ResourceStateTracker::kFetchConstantBase + slot * 6u + select * 2u;
  tracker.apply({offset, 3u | ((0x2000u >> 2) << 2)});
  tracker.apply({offset + 1, std::uint32_t(Endian::Swap16In32) | (64u << 2)});
  const auto vertex = tracker.vertex_buffer(slot, select);
  assert(vertex && vertex->physical_address == 0x2000 &&
         vertex->size_dwords == 64 && vertex->endian == Endian::Swap16In32);

  tracker.apply({0x2000, 1280u | (2u << 16)});
  tracker.apply({0x2001, 12u | (6u << 16) | (0x3Eu << 20)});
  tracker.apply({0x2104, 0x000Fu});
  tracker.apply({0x200E, 10u | (20u << 16)});
  tracker.apply({0x200F, 1010u | (620u << 16)});
  tracker.apply({0x2200, (1u << 1) | (1u << 2)});
  tracker.apply({0x2201, 6u | (1u << 5) | (7u << 8) |
                              (1u << 16) | (4u << 21) | (10u << 24)});
  tracker.apply({0x2105, std::bit_cast<std::uint32_t>(0.25f)});
  tracker.apply({0x2106, std::bit_cast<std::uint32_t>(0.5f)});
  tracker.apply({0x210F, std::bit_cast<std::uint32_t>(640.0f)});
  tracker.apply({0x2110, std::bit_cast<std::uint32_t>(640.0f)});
  tracker.apply({0x2111, std::bit_cast<std::uint32_t>(-360.0f)});
  tracker.apply({0x2112, std::bit_cast<std::uint32_t>(360.0f)});
  tracker.apply({0x2205, 3u | (1u << 2) | (1u << 15)});
  tracker.apply({0x2208, static_cast<std::uint32_t>(EdramMode::ColorDepth)});
  const auto state = tracker.snapshot();
  assert(state.raster.surface_pitch == 1280 && state.raster.msaa_samples_log2 == 2);
  assert(state.raster.scissor_left == 10 && state.raster.scissor_bottom == 620);
  assert(state.color_targets[0].enabled && state.color_targets[0].base_tile == 12);
  assert(state.color_targets[0].format == 6 && state.color_targets[0].exponent_bias == -2);
  assert(state.color_targets[0].blend.enabled);
  assert(state.color_targets[0].blend.color_source == BlendFactor::SrcAlpha);
  assert(state.color_targets[0].blend.color_operation == BlendOperation::Subtract);
  assert(state.color_targets[0].blend.color_destination == BlendFactor::InvSrcAlpha);
  assert(state.color_targets[0].blend.alpha_operation ==
         BlendOperation::ReverseSubtract);
  assert(state.blend_constant[0] == 0.25f && state.blend_constant[1] == 0.5f);
  assert(state.depth_target.test_enabled && state.depth_target.write_enabled);
  assert(state.raster.cull_front && state.raster.cull_back &&
         state.raster.front_face_clockwise && state.raster.multisample_enabled);
  assert(state.raster.viewport.x_scale == 640.0f &&
         state.raster.viewport.y_scale == -360.0f);
  assert(state.edram_mode == EdramMode::ColorDepth);
  ir::DrawPacket draw{};
  draw.vertex_shader = {true, 1, 0};
  draw.pixel_shader = {true, 2, 0};
  draw.primitive_type = PrimitiveType::TriangleList;
  assert(state.pipeline_hash(draw) == state.pipeline_hash(draw));
}

void test_constant_buffer_abi() {
  using namespace xenon::gpu;
  ResourceStateTracker tracker;
  tracker.apply({0x4000, 0x3F800000});
  tracker.apply({0x4400, 0x40000000});
  tracker.apply({0x4900, 0xA5A5A5A5});
  tracker.apply({0x4908, 0x00001234});
  tracker.apply({0x4800, 0xABCDEF03});
  std::vector<std::byte> data(9472);
  assert(tracker.write_constant_buffer(data));
  const auto word = [&](std::size_t offset) {
    std::uint32_t value{};
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
  };
  assert(word(0) == 0x3F800000);
  assert(word(4096) == 0x40000000);
  assert(word(8192) == 0xA5A5A5A5);
  assert(word(8320) == 0x00001234);
  assert(word(8832) == 0xABCDEF03);
}

}  // namespace

int main() {
  test_texture_descriptor();
  test_vertex_and_render_state();
  test_constant_buffer_abi();
  std::cout << "xenon_resource_ir_tests: ok\n";
}

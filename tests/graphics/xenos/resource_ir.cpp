#include <cassert>
#include <bit>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
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
  const std::uint32_t w3 = (0x688u << 1) | (0x3Du << 13) | (1u << 19) | (2u << 21) |
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
         texture->mip_max_level == 6 && texture->lod_bias == -16 &&
         texture->exp_adjust == -3);
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
  tracker.apply({0x2103, 0x00ABCDEFu});
  tracker.apply({0x210E, std::bit_cast<std::uint32_t>(0.375f)});
  tracker.apply({0x2202, 4u | (1u << 3) | (1u << 4) |
                             (3u << 24) | (1u << 26) | (2u << 30)});
  tracker.apply({0x2380, std::bit_cast<std::uint32_t>(16.0f)});
  tracker.apply({0x2381, std::bit_cast<std::uint32_t>(2.0f)});
  tracker.apply({0x2382, std::bit_cast<std::uint32_t>(32.0f)});
  tracker.apply({0x2383, std::bit_cast<std::uint32_t>(4.0f)});
  tracker.apply({0x2205, 3u | (1u << 2) | (1u << 11) | (1u << 12) |
                             (1u << 13) | (1u << 15) | (1u << 21)});
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
  assert(state.primitive_assembly.reset_enabled &&
         state.primitive_assembly.reset_index == 0x00ABCDEFu);
  assert(state.raster.viewport.x_scale == 640.0f &&
         state.raster.viewport.y_scale == -360.0f);
  assert(state.pixel_control.alpha_test_enabled &&
         state.pixel_control.alpha_to_mask_enabled);
  assert(state.pixel_control.alpha_function == CompareFunction::Greater);
  assert(state.pixel_control.alpha_reference == 0.375f);
  assert((state.pixel_control.alpha_to_mask_offsets ==
          std::array<std::uint8_t, 4>{3, 1, 0, 2}));
  assert(state.raster.polygon_offset_front_enabled &&
         state.raster.polygon_offset_back_enabled &&
         state.raster.polygon_offset_parallel_enabled);
  assert(state.edram_mode == EdramMode::ColorDepth);
  ir::DrawPacket draw{};
  draw.vertex_shader = {true, 1, 0};
  draw.pixel_shader = {true, 2, 0};
  draw.primitive_type = PrimitiveType::TriangleList;
  const auto key = state.native_pipeline_key(draw);
  auto dynamic_state = state;
  dynamic_state.raster.viewport.x_offset += 100.0f;
  dynamic_state.raster.scissor_left += 7;
  dynamic_state.raster.surface_pitch += 64;
  dynamic_state.color_targets[0].base_tile += 1;
  dynamic_state.blend_constant[0] = 0.75f;
  dynamic_state.pixel_control.alpha_reference = 0.5f;
  dynamic_state.depth_target.stencil_reference = 23;
  dynamic_state.depth_target.stencil_back_reference = 42;
  dynamic_state.color_targets[1].format = 7;
  dynamic_state.color_targets[1].blend.color_source = BlendFactor::DestAlpha;
  dynamic_state.depth_target.stencil_front.fail = StencilOperation::Invert;
  assert(dynamic_state.native_pipeline_key(draw) == key);
  dynamic_state.color_targets[0].write_mask ^= 1u;
  assert(!(dynamic_state.native_pipeline_key(draw) == key));
}

void test_constant_buffer_abi() {
  using namespace xenon::gpu;
  ResourceStateTracker tracker;
  tracker.apply({0x4000, 0x3F800000});
  tracker.apply({0x4400, 0x40000000});
  tracker.apply({0x4900, 0xA5A5A5A5});
  tracker.apply({0x4908, 0x00001234});
  tracker.apply({0x4800, 0xABCDEF03});
  tracker.apply({0x4803, 0x3Du << 13});
  tracker.apply({0x2000, 1u << 16});
  tracker.apply({0x210E, std::bit_cast<std::uint32_t>(0.5f)});
  tracker.apply({0x2202, 6u | (1u << 3) | (1u << 4) | (2u << 24) |
                             (2u << 26) | (2u << 28) | (2u << 30)});
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
  assert(word(8836) == 0xFFFFFFFDu);
  assert(word(9344) == 3u);
  assert(word(9348) == static_cast<std::uint32_t>(CompareFunction::GreaterEqual));
  assert(word(9352) == 2u);
  assert(word(9356) == 0xAAu);
  assert(word(9360) == std::bit_cast<std::uint32_t>(0.5f));
}

void test_copy_resolve_state() {
  using namespace xenon::gpu;
  ResourceStateTracker tracker;
  tracker.apply({0x2208, static_cast<std::uint32_t>(EdramMode::Copy)});
  tracker.apply({0x2318, 4u | (6u << 4) | (1u << 8) | (1u << 9) |
                             (1u << 20)});
  tracker.apply({0x2319, 0x01234000u});
  tracker.apply({0x231A, 1280u | (720u << 16)});
  tracker.apply({0x231B, 5u | (1u << 3) | (3u << 4) | (38u << 7) |
                             (2u << 13) | (0x3Eu << 16) | (1u << 24)});
  tracker.apply({0x231D, 0x12ABCDEFu});
  tracker.apply({0x231E, 0x89ABCDEFu});
  tracker.apply({0x231F, 0x01234567u});
  const auto state = tracker.snapshot();
  assert(state.edram_mode == EdramMode::Copy);
  assert(state.copy.copies_depth());
  assert(state.copy.sample_select == CopySampleSelect::Samples0123);
  assert(state.copy.command == CopyCommand::Convert);
  assert(state.copy.color_clear_enabled && state.copy.depth_clear_enabled);
  assert(state.copy.depth_clear == 0x12ABCDEFu);
  assert((state.copy.color_clear ==
          std::array<std::uint32_t, 2>{0x89ABCDEFu, 0x01234567u}));
  assert(state.copy.destination_base == 0x01234000u);
  assert(state.copy.destination_pitch == 1280);
  assert(state.copy.destination_height == 720);
  assert(state.copy.destination_endian == Endian128::Swap8In128);
  assert(state.copy.destination_array && state.copy.destination_slice == 3);
  assert(state.copy.destination_format == 38);
  assert(state.copy.destination_number_format == 2);
  assert(state.copy.destination_exponent_bias == -2);
  assert(state.copy.destination_red_blue_swap);

  assert(sanitize_copy_sample_select(CopySampleSelect::Samples0123,
                                     MsaaSamples::X4, false) ==
         CopySampleSelect::Samples0123);
  assert(sanitize_copy_sample_select(CopySampleSelect::Samples0123,
                                     MsaaSamples::X4, true) ==
         CopySampleSelect::Sample0);
  assert(sanitize_copy_sample_select(CopySampleSelect::Sample3,
                                     MsaaSamples::X2, false) ==
         CopySampleSelect::Sample1);
  assert(sanitize_copy_sample_select(CopySampleSelect::Samples23,
                                     MsaaSamples::X2, false) ==
         CopySampleSelect::Samples01);
  assert(sanitize_copy_sample_select(CopySampleSelect::Sample3,
                                     MsaaSamples::X1, false) ==
         CopySampleSelect::Sample0);
  assert(is_full_color_resolve(CopySampleSelect::Sample3, MsaaSamples::X1));
  assert(is_full_color_resolve(CopySampleSelect::Samples23, MsaaSamples::X2));
  assert(!is_full_color_resolve(CopySampleSelect::Sample1, MsaaSamples::X2));
  assert(is_full_color_resolve(CopySampleSelect::Samples0123,
                               MsaaSamples::X4));
  assert(!is_full_color_resolve(CopySampleSelect::Samples01,
                                MsaaSamples::X4));
  assert(float_to_d3d_fixed_16_8(0.5f / 256.0f) == 0);
  assert(float_to_d3d_fixed_16_8(1.5f / 256.0f) == 2);
  assert(float_to_d3d_fixed_16_8(-0.5f / 256.0f) == 0);
  assert(float_to_d3d_fixed_16_8(-1.5f / 256.0f) == -2);
  assert(float_to_d3d_fixed_16_8(
             std::numeric_limits<float>::quiet_NaN()) == 0);
  assert(float_to_d3d_fixed_16_8(
             std::numeric_limits<float>::infinity()) == 0x7FFFFF);
  assert(float_to_d3d_fixed_16_8(
             -std::numeric_limits<float>::infinity()) == -0x800000);

  // Resolve geometry is the conventional three float2 vertices in vf0.
  std::vector<std::byte> memory(0x1100);
  const std::array<float, 6> vertices{-0.5f, -0.5f, 15.5f,
                                      -0.5f, -0.5f, 15.5f};
  std::memcpy(memory.data() + 0x1000, vertices.data(), sizeof(vertices));
  write_fetch(tracker, 0, 0, 3u | 0x1000u);
  write_fetch(tracker, 0, 1, std::uint32_t(Endian::None) | (6u << 2));
  tracker.apply({0x2000, 64u});
  tracker.apply({0x200E, 0u});
  tracker.apply({0x200F, 64u | (64u << 16)});
  auto rectangle = decode_resolve_rectangle(tracker.snapshot(), memory);
  assert(rectangle.valid);
  assert(rectangle.left == 0 && rectangle.top == 0);
  assert(rectangle.right == 16 && rectangle.bottom == 16);

  auto plan_state = tracker.snapshot();
  plan_state.raster.msaa_samples_log2 = 2;
  plan_state.copy.source_select = 0;
  plan_state.copy.sample_select = CopySampleSelect::Sample2;
  auto resolve_plan = plan_resolve(plan_state, memory);
  assert(resolve_plan.valid && !resolve_plan.depth);
  assert(resolve_plan.samples == MsaaSamples::X4);
  assert(resolve_plan.guest_sample_mask == 0x4);
  assert(resolve_plan.selected_sample_count == 1);
  assert(resolve_plan.host_sample_for_guest[2] == 2);
  assert(!resolve_plan.native_color_average);
  plan_state.copy.sample_select = CopySampleSelect::Samples01;
  resolve_plan = plan_resolve(plan_state, memory);
  assert(resolve_plan.valid && !resolve_plan.native_color_average);
  assert(resolve_plan.guest_sample_mask == 0x3);
  assert(resolve_plan.selected_sample_count == 2);
  assert(resolve_plan.host_sample_for_guest[0] == 0);
  assert(resolve_plan.host_sample_for_guest[1] == 1);
  plan_state.copy.sample_select = CopySampleSelect::Samples23;
  resolve_plan = plan_resolve(plan_state, memory);
  assert(resolve_plan.valid && !resolve_plan.native_color_average);
  assert(resolve_plan.guest_sample_mask == 0xC);
  assert(resolve_plan.selected_sample_count == 2);
  assert(resolve_plan.host_sample_for_guest[2] == 2);
  assert(resolve_plan.host_sample_for_guest[3] == 3);
  plan_state.copy.sample_select = CopySampleSelect::Samples0123;
  resolve_plan = plan_resolve(plan_state, memory);
  assert(resolve_plan.valid && resolve_plan.native_color_average);
  assert(resolve_plan.guest_sample_mask == 0xF);

  // Depth resolves never average. Pair/full selections sanitize to one guest
  // sample before the native backend sees the plan.
  plan_state.copy.source_select = 4;
  plan_state.copy.sample_select = CopySampleSelect::Samples0123;
  resolve_plan = plan_resolve(plan_state, memory);
  assert(resolve_plan.valid && resolve_plan.depth);
  assert(!resolve_plan.native_color_average);
  assert(resolve_plan.copy.sample_select == CopySampleSelect::Sample0);
  assert(resolve_plan.guest_sample_mask == 0x1);
  assert(resolve_plan.selected_sample_count == 1);
  plan_state.copy.sample_select = CopySampleSelect::Samples23;
  resolve_plan = plan_resolve(plan_state, memory);
  assert(resolve_plan.valid && resolve_plan.depth);
  assert(resolve_plan.copy.sample_select == CopySampleSelect::Sample2);
  assert(resolve_plan.guest_sample_mask == 0x4);
  assert(resolve_plan.selected_sample_count == 1);

  tracker.apply({0x2080, 8u | (8u << 16)});
  tracker.apply({0x2205, 1u << 16});
  rectangle = decode_resolve_rectangle(tracker.snapshot(), memory);
  assert(rectangle.valid);
  assert(rectangle.left == 8 && rectangle.top == 8);
  assert(rectangle.right == 24 && rectangle.bottom == 24);

  // Resolve coordinates use D3D fixed-point conversion even for non-finite
  // inputs: NaN becomes zero instead of invalidating the whole rectangle.
  const float nan = std::numeric_limits<float>::quiet_NaN();
  std::memcpy(memory.data() + 0x1000, &nan, sizeof(nan));
  rectangle = decode_resolve_rectangle(tracker.snapshot(), memory);
  assert(rectangle.valid);
  assert(rectangle.left == 8 && rectangle.top == 8);
  assert(rectangle.right == 24 && rectangle.bottom == 24);

  // A well-formed resolve fully clipped by the scissor is an empty command,
  // not malformed geometry.
  tracker.apply({0x200E, 32u | (32u << 16)});
  rectangle = decode_resolve_rectangle(tracker.snapshot(), memory);
  assert(rectangle.valid && rectangle.empty());
  assert(rectangle.left == 32 && rectangle.top == 32);
  assert(rectangle.right == 32 && rectangle.bottom == 32);
}

void test_color_target_plan() {
  using namespace xenon::gpu;
  DrawResourceState state{};
  state.edram_mode = EdramMode::ColorDepth;

  auto plan = plan_color_targets(state);
  assert(!plan.any());
  assert(plan.attachment_count == 0);
  assert(plan.contiguous);

  state.color_targets[0].enabled = true;
  state.color_targets[1].enabled = true;
  state.color_targets[3].enabled = true;
  plan = plan_color_targets(state);
  assert(plan.any());
  assert(plan.enabled_mask == 0b1011u);
  assert(plan.attachment_count == 4);
  assert(!plan.contiguous);
  assert(plan.enabled[0] && plan.enabled[1] && !plan.enabled[2] && plan.enabled[3]);

  state.color_targets[2].enabled = true;
  plan = plan_color_targets(state);
  assert(plan.enabled_mask == 0b1111u);
  assert(plan.attachment_count == 4);
  assert(plan.contiguous);

  state.edram_mode = EdramMode::DepthOnly;
  plan = plan_color_targets(state);
  assert(!plan.any());
  assert(plan.attachment_count == 0);
}

void test_host_polygon_mode() {
  using namespace xenon::gpu;
  RasterState raster{};
  raster.front_polygon_type = 0;
  raster.back_polygon_type = 2;
  assert(host_polygon_mode(raster) == HostPolygonMode::Fill);

  raster.polygon_mode = 1;
  assert(host_polygon_mode(raster) == HostPolygonMode::Point);
  raster.front_polygon_type = 1;
  assert(host_polygon_mode(raster) == HostPolygonMode::Line);
  raster.cull_front = true;
  assert(host_polygon_mode(raster) == HostPolygonMode::Fill);

  // Reserved/non-dual Xenos values are normal filled triangles.
  raster.polygon_mode = 2;
  raster.cull_front = false;
  raster.front_polygon_type = 0;
  assert(host_polygon_mode(raster) == HostPolygonMode::Fill);
}

void test_preferred_polygon_offset() {
  using namespace xenon::gpu;
  RasterState raster{};
  raster.polygon_offset_front_enabled = true;
  raster.polygon_offset_back_enabled = true;
  raster.polygon_offset_parallel_enabled = true;
  raster.polygon_offset_front_scale = 16.0f;
  raster.polygon_offset_front_offset = 2.0f;
  raster.polygon_offset_back_scale = 32.0f;
  raster.polygon_offset_back_offset = 4.0f;
  auto offset = preferred_polygon_offset(
      raster, HostPrimitiveTopology::TriangleList);
  assert(offset.enabled && offset.scale == 16.0f && offset.offset == 2.0f);
  raster.cull_front = true;
  offset = preferred_polygon_offset(raster, HostPrimitiveTopology::TriangleList);
  assert(offset.enabled && offset.scale == 32.0f && offset.offset == 4.0f);
  offset = preferred_polygon_offset(raster, HostPrimitiveTopology::LineList);
  assert(offset.enabled && offset.scale == 16.0f && offset.offset == 2.0f);

  assert(scaled_polygon_offset_constant(
             1.0f, DepthRenderTargetFormat::D24S8) == 16777215.0f);
  assert(scaled_polygon_offset_constant(
             1.0f, DepthRenderTargetFormat::D24FS8) == 16777216.0f);
  assert(integer_polygon_offset(
             1.0f / 16777215.0f, DepthRenderTargetFormat::D24S8) == 1);
  assert(integer_polygon_offset(
             1.0f / 16777216.0f, DepthRenderTargetFormat::D24FS8) == 8);
  assert(integer_polygon_offset(
             -1.0f / 16777216.0f, DepthRenderTargetFormat::D24FS8) == -8);
}

}  // namespace

int main() {
  test_texture_descriptor();
  test_vertex_and_render_state();
  test_constant_buffer_abi();
  test_copy_resolve_state();
  test_color_target_plan();
  test_host_polygon_mode();
  test_preferred_polygon_offset();
  std::cout << "xenon_resource_ir_tests: ok\n";
}

#include <cassert>
#include <bit>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

#include "xenon/memory/gpu_coherency.hpp"
#include "xenon/gpu/resource_ir.hpp"

namespace {

void write_fetch(xenon::gpu::ResourceStateTracker& tracker, unsigned slot,
                 unsigned dword, std::uint32_t value) {
  tracker.apply({xenon::gpu::ResourceStateTracker::kFetchConstantBase +
                     slot * 6u + dword,
                 value});
}


void write_memexport_stream(xenon::gpu::ResourceStateTracker& tracker,
                            xenon::gpu::ShaderStage stage,
                            std::uint16_t constant_index,
                            std::uint32_t base_address_dwords,
                            std::uint8_t format,
                            std::uint32_t index_count,
                            xenon::gpu::Endian128 endian =
                                xenon::gpu::Endian128::None,
                            std::uint8_t number_format = 0,
                            bool red_blue_swap = false) {
  const std::uint32_t bank =
      stage == xenon::gpu::ShaderStage::Pixel ? 0x4400u : 0x4000u;
  const auto reg = bank + std::uint32_t(constant_index) * 4u;
  tracker.apply({reg + 0u, (base_address_dwords & 0x3FFFFFFFu) | (1u << 30u)});
  tracker.apply({reg + 1u, 0x4B000000u});
  tracker.apply({reg + 2u,
                 std::uint32_t(endian) | (std::uint32_t(format) << 8u) |
                     (std::uint32_t(number_format) << 16u) |
                     (std::uint32_t(red_blue_swap) << 19u) |
                     (0x4B0u << 20u)});
  tracker.apply({reg + 3u,
                 (index_count & 0x7FFFFFu) | (0x96u << 23u)});
}


void test_guest_memory_coherency_ranges() {
  using xenon::memory::GuestMemoryGpuCoherency;

  GuestMemoryGpuCoherency tracker;
  tracker.reset(0x10000u);
  auto cpu = tracker.cpu_dirty_ranges();
  assert(cpu.size() == 1 && cpu[0].address == 0 &&
         cpu[0].size == 0x10000u);
  tracker.mark_cpu_uploaded(0, 0x10000u);
  assert(tracker.cpu_dirty_ranges().empty());

  // A shader owns this byte range after memexport.
  tracker.mark_gpu_write(0x2000u, 0x1000u);
  assert(tracker.has_gpu_dirty(0x2400u, 4));

  // A four-byte CPU write in the middle wins only for those bytes. The rest of
  // the GPU-authored range must stay GPU-owned so a later CPU upload cannot
  // clobber unrelated memexport data on the same page.
  tracker.mark_cpu_write(0x2400u, 4u);
  const auto gpu = tracker.gpu_dirty_ranges();
  assert(gpu.size() == 2);
  assert(gpu[0].address == 0x2000u && gpu[0].size == 0x400u);
  assert(gpu[1].address == 0x2404u && gpu[1].size == 0xBFCu);
  cpu = tracker.cpu_dirty_ranges();
  assert(cpu.size() == 1 && cpu[0].address == 0x2400u && cpu[0].size == 4u);

  // CPU visibility is demand-driven and may cover only a subrange.
  tracker.mark_gpu_downloaded(0x2800u, 0x100u);
  assert(!tracker.has_gpu_dirty(0x2800u, 0x100u));
  assert(tracker.has_gpu_dirty(0x2700u, 0x100u));
  assert(tracker.has_gpu_dirty(0x2900u, 0x100u));

  // A later GPU export supersedes pending CPU ownership for exactly the bytes
  // it writes. Adjacent CPU dirt remains uploadable.
  tracker.mark_cpu_write(0x4000u, 0x100u);
  tracker.mark_gpu_write(0x4040u, 0x20u);
  cpu = tracker.cpu_dirty_ranges(0x4000u, 0x100u);
  assert(cpu.size() == 2);
  assert(cpu[0].address == 0x4000u && cpu[0].size == 0x40u);
  assert(cpu[1].address == 0x4060u && cpu[1].size == 0xA0u);

  // Xenon Memory, rather than either native API, builds requested-range upload
  // plans and tracks which bytes are valid in the device mirror.
  xenon::memory::GuestMemoryCoherency memory_writes;
  GuestMemoryGpuCoherency planned;
  planned.reset(0x10000u, false);
  memory_writes.mark_write(0x100u, 4u);
  memory_writes.mark_write(0x300u, 8u);
  auto plan = planned.plan_upload(memory_writes, 0x300u, 4u, 2u);
  assert(plan.exact_history);
  assert(plan.ranges.size() == 2u);
  assert(plan.ranges[0].address == 0x300u && plan.ranges[0].size == 2u);
  assert(plan.ranges[1].address == 0x302u && plan.ranges[1].size == 2u);
  assert(!planned.device_range_valid(0x300u, 4u));
  for (const auto& range : plan.ranges)
    planned.commit_cpu_upload(range.address, range.size);
  assert(planned.device_range_valid(0x300u, 4u));
  assert(!planned.device_range_valid(0x100u, 4u));

  // Dirt outside the first request remains pending even though the journal
  // epoch has advanced, and a later exact CPU write invalidates only its byte.
  plan = planned.plan_upload(memory_writes, 0x100u, 4u);
  assert(plan.ranges.size() == 1u && plan.ranges[0].address == 0x100u &&
         plan.ranges[0].size == 4u);
  memory_writes.mark_write(0x301u, 1u);
  plan = planned.plan_upload(memory_writes, 0x301u, 1u);
  assert(plan.ranges.size() == 1u && plan.ranges[0].address == 0x301u &&
         plan.ranges[0].size == 1u);
  assert(!planned.device_range_valid(0x300u, 4u));

  // CPU ownership is reconciled before a readback plan is emitted. A CPU write
  // that is newer than the GPU ownership removes only the overlapping bytes
  // from the requested GPU range instead of allowing stale device data to win.
  GuestMemoryGpuCoherency preplan_tracker;
  preplan_tracker.reset(0x10000u, false);
  preplan_tracker.mark_gpu_write(0x480u, 8u);
  memory_writes.mark_write(0x482u, 2u);
  const auto preplan = preplan_tracker.plan_readback(
      memory_writes, 0x480u, 8u);
  assert(preplan.ranges.size() == 2u);
  assert(preplan.ranges[0].address == 0x480u && preplan.ranges[0].size == 2u);
  assert(preplan.ranges[1].address == 0x484u && preplan.ranges[1].size == 4u);

  // GPU->CPU readback publication is source-aware. The mirror's own physical
  // write must not immediately appear as CPU dirt that needs uploading back to
  // the same device mirror.
  GuestMemoryGpuCoherency readback_tracker;
  readback_tracker.reset(0x10000u, false);
  readback_tracker.mark_gpu_write(0x500u, 8u);
  const auto readback_plan = readback_tracker.plan_readback(
      memory_writes, 0x500u, 8u);
  assert(readback_plan.action ==
         xenon::memory::GpuSynchronizationAction::Copy);
  assert(readback_plan.ranges.size() == 1u);
  const auto self_epoch = memory_writes.mark_write(0x500u, 8u);
  assert(readback_tracker.commit_gpu_download(
      memory_writes, readback_plan.ranges[0], 0x500u, 8u, self_epoch));
  assert(!readback_tracker.has_gpu_dirty(0x500u, 8u));
  assert(readback_tracker.cpu_dirty_ranges(0x500u, 8u).empty());
  assert(readback_tracker.device_range_valid(0x500u, 8u));
  auto echo_plan = readback_tracker.plan_upload(memory_writes, 0x500u, 8u);
  assert(echo_plan.ranges.empty());

  // An unrelated CPU publication before the mirror's own readback epoch must
  // survive acknowledgement and remain uploadable byte-precisely.
  readback_tracker.mark_gpu_write(0x600u, 8u);
  const auto conflict_plan = readback_tracker.plan_readback(
      memory_writes, 0x600u, 8u);
  memory_writes.mark_write(0x603u, 1u);
  const auto conflict_safe = readback_tracker.prepare_gpu_download(
      memory_writes, conflict_plan.ranges[0]);
  assert(conflict_safe.size() == 2u);
  assert(conflict_safe[0].address == 0x600u && conflict_safe[0].size == 3u);
  assert(conflict_safe[1].address == 0x604u && conflict_safe[1].size == 4u);
  for (const auto& safe : conflict_safe) {
    const auto conflict_self_epoch =
        memory_writes.mark_write(safe.address, safe.size);
    assert(readback_tracker.commit_gpu_download(
        memory_writes, safe, safe.address, safe.size, conflict_self_epoch));
  }
  const auto conflict_cpu = readback_tracker.cpu_dirty_ranges(0x600u, 8u);
  assert(conflict_cpu.size() == 1u);
  assert(conflict_cpu[0].address == 0x603u && conflict_cpu[0].size == 1u);
  assert(!readback_tracker.device_range_valid(0x603u, 1u));

  // GPU generations are range-local. An unrelated later GPU write must not
  // invalidate the planned ownership of 0x700, while a newer overlapping write
  // must survive the older readback commit.
  readback_tracker.mark_gpu_write(0x700u, 4u);
  const auto independent_plan = readback_tracker.plan_readback(
      memory_writes, 0x700u, 4u);
  readback_tracker.mark_gpu_write(0x800u, 4u);
  const auto independent_self_epoch = memory_writes.mark_write(0x700u, 4u);
  assert(readback_tracker.commit_gpu_download(
      memory_writes, independent_plan.ranges[0], 0x700u, 4u,
      independent_self_epoch));
  assert(!readback_tracker.has_gpu_dirty(0x700u, 4u));
  assert(readback_tracker.has_gpu_dirty(0x800u, 4u));

  readback_tracker.mark_gpu_write(0x900u, 8u);
  const auto stale_plan = readback_tracker.plan_readback(
      memory_writes, 0x900u, 8u);
  readback_tracker.mark_gpu_write(0x902u, 2u);
  const auto stale_safe = readback_tracker.prepare_gpu_download(
      memory_writes, stale_plan.ranges[0]);
  assert(stale_safe.size() == 2u);
  assert(stale_safe[0].address == 0x900u && stale_safe[0].size == 2u);
  assert(stale_safe[1].address == 0x904u && stale_safe[1].size == 4u);
  for (const auto& safe : stale_safe) {
    const auto epoch = memory_writes.mark_write(safe.address, safe.size);
    assert(readback_tracker.commit_gpu_download(
        memory_writes, safe, safe.address, safe.size, epoch));
  }
  assert(!readback_tracker.has_gpu_dirty(0x900u, 2u));
  assert(readback_tracker.has_gpu_dirty(0x902u, 2u));
  assert(!readback_tracker.has_gpu_dirty(0x904u, 4u));

  // UMA/mobile-capable policy is represented without changing Xbox semantics.
  // A shared-host-visible implementation receives the same requested ranges,
  // but executes cache/visibility work instead of redundant memory copies.
  GuestMemoryGpuCoherency uma_tracker;
  uma_tracker.reset(0x10000u, false,
                    xenon::memory::GpuMemoryTopology::SharedHostVisible);
  memory_writes.mark_write(0xA00u, 4u);
  const auto uma_upload = uma_tracker.plan_upload(memory_writes, 0xA00u, 4u);
  assert(uma_upload.action ==
         xenon::memory::GpuSynchronizationAction::VisibilityOnly);
  assert(uma_upload.ranges.size() == 1u);
  uma_tracker.mark_gpu_write(0xB00u, 4u);
  const auto uma_readback = uma_tracker.plan_readback(
      memory_writes, 0xB00u, 4u);
  assert(uma_readback.action ==
         xenon::memory::GpuSynchronizationAction::VisibilityOnly);
  assert(uma_readback.ranges.size() == 1u);
}

void test_memexport_stream_planning() {
  using namespace xenon::gpu;
  ResourceStateTracker tracker;
  write_memexport_stream(tracker, ShaderStage::Vertex, 7, 0x1000u, 37, 12,
                         Endian128::Swap8In32, 2, true);
  // Same base with a larger stream must merge to one maximum-sized range.
  write_memexport_stream(tracker, ShaderStage::Vertex, 9, 0x1000u, 37, 20);

  DecodedShader shader{};
  shader.stage = ShaderStage::Vertex;
  shader.reflection.writes_export_address = true;
  shader.reflection.memory_export_mask = 0b00101;
  shader.reflection.memory_exports = 2;
  shader.reflection.memexport_stream_constants = {7, 9};
  const auto plan = tracker.plan_memexport(shader);
  assert(plan.valid && plan.has_writes());
  assert(!plan.requires_dynamic_address_analysis);
  assert(plan.export_mask == 0b00101);
  assert(plan.streams.size() == 2);
  assert(plan.streams[0].valid && plan.streams[1].valid);
  assert(plan.streams[0].base_address_dwords == 0x1000u);
  assert(plan.streams[0].base_address_bytes == 0x4000u);
  assert(plan.streams[0].format == 37);
  assert(plan.streams[0].element_size_bytes == 8);
  assert(plan.streams[0].index_count == 12);
  assert(plan.streams[0].size_bytes() == 96);
  assert(plan.streams[0].endian == Endian128::Swap8In32);
  assert(plan.streams[0].number_format == 2);
  assert(plan.streams[0].red_blue_swap);
  assert(plan.ranges.size() == 1);
  assert(plan.ranges[0].base_address_dwords == 0x1000u);
  assert(plan.ranges[0].size_bytes == 160);

  // A shader with both a recoverable stream and another noncanonical eA path
  // may use the static range, but must still advertise dynamic analysis.
  shader.reflection.requires_dynamic_memexport_address = true;
  const auto mixed_plan = tracker.plan_memexport(shader);
  assert(mixed_plan.ranges.size() == 1);
  assert(mixed_plan.requires_dynamic_address_analysis);
  shader.reflection.requires_dynamic_memexport_address = false;

  // Pixel shaders use the second architectural float-constant bank.
  ResourceStateTracker pixel_tracker;
  write_memexport_stream(pixel_tracker, ShaderStage::Pixel, 4, 0x2000u, 6, 32);
  DecodedShader pixel{};
  pixel.stage = ShaderStage::Pixel;
  pixel.reflection.writes_export_address = true;
  pixel.reflection.memory_export_mask = 1;
  pixel.reflection.memory_exports = 1;
  pixel.reflection.memexport_stream_constants = {4};
  const auto pixel_plan = pixel_tracker.plan_memexport(pixel);
  assert(pixel_plan.ranges.size() == 1);
  assert(pixel_plan.streams[0].valid);
  assert(pixel_plan.streams[0].base_address_bytes == 0x8000u);
  assert(pixel_plan.streams[0].element_size_bytes == 4);
  assert(pixel_plan.ranges[0].size_bytes == 128);
}

void test_memexport_dynamic_and_invalid_streams_stay_explicit() {
  using namespace xenon::gpu;
  ResourceStateTracker tracker;
  DecodedShader dynamic{};
  dynamic.stage = ShaderStage::Vertex;
  dynamic.reflection.writes_export_address = true;
  dynamic.reflection.memory_export_mask = 1;
  dynamic.reflection.memory_exports = 1;
  auto plan = tracker.plan_memexport(dynamic);
  assert(plan.valid && plan.has_writes());
  assert(plan.requires_dynamic_address_analysis);
  assert(plan.streams.empty() && plan.ranges.empty());

  write_memexport_stream(tracker, ShaderStage::Vertex, 3, 0x1000u, 6, 8);
  // Break the required normalized-float guard word. This can legitimately be
  // encountered when a conditional export path isn't taken, so it is skipped
  // rather than being turned into a guessed memory range.
  tracker.apply({0x4000u + 3u * 4u + 1u, 0u});
  dynamic.reflection.memexport_stream_constants = {3};
  plan = tracker.plan_memexport(dynamic);
  assert(plan.streams.size() == 1 && !plan.streams[0].valid);
  assert(!plan.streams[0].error.empty());
  assert(plan.ranges.empty());
  assert(plan.requires_dynamic_address_analysis);

  // Byte-addressability alone must not make an arbitrary texture format a
  // legal memexport format. Format 33 is outside the Xenos color-export set.
  ResourceStateTracker invalid_format_tracker;
  write_memexport_stream(invalid_format_tracker, ShaderStage::Vertex, 3,
                         0x1000u, 33, 8);
  plan = invalid_format_tracker.plan_memexport(dynamic);
  assert(plan.streams.size() == 1 && !plan.streams[0].valid);
  assert(plan.streams[0].error.find("non-exportable") != std::string::npos);
  assert(plan.ranges.empty() && plan.requires_dynamic_address_analysis);

  // Reserved endian and number-format encodings also remain explicit.
  ResourceStateTracker invalid_encoding_tracker;
  write_memexport_stream(invalid_encoding_tracker, ShaderStage::Vertex, 3,
                         0x1000u, 6, 8, static_cast<Endian128>(6), 4);
  plan = invalid_encoding_tracker.plan_memexport(dynamic);
  assert(plan.streams.size() == 1 && !plan.streams[0].valid);
  assert(plan.streams[0].error.find("endian") != std::string::npos);

  ResourceStateTracker invalid_number_tracker;
  write_memexport_stream(invalid_number_tracker, ShaderStage::Vertex, 3,
                         0x1000u, 6, 8, Endian128::None, 4);
  plan = invalid_number_tracker.plan_memexport(dynamic);
  assert(plan.streams.size() == 1 && !plan.streams[0].valid);
  assert(plan.streams[0].error.find("number format") != std::string::npos);
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
  const auto snapshot_rectangle = decode_resolve_rectangle(
      tracker.snapshot(),
      std::span<const std::byte>(memory).subspan(0x1000, sizeof(vertices)),
      0x1000u);
  assert(snapshot_rectangle.valid);
  assert(snapshot_rectangle.left == rectangle.left &&
         snapshot_rectangle.top == rectangle.top &&
         snapshot_rectangle.right == rectangle.right &&
         snapshot_rectangle.bottom == rectangle.bottom);

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
  assert(resolve_plan.valid && !resolve_plan.native_color_average);
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
  test_guest_memory_coherency_ranges();
  test_memexport_stream_planning();
  test_memexport_dynamic_and_invalid_streams_stay_explicit();
  test_texture_descriptor();
  test_vertex_and_render_state();
  test_constant_buffer_abi();
  test_copy_resolve_state();
  test_color_target_plan();
  test_host_polygon_mode();
  test_preferred_polygon_offset();
  std::cout << "xenon_resource_ir_tests: ok\n";
}

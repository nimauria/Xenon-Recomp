#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>

#include "xenon/gpu/backend_capabilities.hpp"
#include "xenon/gpu/depth_format.hpp"

#if defined(XENON_TEST_DXC)
#include "xenon/gpu/dxc_shader_compiler.hpp"
#endif

#if defined(XENON_TEST_D3D12)
#include "xenon/gpu/d3d12/command_queue.hpp"
#include "xenon/gpu/d3d12/depth_target.hpp"
#include "xenon/gpu/d3d12/context.hpp"
#include "xenon/gpu/d3d12/guest_memory_mirror.hpp"
#include "xenon/gpu/d3d12/memory.hpp"
#include "xenon/gpu/d3d12/pipeline.hpp"
#include "xenon/gpu/d3d12/resource_layout.hpp"
#include "xenon/gpu/d3d12/render_target.hpp"
#include "xenon/gpu/d3d12/texture.hpp"
#include "xenon/memory/address_space.hpp"
#endif

#if defined(XENON_TEST_VULKAN)
#include "xenon/gpu/vulkan/command_queue.hpp"
#include "xenon/gpu/vulkan/depth_target.hpp"
#include "xenon/gpu/vulkan/context.hpp"
#include "xenon/gpu/vulkan/guest_memory_mirror.hpp"
#include "xenon/gpu/vulkan/memory.hpp"
#include "xenon/gpu/vulkan/pipeline.hpp"
#include "xenon/gpu/vulkan/resource_layout.hpp"
#include "xenon/gpu/vulkan/render_target.hpp"
#include "xenon/gpu/vulkan/texture.hpp"
#include "xenon/memory/address_space.hpp"
#endif

#if defined(XENON_TEST_DXC)
namespace {

xenon::gpu::LoweredShader test_vertex_shader() {
  xenon::gpu::LoweredShader shader{};
  shader.stage = xenon::gpu::ShaderStage::Vertex;
  shader.source_hash = 0x1001;
  shader.translation_hash = 0x2001;
  shader.entry_point = "main";
  shader.profile = "vs_6_0";
  shader.hlsl = R"(
struct Output { float4 position : SV_Position; };
Output main(uint vertex_id : SV_VertexID) {
  float2 p[3] = { float2(-0.75, -0.75), float2(0.0, 0.75), float2(0.75, -0.75) };
  Output result;
  result.position = float4(p[vertex_id], 0.0, 1.0);
  return result;
})";
  shader.complete = true;
  return shader;
}

xenon::gpu::LoweredShader test_pixel_shader() {
  xenon::gpu::LoweredShader shader{};
  shader.stage = xenon::gpu::ShaderStage::Pixel;
  shader.source_hash = 0x1002;
  shader.translation_hash = 0x2002;
  shader.entry_point = "main";
  shader.profile = "ps_6_0";
  shader.hlsl = R"(
struct Output {
  float4 c0 : SV_Target0;
  float4 c1 : SV_Target1;
};
Output main() {
  Output result;
  result.c0 = float4(0.0, 1.0, 0.0, 1.0);
  result.c1 = float4(0.0, 0.0, 1.0, 1.0);
  return result;
})";
  shader.complete = true;
  return shader;
}

}  // namespace
#endif

int main() {
  // This is a rendered-pixel validation, not merely API capability discovery.
  const auto capabilities = xenon::gpu::discover_backend_capabilities();
  assert(capabilities.size() == 2);
  assert(capabilities[0].kind == xenon::gpu::BackendKind::Vulkan);
  assert(capabilities[1].kind == xenon::gpu::BackendKind::Direct3D12);
#if defined(_WIN32)
  assert(!capabilities[0].detail.empty());
  if (capabilities[0].runtime_available)
    assert(capabilities[0].api_version != 0);
#endif
#if defined(XENON_TEST_VULKAN)
  {
  assert(capabilities[0].runtime_available);
  assert(capabilities[0].development_files_available);
  xenon::gpu::vulkan::Context vulkan_context;
  assert(vulkan_context.initialize({.enable_validation = true}));
  assert(vulkan_context.properties().device_type !=
         VK_PHYSICAL_DEVICE_TYPE_OTHER);
  xenon::gpu::vulkan::CommandQueue vulkan_queue;
  assert(vulkan_queue.initialize(vulkan_context.device(),
                                 vulkan_context.graphics_queue(),
                                 vulkan_context.graphics_queue_family()));
  xenon::memory::AddressSpace vulkan_guest_memory;
  assert(vulkan_guest_memory.initialize());
  xenon::gpu::vulkan::GuestMemoryMirror vulkan_mirror;
  assert(vulkan_mirror.initialize(vulkan_context.physical_device(),
                                  vulkan_context.device(), vulkan_queue,
                                  vulkan_guest_memory));
  assert(vulkan_mirror.synchronize());
  xenon::gpu::vulkan::ResourceLayout vulkan_resources;
  assert(vulkan_resources.initialize(vulkan_context.physical_device(),
                                     vulkan_context.device()));
  assert(vulkan_resources.ready());
  assert(vulkan_resources.bind_guest_memory(
      vulkan_mirror.buffer(), xenon::memory::kPhysicalMemorySize));
  xenon::gpu::TextureDescriptor vulkan_texture_descriptor{};
  vulkan_texture_descriptor.base_address = 0x8000;
  vulkan_texture_descriptor.width = 4;
  vulkan_texture_descriptor.height = 4;
  vulkan_texture_descriptor.depth = 1;
  vulkan_texture_descriptor.pitch = 32;
  vulkan_texture_descriptor.format = 6;
  vulkan_texture_descriptor.valid = true;
  for (std::uint32_t y = 0; y < 4; ++y)
    for (std::uint32_t x = 0; x < 4; ++x) {
      const std::uint32_t pixel = 0xFF000000u | (y << 8) | x;
      assert(vulkan_guest_memory.write_physical(
          vulkan_texture_descriptor.base_address + y * 128 + x * 4,
          std::as_bytes(std::span<const std::uint32_t>(&pixel, 1))));
    }
  const auto vulkan_decoded = xenon::gpu::decode_texture(
      vulkan_texture_descriptor,
      {vulkan_guest_memory.physical_data(), xenon::memory::kPhysicalMemorySize});
  assert(vulkan_decoded.valid);
  xenon::gpu::vulkan::TextureImage vulkan_texture;
  assert(vulkan_texture.initialize(vulkan_context.physical_device(),
                                   vulkan_context.device(), vulkan_queue,
                                   vulkan_decoded));
  assert(vulkan_resources.bind_texture(0, xenon::gpu::TextureDimension::TwoDOrStacked,
                                       vulkan_texture.view(), vulkan_texture.sampler()));
  xenon::gpu::EdramSurfaceLayout vulkan_surface{
      0, 64, 64, xenon::gpu::MsaaSamples::X1, false, false};
  xenon::gpu::vulkan::RenderTargetImage vulkan_target;
  assert(vulkan_target.initialize(vulkan_context.physical_device(),
                                  vulkan_context.device(), vulkan_queue,
                                  vulkan_surface,
                                  xenon::gpu::ColorRenderTargetFormat::R8G8B8A8));
  std::vector<std::byte> vulkan_ownership_upload(64u * 64u * 4u);
  for (std::size_t i = 0; i < vulkan_ownership_upload.size(); ++i)
    vulkan_ownership_upload[i] = std::byte(i & 0xFFu);
  assert(vulkan_target.upload(vulkan_queue, vulkan_ownership_upload, 64u * 4u));
  std::vector<std::byte> vulkan_ownership_readback;
  std::uint32_t vulkan_ownership_pitch{};
  assert(vulkan_target.readback(vulkan_queue, 0, 0, 2, 2,
                                vulkan_ownership_readback,
                                vulkan_ownership_pitch));
  assert(vulkan_ownership_pitch == 8u);
  assert(std::memcmp(vulkan_ownership_readback.data(),
                     vulkan_ownership_upload.data(), 8u) == 0);
  assert(std::memcmp(vulkan_ownership_readback.data() + 8u,
                     vulkan_ownership_upload.data() + 64u * 4u, 8u) == 0);
  VkClearColorValue vulkan_clear{};
  vulkan_clear.float32[0] = 0.25f;
  vulkan_clear.float32[3] = 1.0f;
  assert(vulkan_target.clear(vulkan_queue, vulkan_clear));
  xenon::gpu::EdramSurfaceLayout vulkan_surface_1{
      16, 64, 64, xenon::gpu::MsaaSamples::X1, false, false};
  xenon::gpu::vulkan::RenderTargetImage vulkan_target_1;
  assert(vulkan_target_1.initialize(
      vulkan_context.physical_device(), vulkan_context.device(), vulkan_queue,
      vulkan_surface_1, xenon::gpu::ColorRenderTargetFormat::R8G8B8A8));
  assert(vulkan_target_1.clear(vulkan_queue, vulkan_clear));
  xenon::gpu::EdramSurfaceLayout vulkan_depth_surface{
      64, 64, 64, xenon::gpu::MsaaSamples::X1, false, true};
  xenon::gpu::vulkan::DepthTargetImage vulkan_depth;
  assert(vulkan_depth.initialize(vulkan_context.physical_device(),
                                  vulkan_context.device(), vulkan_depth_surface,
                                  xenon::gpu::DepthRenderTargetFormat::D24S8));
  assert(vulkan_depth.clear(vulkan_queue, 1.0f, 0));
  std::vector<std::uint32_t> vulkan_depth_readback;
  std::uint32_t vulkan_depth_pitch{};
  assert(vulkan_depth.readback_sample(vulkan_queue, 0, 0, 0, 4, 4,
                                      vulkan_depth_readback,
                                      vulkan_depth_pitch));
  assert(vulkan_depth_pitch == 16 && vulkan_depth_readback.size() == 16);
  for (const auto packed : vulkan_depth_readback)
    assert(packed == xenon::gpu::pack_depth_stencil(
                         xenon::gpu::DepthRenderTargetFormat::D24S8,
                         1.0f, 0));
#if defined(XENON_TEST_DXC)
  for (const auto depth_format : {
           xenon::gpu::DepthRenderTargetFormat::D24S8,
           xenon::gpu::DepthRenderTargetFormat::D24FS8}) {
    xenon::gpu::EdramSurfaceLayout transfer_surface{
        80u + 16u * static_cast<unsigned>(depth_format), 4, 4,
        xenon::gpu::MsaaSamples::X4, false, true};
    xenon::gpu::vulkan::DepthTargetImage transfer_target;
    assert(transfer_target.initialize(
        vulkan_context.physical_device(), vulkan_context.device(),
        transfer_surface, depth_format));
    assert(transfer_target.clear(vulkan_queue, 0.0f, 0));
    constexpr std::array<float, 9> depths{
        0.0f, 0.25f, 0.5f, 0.999f, 1.0f,
        1.25f, 1.5f, 1.75f, 1.99999f};
    std::array<std::vector<std::uint32_t>, 4> expected;
    for (std::uint32_t sample = 0; sample < 4; ++sample) {
      expected[sample].resize(16);
      for (std::size_t pixel = 0; pixel < expected[sample].size(); ++pixel) {
        const auto value = depths[(pixel + sample * 2) % depths.size()];
        expected[sample][pixel] = xenon::gpu::pack_depth_stencil(
            depth_format, value,
            static_cast<std::uint8_t>(0x21u + sample * 0x30u + pixel));
      }
      const bool uploaded = transfer_target.upload_sample(
          vulkan_queue, sample, 0, 0, 4, 4, expected[sample], 16);
      if (!uploaded) std::cerr << transfer_target.error() << '\n';
      assert(uploaded);
    }
    for (std::uint32_t sample = 0; sample < 4; ++sample) {
      std::vector<std::uint32_t> returned;
      std::uint32_t pitch{};
      assert(transfer_target.readback_sample(
          vulkan_queue, sample, 0, 0, 4, 4, returned, pitch));
      if (returned != expected[sample]) {
        for (std::size_t i = 0; i < returned.size(); ++i)
          if (returned[i] != expected[sample][i]) {
            std::cerr << "Vulkan depth mismatch format="
                      << static_cast<unsigned>(depth_format)
                      << " sample=" << sample << " pixel=" << i
                      << " expected=0x" << std::hex << expected[sample][i]
                      << " actual=0x" << returned[i] << std::dec << '\n';
            break;
          }
      }
      assert(pitch == 16 && returned == expected[sample]);
    }
  }
  xenon::gpu::EdramSurfaceLayout vulkan_msaa_surface{
      96, 8, 8, xenon::gpu::MsaaSamples::X4, false, false};
  xenon::gpu::vulkan::RenderTargetImage vulkan_msaa_target;
  assert(vulkan_msaa_target.initialize(
      vulkan_context.physical_device(), vulkan_context.device(), vulkan_queue,
      vulkan_msaa_surface, xenon::gpu::ColorRenderTargetFormat::R8G8B8A8));
  VkClearColorValue vulkan_msaa_clear{};
  vulkan_msaa_clear.float32[3] = 1.0f;
  assert(vulkan_msaa_target.clear(vulkan_queue, vulkan_msaa_clear));
  constexpr std::array<std::array<std::uint8_t, 4>, 4> sample_colors{{
      {{255, 0, 0, 255}}, {{0, 255, 0, 255}},
      {{0, 0, 255, 255}}, {{255, 255, 255, 255}}}};
  for (std::uint32_t guest_sample = 0; guest_sample < 4; ++guest_sample) {
    std::vector<std::byte> upload(8u * 8u * 4u);
    for (std::size_t pixel = 0; pixel < 64; ++pixel)
      std::memcpy(upload.data() + pixel * 4,
                  sample_colors[guest_sample].data(), 4);
    assert(vulkan_msaa_target.upload_sample(
        vulkan_queue, guest_sample, 0, 0, 8, 8, upload, 8u * 4u));
  }
  for (std::uint32_t guest_sample = 0; guest_sample < 4; ++guest_sample) {
    std::vector<std::byte> selected;
    std::uint32_t selected_pitch{};
    assert(vulkan_msaa_target.readback_sample(
        vulkan_queue, guest_sample, 0, 0, 8, 8, selected, selected_pitch));
    assert(selected_pitch == 32u && selected.size() == 256u);
    for (std::size_t pixel = 0; pixel < 64; ++pixel)
      assert(std::memcmp(selected.data() + pixel * 4,
                         sample_colors[guest_sample].data(), 4) == 0);
  }
  xenon::gpu::EdramSurfaceLayout vulkan_x2_surface{
      112, 8, 8, xenon::gpu::MsaaSamples::X2, false, false};
  for (const bool native_2x : {true, false}) {
    xenon::gpu::vulkan::RenderTargetImage target;
    assert(target.initialize(vulkan_context.physical_device(),
                             vulkan_context.device(), vulkan_queue,
                             vulkan_x2_surface,
                             xenon::gpu::ColorRenderTargetFormat::R8G8B8A8,
                             native_2x));
    assert(target.clear(vulkan_queue, vulkan_msaa_clear));
    for (std::uint32_t guest_sample = 0; guest_sample < 2; ++guest_sample) {
      std::vector<std::byte> upload(8u * 8u * 4u);
      for (std::size_t pixel = 0; pixel < 64; ++pixel)
        std::memcpy(upload.data() + pixel * 4,
                    sample_colors[guest_sample].data(), 4);
      assert(target.upload_sample(vulkan_queue, guest_sample, 0, 0, 8, 8,
                                  upload, 32));
    }
    for (std::uint32_t guest_sample = 0; guest_sample < 2; ++guest_sample) {
      std::vector<std::byte> selected;
      std::uint32_t pitch{};
      assert(target.readback_sample(vulkan_queue, guest_sample, 0, 0, 8, 8,
                                    selected, pitch));
      for (std::size_t pixel = 0; pixel < 64; ++pixel)
        assert(std::memcmp(selected.data() + pixel * 4,
                           sample_colors[guest_sample].data(), 4) == 0);
    }
  }
  xenon::gpu::DxcShaderCompiler vulkan_compiler;
  assert(vulkan_compiler.available());
  xenon::gpu::ShaderCompileOptions vulkan_compile_options{};
  vulkan_compile_options.format = xenon::gpu::ShaderBinaryFormat::Spirv;
  const auto vulkan_vs = vulkan_compiler.compile(test_vertex_shader(),
                                                  vulkan_compile_options);
  const auto vulkan_ps = vulkan_compiler.compile(test_pixel_shader(),
                                                  vulkan_compile_options);
  assert(vulkan_vs.succeeded && vulkan_ps.succeeded);
  xenon::gpu::RasterState vulkan_raster{};
  xenon::gpu::vulkan::GraphicsPipeline vulkan_pipeline;
  const std::array vulkan_formats{vulkan_target.format(),
                                  vulkan_target_1.format()};
  const std::array<std::uint8_t, 2> vulkan_write_masks{0xFu, 0xFu};
  const std::array<xenon::gpu::BlendState, 2> vulkan_blend_states{};
  xenon::gpu::DepthTargetDescriptor vulkan_depth_state{};
  vulkan_depth_state.test_enabled = true;
  vulkan_depth_state.write_enabled = true;
  vulkan_depth_state.function = xenon::gpu::CompareFunction::Less;
  assert(vulkan_pipeline.initialize(
      vulkan_context.device(), vulkan_resources.pipeline_layout(), vulkan_vs,
      vulkan_ps, nullptr, vulkan_formats, xenon::gpu::MsaaSamples::X1,
      xenon::gpu::HostPrimitiveTopology::TriangleList, vulkan_raster,
      vulkan_write_masks, vulkan_blend_states, vulkan_depth.format(),
      &vulkan_depth_state));
  assert(vulkan_queue.execute([&](VkCommandBuffer command) {
    vulkan_target.transition_to_color_attachment(command);
    vulkan_target_1.transition_to_color_attachment(command);
    vulkan_depth.transition_to_depth_attachment(command);
    std::array<VkRenderingAttachmentInfo, 2> attachments{};
    attachments[0].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachments[0].imageView = vulkan_target.view();
    attachments[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachments[1].imageView = vulkan_target_1.view();
    attachments[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {vulkan_target.width(), vulkan_target.height()};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 2;
    rendering.pColorAttachments = attachments.data();
    VkRenderingAttachmentInfo depth_attachment{
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth_attachment.imageView = vulkan_depth.view();
    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    rendering.pDepthAttachment = &depth_attachment;
    rendering.pStencilAttachment = &depth_attachment;
    vkCmdBeginRendering(command, &rendering);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      vulkan_pipeline.pipeline());
    const VkViewport viewport{0.0f, 0.0f, float(vulkan_target.width()),
                              float(vulkan_target.height()), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {vulkan_target.width(), vulkan_target.height()}};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRendering(command);
  }));
  std::vector<std::byte> vulkan_region;
  std::uint32_t vulkan_region_pitch{};
  assert(vulkan_target.readback(vulkan_queue, 31, 31, 33, 33,
                                vulkan_region, vulkan_region_pitch));
  assert(vulkan_region_pitch == 8 && vulkan_region.size() == 16);
  assert(std::to_integer<unsigned>(vulkan_region[5]) > 200u);
  xenon::gpu::vulkan::Buffer vulkan_draw_readback;
  xenon::gpu::vulkan::Buffer vulkan_draw_readback_1;
  constexpr VkDeviceSize kVulkanDrawBytes = 64u * 64u * 4u;
  assert(vulkan_draw_readback.initialize(
      vulkan_context.physical_device(), vulkan_context.device(),
      kVulkanDrawBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
  assert(vulkan_draw_readback_1.initialize(
      vulkan_context.physical_device(), vulkan_context.device(),
      kVulkanDrawBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
  assert(vulkan_queue.execute([&](VkCommandBuffer command) {
    std::array<VkImageMemoryBarrier2, 2> barriers{};
    for (auto& barrier : barriers) {
      barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
      barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
      barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
      barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    barriers[0].image = vulkan_target.image();
    barriers[1].image = vulkan_target_1.image();
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 2;
    dependency.pImageMemoryBarriers = barriers.data();
    vkCmdPipelineBarrier2(command, &dependency);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {64, 64, 1};
    vkCmdCopyImageToBuffer(command, vulkan_target.image(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           vulkan_draw_readback.buffer(), 1, &copy);
    vkCmdCopyImageToBuffer(command, vulkan_target_1.image(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           vulkan_draw_readback_1.buffer(), 1, &copy);
    std::array<VkBufferMemoryBarrier2, 2> readback_barriers{};
    for (auto& barrier : readback_barriers) {
      barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
      barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
      barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
      barrier.size = kVulkanDrawBytes;
    }
    readback_barriers[0].buffer = vulkan_draw_readback.buffer();
    readback_barriers[1].buffer = vulkan_draw_readback_1.buffer();
    dependency.imageMemoryBarrierCount = 0;
    dependency.pImageMemoryBarriers = nullptr;
    dependency.bufferMemoryBarrierCount = 2;
    dependency.pBufferMemoryBarriers = readback_barriers.data();
    vkCmdPipelineBarrier2(command, &dependency);
  }));
  std::span<std::byte> vulkan_draw_pixels;
  std::span<std::byte> vulkan_draw_pixels_1;
  assert(vulkan_draw_readback.map(vulkan_draw_pixels));
  assert(vulkan_draw_readback_1.map(vulkan_draw_pixels_1));
  const auto vulkan_center = (32u * 64u + 32u) * 4u;
  assert(std::to_integer<unsigned>(vulkan_draw_pixels[vulkan_center + 1u]) > 200u);
  assert(std::to_integer<unsigned>(vulkan_draw_pixels_1[vulkan_center + 2u]) > 200u);
  vulkan_draw_readback.unmap();
  vulkan_draw_readback_1.unmap();
#endif
  constexpr std::uint32_t kVulkanProbeAddress = 0x4321;
  const std::array<std::byte, 1> vulkan_probe{std::byte{0x6B}};
  assert(vulkan_guest_memory.write_physical(kVulkanProbeAddress, vulkan_probe));
  assert(vulkan_mirror.synchronize());
  xenon::gpu::vulkan::Buffer vulkan_readback;
  assert(vulkan_readback.initialize(
      vulkan_context.physical_device(), vulkan_context.device(), 1,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
  assert(vulkan_queue.execute([&](VkCommandBuffer command) {
    VkBufferCopy copy{kVulkanProbeAddress, 0, 1};
    vkCmdCopyBuffer(command, vulkan_mirror.buffer(), vulkan_readback.buffer(),
                    1, &copy);
    VkBufferMemoryBarrier2 barrier{
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    barrier.buffer = vulkan_readback.buffer();
    barrier.size = 1;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(command, &dependency);
  }));
  std::span<std::byte> vulkan_mapped;
  assert(vulkan_readback.map(vulkan_mapped));
  assert(vulkan_mapped[0] == std::byte{0x6B});
  vulkan_readback.unmap();
  std::cout << "Vulkan device: "
            << vulkan_context.properties().device_name << '\n';
  }
#endif
#if defined(XENON_TEST_D3D12)
  xenon::gpu::d3d12::Context context;
  if (context.initialize({.enable_debug_layer = true})) {
    xenon::gpu::d3d12::CommandQueue queue;
    assert(queue.initialize(context.device()));
    assert(queue.execute([](ID3D12GraphicsCommandList*) {}));
    xenon::gpu::d3d12::Buffer upload;
    assert(upload.initialize(context.device(), 4096, D3D12_HEAP_TYPE_UPLOAD,
                             D3D12_RESOURCE_STATE_GENERIC_READ));
    std::span<std::byte> mapped;
    assert(upload.map(mapped) && mapped.size() == 4096);
    mapped[0] = std::byte{0x5A};
    upload.unmap();

    // Exercise the real 512 MiB Xbox physical-memory mirror, including a
    // post-initialization dirty-page update and GPU-to-CPU verification.
    xenon::memory::AddressSpace guest_memory;
    assert(guest_memory.initialize());
    xenon::gpu::d3d12::GuestMemoryMirror mirror;
    assert(mirror.initialize(context.device(), queue, guest_memory));
    assert(mirror.synchronize());
    xenon::gpu::d3d12::ResourceLayout resources;
    assert(resources.initialize(context.device()));
    assert(resources.ready());
    assert(resources.bind_guest_memory(mirror.resource(),
                                       xenon::memory::kPhysicalMemorySize));
    xenon::gpu::TextureDescriptor texture_descriptor{};
    texture_descriptor.base_address = 0xC000;
    texture_descriptor.width = 4;
    texture_descriptor.height = 4;
    texture_descriptor.depth = 1;
    texture_descriptor.pitch = 32;
    texture_descriptor.format = 6;
    texture_descriptor.valid = true;
    for (std::uint32_t y = 0; y < 4; ++y)
      for (std::uint32_t x = 0; x < 4; ++x) {
        const std::uint32_t pixel = 0xFF000000u | (y << 8) | x;
        assert(guest_memory.write_physical(
            texture_descriptor.base_address + y * 128 + x * 4,
            std::as_bytes(std::span<const std::uint32_t>(&pixel, 1))));
      }
    const auto decoded = xenon::gpu::decode_texture(
        texture_descriptor,
        {guest_memory.physical_data(), xenon::memory::kPhysicalMemorySize});
    assert(decoded.valid);
    xenon::gpu::d3d12::TextureImage texture;
    assert(texture.initialize(context.device(), queue, decoded));
    assert(resources.bind_texture(0, xenon::gpu::TextureDimension::TwoDOrStacked,
                                  texture.resource(), texture.format(), 1,
                                  texture_descriptor));
    xenon::gpu::EdramSurfaceLayout d3d_surface{
        0, 64, 64, xenon::gpu::MsaaSamples::X1, false, false};
    xenon::gpu::d3d12::RenderTargetImage d3d_target;
    assert(d3d_target.initialize(context.device(), d3d_surface,
                                 xenon::gpu::ColorRenderTargetFormat::R8G8B8A8));
    std::vector<std::byte> d3d_ownership_upload(64u * 64u * 4u);
    for (std::size_t i = 0; i < d3d_ownership_upload.size(); ++i)
      d3d_ownership_upload[i] = std::byte(i & 0xFFu);
    assert(d3d_target.upload(queue, d3d_ownership_upload, 64u * 4u));
    std::vector<std::byte> d3d_ownership_readback;
    std::uint32_t d3d_ownership_pitch{};
    assert(d3d_target.readback(queue, 0, 0, 2, 2,
                               d3d_ownership_readback,
                               d3d_ownership_pitch));
    assert(d3d_ownership_pitch == 8u);
    assert(std::memcmp(d3d_ownership_readback.data(),
                       d3d_ownership_upload.data(), 8u) == 0);
    assert(std::memcmp(d3d_ownership_readback.data() + 8u,
                       d3d_ownership_upload.data() + 64u * 4u, 8u) == 0);
    const float d3d_clear[4]{0.25f, 0.0f, 0.0f, 1.0f};
    assert(d3d_target.clear(queue, d3d_clear));
    xenon::gpu::EdramSurfaceLayout d3d_surface_1{
        16, 64, 64, xenon::gpu::MsaaSamples::X1, false, false};
    xenon::gpu::d3d12::RenderTargetImage d3d_target_1;
    assert(d3d_target_1.initialize(
        context.device(), d3d_surface_1,
        xenon::gpu::ColorRenderTargetFormat::R8G8B8A8));
    assert(d3d_target_1.clear(queue, d3d_clear));
    xenon::gpu::EdramSurfaceLayout d3d_depth_surface{
        64, 64, 64, xenon::gpu::MsaaSamples::X1, false, true};
    xenon::gpu::d3d12::DepthTargetImage d3d_depth;
    assert(d3d_depth.initialize(context.device(), d3d_depth_surface,
                                 xenon::gpu::DepthRenderTargetFormat::D24S8));
    assert(d3d_depth.clear(queue, 1.0f, 0));
#if defined(XENON_TEST_DXC)
    for (const auto depth_format : {
             xenon::gpu::DepthRenderTargetFormat::D24S8,
             xenon::gpu::DepthRenderTargetFormat::D24FS8}) {
      xenon::gpu::EdramSurfaceLayout transfer_surface{
          80u + 16u * static_cast<unsigned>(depth_format), 4, 4,
          xenon::gpu::MsaaSamples::X4, false, true};
      xenon::gpu::d3d12::DepthTargetImage transfer_target;
      assert(transfer_target.initialize(context.device(), transfer_surface,
                                        depth_format));
      assert(transfer_target.clear(queue, 0.0f, 0));
      constexpr std::array<float, 9> depths{
          0.0f, 0.25f, 0.5f, 0.999f, 1.0f,
          1.25f, 1.5f, 1.75f, 1.99999f};
      std::array<std::vector<std::uint32_t>, 4> expected;
      for (std::uint32_t sample = 0; sample < 4; ++sample) {
        expected[sample].resize(16);
        for (std::size_t pixel = 0; pixel < 16; ++pixel) {
          const auto value = depth_format == xenon::gpu::DepthRenderTargetFormat::D24S8
              ? (std::min)(depths[(pixel + sample) % 5], 1.0f)
              : depths[(pixel + sample) % depths.size()];
          expected[sample][pixel] = xenon::gpu::pack_depth_stencil(
              depth_format, value,
              static_cast<std::uint8_t>(0x21u + sample * 0x30u + pixel));
        }
        const bool uploaded = transfer_target.upload_sample(
            queue, sample, 0, 0, 4, 4, expected[sample], 16);
        if (!uploaded) std::cerr << transfer_target.error() << '\n';
        assert(uploaded);
      }
      for (std::uint32_t sample = 0; sample < 4; ++sample) {
        std::vector<std::uint32_t> returned;
        std::uint32_t pitch{};
        assert(transfer_target.readback_sample(queue, sample, 0, 0, 4, 4,
                                               returned, pitch));
        if (returned != expected[sample]) {
          for (std::size_t i = 0; i < returned.size(); ++i)
            if (returned[i] != expected[sample][i]) {
              std::cerr << "depth mismatch format="
                        << static_cast<unsigned>(depth_format)
                        << " sample=" << sample << " pixel=" << i
                        << " expected=0x" << std::hex << expected[sample][i]
                        << " actual=0x" << returned[i] << std::dec << '\n';
              break;
            }
        }
        assert(pitch == 16 && returned == expected[sample]);
      }
    }
    xenon::gpu::EdramSurfaceLayout d3d_msaa_surface{
        96, 8, 8, xenon::gpu::MsaaSamples::X4, false, false};
    xenon::gpu::d3d12::RenderTargetImage d3d_msaa_target;
    assert(d3d_msaa_target.initialize(
        context.device(), d3d_msaa_surface,
        xenon::gpu::ColorRenderTargetFormat::R8G8B8A8));
    const float d3d_msaa_clear[4]{0, 0, 0, 1};
    assert(d3d_msaa_target.clear(queue, d3d_msaa_clear));
    constexpr std::array<std::array<std::uint8_t, 4>, 4> d3d_sample_colors{{
        {{255, 0, 0, 255}}, {{0, 255, 0, 255}},
        {{0, 0, 255, 255}}, {{255, 255, 255, 255}}}};
    for (std::uint32_t guest_sample = 0; guest_sample < 4; ++guest_sample) {
      std::vector<std::byte> sample_upload(8u * 8u * 4u);
      for (std::size_t pixel = 0; pixel < 64; ++pixel)
        std::memcpy(sample_upload.data() + pixel * 4,
                    d3d_sample_colors[guest_sample].data(), 4);
      assert(d3d_msaa_target.upload_sample(
          queue, guest_sample, 0, 0, 8, 8, sample_upload, 8u * 4u));
    }
    for (std::uint32_t guest_sample = 0; guest_sample < 4; ++guest_sample) {
      std::vector<std::byte> selected;
      std::uint32_t selected_pitch{};
      assert(d3d_msaa_target.readback_sample(
          queue, guest_sample, 0, 0, 8, 8, selected, selected_pitch));
      assert(selected_pitch == 32u && selected.size() == 256u);
      for (std::size_t pixel = 0; pixel < 64; ++pixel)
        assert(std::memcmp(selected.data() + pixel * 4,
                           d3d_sample_colors[guest_sample].data(), 4) == 0);
    }
    xenon::gpu::EdramSurfaceLayout d3d_x2_surface{
        112, 8, 8, xenon::gpu::MsaaSamples::X2, false, false};
    for (const bool native_2x : {true, false}) {
      xenon::gpu::d3d12::RenderTargetImage target;
      assert(target.initialize(
          context.device(), d3d_x2_surface,
          xenon::gpu::ColorRenderTargetFormat::R8G8B8A8, native_2x));
      assert(target.clear(queue, d3d_msaa_clear));
      for (std::uint32_t guest_sample = 0; guest_sample < 2; ++guest_sample) {
        std::vector<std::byte> upload(8u * 8u * 4u);
        for (std::size_t pixel = 0; pixel < 64; ++pixel)
          std::memcpy(upload.data() + pixel * 4,
                      d3d_sample_colors[guest_sample].data(), 4);
        assert(target.upload_sample(queue, guest_sample, 0, 0, 8, 8,
                                    upload, 32));
      }
      for (std::uint32_t guest_sample = 0; guest_sample < 2; ++guest_sample) {
        std::vector<std::byte> selected;
        std::uint32_t pitch{};
        assert(target.readback_sample(queue, guest_sample, 0, 0, 8, 8,
                                      selected, pitch));
        for (std::size_t pixel = 0; pixel < 64; ++pixel)
          assert(std::memcmp(selected.data() + pixel * 4,
                             d3d_sample_colors[guest_sample].data(), 4) == 0);
      }
    }
    xenon::gpu::DxcShaderCompiler d3d_compiler;
    assert(d3d_compiler.available());
    const auto d3d_vs = d3d_compiler.compile(test_vertex_shader());
    const auto d3d_ps = d3d_compiler.compile(test_pixel_shader());
    assert(d3d_vs.succeeded && d3d_ps.succeeded);
    xenon::gpu::RasterState d3d_raster{};
    xenon::gpu::d3d12::GraphicsPipeline d3d_pipeline;
    const std::array d3d_formats{d3d_target.format(), d3d_target_1.format()};
    const std::array<std::uint8_t, 2> d3d_write_masks{0xFu, 0xFu};
    const std::array<xenon::gpu::BlendState, 2> d3d_blend_states{};
    xenon::gpu::DepthTargetDescriptor d3d_depth_state{};
    d3d_depth_state.test_enabled = true;
    d3d_depth_state.write_enabled = true;
    d3d_depth_state.function = xenon::gpu::CompareFunction::Less;
    assert(d3d_pipeline.initialize(
        context.device(), resources.root_signature(), d3d_vs, d3d_ps,
        nullptr, d3d_formats, xenon::gpu::MsaaSamples::X1,
        xenon::gpu::HostPrimitiveTopology::TriangleList, d3d_raster,
        d3d_write_masks, d3d_blend_states, d3d_depth.format(),
        &d3d_depth_state));
    assert(queue.execute([&](ID3D12GraphicsCommandList* list) {
      list->SetPipelineState(d3d_pipeline.pipeline());
      list->SetGraphicsRootSignature(resources.root_signature());
      ID3D12DescriptorHeap* heaps[]{resources.resource_heap(),
                                    resources.sampler_heap()};
      list->SetDescriptorHeaps(2, heaps);
      list->SetGraphicsRootConstantBufferView(
          0, resources.constants().resource()->GetGPUVirtualAddress());
      list->SetGraphicsRootDescriptorTable(
          1, resources.resource_heap()->GetGPUDescriptorHandleForHeapStart());
      list->SetGraphicsRootDescriptorTable(
          2, resources.sampler_heap()->GetGPUDescriptorHandleForHeapStart());
      const D3D12_VIEWPORT viewport{0.0f, 0.0f, float(d3d_target.width()),
                                    float(d3d_target.height()), 0.0f, 1.0f};
      const D3D12_RECT scissor{0, 0, static_cast<LONG>(d3d_target.width()),
                               static_cast<LONG>(d3d_target.height())};
      list->RSSetViewports(1, &viewport);
      list->RSSetScissorRects(1, &scissor);
      const std::array rtvs{d3d_target.rtv(), d3d_target_1.rtv()};
      const auto dsv = d3d_depth.dsv();
      list->OMSetRenderTargets(static_cast<UINT>(rtvs.size()), rtvs.data(),
                               FALSE, &dsv);
      list->IASetPrimitiveTopology(d3d_pipeline.native_topology());
      list->DrawInstanced(3, 1, 0, 0);
    }));
    std::vector<std::byte> d3d_region;
    std::uint32_t d3d_region_pitch{};
    assert(d3d_target.readback(queue, 31, 31, 33, 33, d3d_region,
                               d3d_region_pitch));
    assert(d3d_region_pitch == 8 && d3d_region.size() == 16);
    assert(std::to_integer<unsigned>(d3d_region[5]) > 200u);
    const auto draw_desc = d3d_target.resource()->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT draw_footprint{};
    UINT draw_rows{};
    UINT64 draw_row_bytes{};
    UINT64 draw_total_bytes{};
    context.device()->GetCopyableFootprints(&draw_desc, 0, 1, 0,
                                             &draw_footprint, &draw_rows,
                                             &draw_row_bytes, &draw_total_bytes);
    xenon::gpu::d3d12::Buffer d3d_draw_readback;
    xenon::gpu::d3d12::Buffer d3d_draw_readback_1;
    assert(d3d_draw_readback.initialize(context.device(), draw_total_bytes,
                                         D3D12_HEAP_TYPE_READBACK,
                                         D3D12_RESOURCE_STATE_COPY_DEST));
    assert(d3d_draw_readback_1.initialize(context.device(), draw_total_bytes,
                                           D3D12_HEAP_TYPE_READBACK,
                                           D3D12_RESOURCE_STATE_COPY_DEST));
    assert(queue.execute([&](ID3D12GraphicsCommandList* list) {
      std::array<D3D12_RESOURCE_BARRIER, 2> barriers{};
      const std::array<ID3D12Resource*, 2> draw_targets{
          d3d_target.resource(), d3d_target_1.resource()};
      for (std::size_t i = 0; i < barriers.size(); ++i) {
        auto& barrier = barriers[i];
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = draw_targets[i];
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
      }
      list->ResourceBarrier(static_cast<UINT>(barriers.size()),
                            barriers.data());
      const D3D12_TEXTURE_COPY_LOCATION source{
          d3d_target.resource(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
          {.SubresourceIndex = 0}};
      const D3D12_TEXTURE_COPY_LOCATION source_1{
          d3d_target_1.resource(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
          {.SubresourceIndex = 0}};
      const D3D12_TEXTURE_COPY_LOCATION destination{
          d3d_draw_readback.resource(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
          {.PlacedFootprint = draw_footprint}};
      const D3D12_TEXTURE_COPY_LOCATION destination_1{
          d3d_draw_readback_1.resource(),
          D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
          {.PlacedFootprint = draw_footprint}};
      list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
      list->CopyTextureRegion(&destination_1, 0, 0, 0, &source_1, nullptr);
    }));
    std::span<std::byte> d3d_draw_pixels;
    std::span<std::byte> d3d_draw_pixels_1;
    assert(d3d_draw_readback.map(d3d_draw_pixels));
    assert(d3d_draw_readback_1.map(d3d_draw_pixels_1));
    const auto d3d_center = std::size_t(draw_footprint.Footprint.RowPitch) * 32u + 32u * 4u;
    assert(std::to_integer<unsigned>(d3d_draw_pixels[d3d_center + 1u]) > 200u);
    assert(std::to_integer<unsigned>(d3d_draw_pixels_1[d3d_center + 2u]) > 200u);
    d3d_draw_readback.unmap();
    d3d_draw_readback_1.unmap();
#endif
    constexpr std::uint32_t kProbeAddress = 0x1234;
    const std::array<std::byte, 1> probe{std::byte{0xA5}};
    assert(guest_memory.write_physical(kProbeAddress, probe));
    assert(mirror.synchronize());
    xenon::gpu::d3d12::Buffer readback;
    assert(readback.initialize(context.device(), 1, D3D12_HEAP_TYPE_READBACK,
                               D3D12_RESOURCE_STATE_COPY_DEST));
    assert(queue.execute([&](ID3D12GraphicsCommandList* list) {
      list->CopyBufferRegion(readback.resource(), 0, mirror.resource(),
                             kProbeAddress, 1);
    }));
    assert(readback.map(mapped) && mapped[0] == std::byte{0xA5});
    readback.unmap();
    std::cout << "D3D12 adapter: " << context.properties().adapter_name << '\n';
  } else {
    std::cout << "D3D12 unavailable: " << context.error() << '\n';
  }
#endif
  std::cout << capabilities[0].detail << '\n';
  std::cout << "xenon_backend_capability_tests: ok\n";
}

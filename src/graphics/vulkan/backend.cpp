#include "xenon/gpu/vulkan/backend.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <variant>
#include <span>
#include <unordered_set>
#include <unordered_map>

#include "xenon/gpu/vulkan/command_queue.hpp"
#include "xenon/gpu/vulkan/depth_target.hpp"
#include "xenon/gpu/depth_format.hpp"
#include "xenon/gpu/vulkan/edram_transfer.hpp"
#include "xenon/gpu/vulkan/gpu_resolve.hpp"
#include "xenon/gpu/vulkan/guest_memory_mirror.hpp"
#include "xenon/gpu/vulkan/pipeline.hpp"
#include "xenon/gpu/vulkan/presentation.hpp"
#include "xenon/gpu/vulkan/resource_layout.hpp"
#include "xenon/gpu/vulkan/scanout.hpp"
#include "xenon/gpu/vulkan/render_target.hpp"
#include "xenon/gpu/vulkan/texture.hpp"
#include "graphics/common/backend_core.hpp"
#include "xenon/gpu/resource_ir.hpp"
#include "xenon/gpu/edram_ownership.hpp"
#include "xenon/gpu/texture.hpp"
#include "xenon/gpu/primitive_processor.hpp"
#include "xenon/logging/logger.hpp"
#include "xenon/memory/types.hpp"
#ifdef XENON_HAS_DXC
#include "xenon/gpu/dxc_shader_compiler.hpp"
#include "xenon/gpu/shader_translation.hpp"
#endif

namespace xenon::gpu::vulkan {

struct BackendApi {
  static constexpr std::string_view kName = "Vulkan";
  using Context = vulkan::Context;
  using CommandQueue = vulkan::CommandQueue;
  using PresentationSwapchain = vulkan::PresentationSwapchain;
  using GuestMemoryMirror = vulkan::GuestMemoryMirror;
  using ResourceLayout = vulkan::ResourceLayout;
  using Buffer = vulkan::Buffer;
  using TextureImage = vulkan::TextureImage;
  using RenderTargetImage = vulkan::RenderTargetImage;
  using DepthTargetImage = vulkan::DepthTargetImage;
  using GraphicsPipeline = vulkan::GraphicsPipeline;
  using Format = VkFormat;
  static constexpr VkFormat kUndefinedFormat = VK_FORMAT_UNDEFINED;
  static VkFormat native_color_format(ColorRenderTargetFormat format) {
    return color_render_target_format(format);
  }
  // Dynamic rendering accepts an undefined format for an unused color slot.
  static VkFormat color_format(const vulkan::RenderTargetImage* target) {
    return target ? target->format() : VK_FORMAT_UNDEFINED;
  }
  // VK_CULL_MODE_FRONT_AND_BACK is native.
  static constexpr bool kEmulatesFrontAndBackCull = false;
  // VK_BLEND_FACTOR_CONSTANT_ALPHA is native.
  static constexpr bool kSharedBlendFactorConstant = false;
  static constexpr bool kSupportsDirectColorTransfer = true;
  static constexpr bool kSupportsDirectDepthTransfer = true;
  static constexpr bool kSupportsCanonicalGpuLoad = true;
#ifdef XENON_HAS_DXC
  static constexpr ShaderBinaryFormat kShaderBinaryFormat =
      ShaderBinaryFormat::Spirv;
  static constexpr std::string_view kShaderBinaryName = "SPIR-V";
#endif
};

class Backend::Impl : public detail::BackendCore<Backend::Impl, BackendApi> {
 public:
  ~Impl() {
    // Native draws may still be using render targets, textures, descriptor
    // snapshots and upload arenas owned by this Impl. Drain them before member
    // destruction starts.
    (void)queue.wait_idle();
    close_pipeline_cache();
  }

  // Persistent pipeline cache (GPU V3): pipelines this device built in
  // earlier sessions are created from the driver's cached state instead of
  // being compiled again (AC6's first Vulkan cutscene built 31 pipelines in
  // 1.8 s). Saved next to the shader cache when the backend is destroyed.
  VkPipelineCache pipeline_cache_handle{VK_NULL_HANDLE};
  std::filesystem::path pipeline_cache_path{};
  [[nodiscard]] VkPipelineCache native_pipeline_cache() const noexcept {
    return pipeline_cache_handle;
  }
  void open_pipeline_cache(const std::filesystem::path& path) {
    close_pipeline_cache();
    pipeline_cache_path = path;
    std::vector<char> data;
    if (!path.empty()) {
      std::ifstream file(path, std::ios::binary);
      if (file) data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info.initialDataSize = data.size();
    info.pInitialData = data.empty() ? nullptr : data.data();
    // The driver checks the header (vendor, device, cache UUID) and ignores
    // data from another device or driver; one that refuses it outright gets
    // an empty cache instead.
    if (vkCreatePipelineCache(context.device(), &info, nullptr, &pipeline_cache_handle) !=
        VK_SUCCESS) {
      info.initialDataSize = 0;
      info.pInitialData = nullptr;
      if (vkCreatePipelineCache(context.device(), &info, nullptr, &pipeline_cache_handle) !=
          VK_SUCCESS)
        pipeline_cache_handle = VK_NULL_HANDLE;
    }
  }
  void close_pipeline_cache() noexcept {
    if (!pipeline_cache_handle || !context.device()) return;
    if (!pipeline_cache_path.empty()) {
      std::size_t size = 0;
      if (vkGetPipelineCacheData(context.device(), pipeline_cache_handle, &size, nullptr) ==
              VK_SUCCESS &&
          size) {
        std::vector<char> data(size);
        if (vkGetPipelineCacheData(context.device(), pipeline_cache_handle, &size, data.data()) ==
            VK_SUCCESS) {
          // Written beside, then renamed over: a crash mid-write leaves the
          // previous cache whole.
          std::error_code ignored;
          std::filesystem::create_directories(pipeline_cache_path.parent_path(), ignored);
          auto temporary = pipeline_cache_path;
          temporary += ".tmp";
          {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (file) file.write(data.data(), static_cast<std::streamsize>(size));
          }
          std::filesystem::rename(temporary, pipeline_cache_path, ignored);
        }
      }
    }
    vkDestroyPipelineCache(context.device(), pipeline_cache_handle, nullptr);
    pipeline_cache_handle = VK_NULL_HANDLE;
  }
  EdramColorTransfer edram_color_transfer{};
  EdramDepthTransfer edram_depth_transfer{};
  EdramCanonicalStore edram_canonical_store{};
  GpuResolver gpu_resolver{};

  bool gpu_color_resolve(RenderTargetImage& owner, const GpuResolvePlan& plan) {
    if (gpu_resolver.resolve(context.device(), queue, mirror, owner, plan.constants,
                             plan.range_address, plan.range_size))
      return true;
    error = named(gpu_resolver.error());
    return false;
  }
  bool gpu_color_clear(RenderTargetImage& owner, const GpuClearConstants& constants) {
    if (gpu_resolver.clear(context.device(), queue, owner, constants)) return true;
    error = named(gpu_resolver.error());
    return false;
  }
  bool gpu_depth_resolve(DepthTargetImage& owner, const GpuResolvePlan& plan) {
    if (gpu_resolver.resolve_depth(context.device(), queue, mirror, owner, plan.constants,
                                   plan.range_address, plan.range_size))
      return true;
    error = named(gpu_resolver.error());
    return false;
  }
  bool gpu_depth_clear(DepthTargetImage& owner, const GpuClearConstants& constants,
                       std::uint8_t stencil) {
    if (gpu_resolver.clear_depth(context.device(), queue, owner, constants, stencil)) return true;
    error = named(gpu_resolver.error());
    return false;
  }
  ScanoutRenderer scanout{};

  bool presentation_srgb_target() const { return presentation.srgb(); }
  PresentStatus present_scanout(const ScanoutConstants& constants) {
    const auto format = presentation.format();
    if (!scanout.prepare(context.device(), format, mirror)) {
      error = named(scanout.error());
      return PresentStatus::Error;
    }
    return presentation.present([&](VkCommandBuffer command, VkImageView target) {
      scanout.record(command, mirror, target, format, constants);
    });
  }
  bool capture_scanout_image(const ScanoutConstants& constants,
                             PreparedPresentationFrame& output) {
    if (scanout.capture(context.physical_device(), context.device(), queue, mirror, constants,
                        output))
      return true;
    error = named(scanout.error());
    return false;
  }

  bool transfer_color_tiles(RenderTargetImage& source, RenderTargetImage& destination,
                            const EdramTransferConstants& constants,
                            std::span<const EdramSurfaceRegion> regions) {
    if (edram_color_transfer.transfer(context.device(), queue, source, destination, constants,
                                      regions))
      return true;
    error = named(edram_color_transfer.error());
    return false;
  }
  bool transfer_depth_tiles(DepthTargetImage& source, DepthTargetImage& destination,
                            const EdramTransferConstants& constants,
                            std::span<const EdramSurfaceRegion> regions) {
    if (edram_depth_transfer.transfer(context.device(), context.physical_device(), queue,
                                      source, destination, constants, regions))
      return true;
    error = named(edram_depth_transfer.error());
    return false;
  }
  template <typename Source>
  bool convert_color_tiles(Source& source, RenderTargetImage& destination,
                           const EdramTransferConstants& constants,
                           std::span<const EdramSurfaceRegion> regions) {
    if (edram_color_transfer.convert(context.device(), queue, source, destination, constants,
                                     regions))
      return true;
    error = named(edram_color_transfer.error());
    return false;
  }
  template <typename Source>
  bool convert_depth_tiles(Source& source, DepthTargetImage& destination,
                           const EdramTransferConstants& constants,
                           std::span<const EdramSurfaceRegion> regions) {
    if (edram_depth_transfer.convert(context.device(), context.physical_device(), queue, source,
                                     destination, constants, regions))
      return true;
    error = named(edram_depth_transfer.error());
    return false;
  }

  // Canonical EDRAM loads on the GPU: `tiles` uploaded from the CPU store,
  // then unpacked into the image (BackendCore::canonical_to_color()).
  bool load_canonical_color(RenderTargetImage& image, const Edram& edram,
                            std::span<const std::uint16_t> tiles,
                            const EdramTransferConstants& constants,
                            std::span<const EdramSurfaceRegion> regions) {
    if (!edram_canonical_store.upload(context.physical_device(), context.device(), queue, edram,
                                      tiles)) {
      error = named(edram_canonical_store.error());
      return false;
    }
    if (edram_color_transfer.load_canonical(context.device(), queue,
                                            edram_canonical_store.source(), image, constants,
                                            regions))
      return true;
    error = named(edram_color_transfer.error());
    return false;
  }
  bool load_canonical_depth(DepthTargetImage& image, const Edram& edram,
                            std::span<const std::uint16_t> tiles,
                            const EdramTransferConstants& constants,
                            std::span<const EdramSurfaceRegion> regions) {
    if (!edram_canonical_store.upload(context.physical_device(), context.device(), queue, edram,
                                      tiles)) {
      error = named(edram_canonical_store.error());
      return false;
    }
    if (edram_depth_transfer.load_canonical(context.device(), context.physical_device(), queue,
                                            edram_canonical_store.source(), image, constants,
                                            regions))
      return true;
    error = named(edram_depth_transfer.error());
    return false;
  }

  bool create_render_target(RenderTargetImage& image,
                            const EdramSurfaceLayout& surface,
                            ColorRenderTargetFormat format) {
    return image.initialize(context.physical_device(), context.device(), queue,
                            surface, format);
  }
  // Dynamic rendering leaves unused color attachments undefined; see
  // BackendApi::color_format().
  bool fill_unbound_color_slots(const DrawResourceState&, std::uint32_t,
                                std::size_t, std::array<RenderTargetImage*, 4>&,
                                std::array<std::uint8_t, 4>&,
                                std::array<BlendState, 4>&) {
    return true;
  }
  bool create_depth_target(DepthTargetImage& image,
                           const EdramSurfaceLayout& surface,
                           DepthRenderTargetFormat format) {
    return image.initialize(context.physical_device(), context.device(),
                            surface, format);
  }
  GpuTextureLoader texture_loader{};
  // Loads `layout` into `image` on the GPU, from `capture` (a sync-point copy
  // of guest memory at `capture_base`) or, when that is empty, from the
  // guest-memory mirror. A new or differently shaped image is created first.
  bool load_texture(TextureImage& image, const TextureLayout& layout,
                    std::span<const std::byte> capture, std::uint32_t capture_base) {
    if (!image.matches(layout)) {
      const bool created = image.create(context.physical_device(), context.device(), layout);
      // Fold in regardless of overall success/failure: a texture can
      // successfully bind while its sampler still substituted an
      // unsupported clamp mode.
      unsupported.unsupported_sampler_behaviors += image.unsupported_sampler_behaviors();
      if (!created) {
        error = named(image.error());
        return false;
      }
    }
    const bool loaded =
        capture.empty()
            ? texture_loader.load_from_mirror(context.physical_device(), context.device(), queue,
                                              mirror, image, layout)
            : texture_loader.load_from_bytes(context.physical_device(), context.device(), queue,
                                             image, layout, capture, capture_base);
    if (!loaded) error = named(texture_loader.error());
    return loaded;
  }
  // A texture mod's texels, uploaded once and waited for (the first draw
  // that samples it waits one copy).
  bool create_replacement_texture(TextureImage& image, const DecodedTexture& texture) {
    return image.initialize(context.physical_device(), context.device(), queue, texture);
  }
  bool bind_texture(std::uint32_t slot, const TextureDescriptor& descriptor,
                    TextureImage& image) {
    return resources.bind_texture(slot, descriptor.dimension, image.view(),
                                  image.sampler());
  }
  VkPipelineLayout pipeline_root() { return resources.pipeline_layout(); }
  std::uint32_t color_render_width(const DrawResourceState& state,
                                   const RenderTargetImage*) {
    return std::uint32_t(state.raster.surface_pitch);
  }
  std::uint32_t color_render_height(const DrawResourceState& state,
                                    const RenderTargetImage*) {
    return std::uint32_t(state.raster.scissor_bottom);
  }
  void forget_backend_render_targets() {}
  bool initialize_mirror(memory::AddressSpace& guest_memory) {
    return mirror.initialize(context.physical_device(), context.device(), queue,
                             guest_memory);
  }
  bool bind_guest_memory() {
    return resources.bind_guest_memory(mirror.buffer(),
                                       memory::kPhysicalMemorySize);
  }
  std::uint64_t pipeline_cache_misses() const { return pipelines.size(); }
  void add_backend_unsupported_counters(GpuUnsupportedCounters&) const {}
  // GPU V3 draw recording (see the D3D12 backend, whose model this shares).
  // A draw is prepared here, in order, into a PreparedDraw that refers to
  // nothing mutable: its descriptor set and constants offset, its uploaded
  // indices, its attachments; image transitions and mirror barriers it needs
  // are recorded as ordered work before it. Consecutive draws on the same
  // attachments form a run, recorded as one rendering instance by
  // record_prepared_run() - rather than one rendering instance per draw,
  // each loading and storing every attachment - into the primary segment, or
  // on a recording worker when the run is long.
  struct PreparedDraw {
    VkPipeline pipeline{VK_NULL_HANDLE};
    VkPipelineLayout layout{VK_NULL_HANDLE};
    VkDescriptorSet set{VK_NULL_HANDLE};
    std::uint32_t constants_offset{};
    bool indexed{};
    VkBuffer index_buffer{VK_NULL_HANDLE};
    VkDeviceSize index_offset{};
    std::uint32_t element_count{};
    std::array<VkImageView, 4> colors{};
    std::uint32_t color_count{};
    VkImageView depth{VK_NULL_HANDLE};
    std::uint32_t width{};
    std::uint32_t height{};
    VkViewport viewport{};
    VkRect2D scissor{};
    std::array<float, 4> blend_constants{};
    bool stencil{};
    std::array<std::uint32_t, 3> stencil_front{};  // compare mask, write mask, reference
    std::array<std::uint32_t, 3> stencil_back{};

    [[nodiscard]] bool same_attachments(const PreparedDraw& other) const noexcept {
      return colors == other.colors && color_count == other.color_count &&
             depth == other.depth && width == other.width && height == other.height;
    }
  };
  std::vector<PreparedDraw> open_run{};
  std::uint32_t parallel_run_draws{8};

  bool record_draw(const DrawRecording& draw, bool& record_failed) {
    // Attachments not in their attachment layouts, and the guest-memory
    // mirror's barrier, are ordered work before the draw (each ends the open
    // run: no barrier is recorded inside a rendering instance).
    bool transition = draw.depth && draw.depth->layout() != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    for (std::size_t i = 0; i < draw.color_count; ++i)
      transition |= draw.targets[i] &&
                    draw.targets[i]->layout() != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const bool mirror_barrier = mirror.needs_shader_access_barrier(draw.memexport_writable);
    if (transition || mirror_barrier) {
      if (!queue.execute_async([&](VkCommandBuffer command, std::uint32_t, std::uint32_t) {
            if (mirror_barrier) mirror.prepare_shader_access(command, draw.memexport_writable);
            for (std::size_t i = 0; i < draw.color_count; ++i)
              if (draw.targets[i]) draw.targets[i]->transition_to_color_attachment(command);
            if (draw.depth) draw.depth->transition_to_depth_attachment(command);
          }))
        return false;
    }
    if (!mirror_barrier) mirror.note_shader_access(draw.memexport_writable);

    PreparedDraw prepared{};
    prepared.color_count = static_cast<std::uint32_t>(draw.color_count);
    for (std::size_t i = 0; i < draw.color_count; ++i)
      prepared.colors[i] = draw.targets[i] ? draw.targets[i]->view() : VK_NULL_HANDLE;
    prepared.depth = draw.depth ? draw.depth->view() : VK_NULL_HANDLE;
    prepared.width = draw.render_width;
    prepared.height = draw.render_height;
    // A run is one rendering instance: a draw on other attachments ends it.
    if (!open_run.empty() && !open_run.back().same_attachments(prepared)) close_draw_run();

    std::uint32_t frame_index = 0;
    std::uint32_t draw_slot = 0;
    if (!queue.reserve_slot(frame_index, draw_slot)) return false;
    const auto binding = resources.prepare_draw(frame_index, draw_slot);
    std::size_t index_offset = 0;
    bool resident_index = false;
    if (!upload_draw_indices(draw, frame_index, index_offset, resident_index)) {
      record_failed = true;
      return true;
    }
    prepared.pipeline = draw.pipeline.pipeline();
    prepared.layout = resources.pipeline_layout();
    prepared.set = binding.set;
    prepared.constants_offset = binding.constants_offset;
    prepared.indexed = draw.batch.indexed;
    if (draw.batch.indexed) {
      prepared.index_buffer = resident_index ? resident_indices.buffer()
                                             : transient_uploads[frame_index].buffer();
      prepared.index_offset = static_cast<VkDeviceSize>(index_offset);
      prepared.element_count = static_cast<std::uint32_t>(draw.batch.indices.size());
    } else {
      prepared.element_count = draw.batch.vertex_count;
    }
    // The guest X/Y viewport transform runs in the vertex shader; the host
    // viewport is the whole render target, flipped so NDC Y points up as
    // in Direct3D. Its depth range is the host depth transform's, which
    // the vertex shader's Z part keeps ordered.
    // A D24FS8 target holds half the guest depth on the host (see
    // host_viewport_depth), so its draws use the halved range too.
    const auto depth_format = draw.depth && draw.depth->requires_float24_conversion()
                                  ? DepthRenderTargetFormat::D24FS8
                                  : DepthRenderTargetFormat::D24S8;
    prepared.viewport = {0.0f, float(draw.render_height), float(draw.render_width),
                         -float(draw.render_height),
                         host_viewport_depth(depth_format, draw.z_min),
                         host_viewport_depth(depth_format, draw.z_max)};
    prepared.scissor = {{draw.scissor_left, draw.scissor_top},
                        {static_cast<std::uint32_t>(draw.scissor_right - draw.scissor_left),
                         static_cast<std::uint32_t>(draw.scissor_bottom - draw.scissor_top)}};
    std::copy(draw.blend_constant.begin(), draw.blend_constant.end(),
              prepared.blend_constants.begin());
    const auto& depth_state = draw.state.depth_target;
    prepared.stencil = draw.depth && depth_state.stencil_enabled;
    prepared.stencil_front = {depth_state.stencil_read_mask, depth_state.stencil_write_mask,
                              depth_state.stencil_reference};
    prepared.stencil_back =
        depth_state.backface_stencil_enabled
            ? std::array<std::uint32_t, 3>{depth_state.stencil_back_read_mask,
                                           depth_state.stencil_back_write_mask,
                                           depth_state.stencil_back_reference}
            : prepared.stencil_front;
    open_run.push_back(prepared);
    return true;
  }

  // Closes the open run (CommandQueue's before-ordered-work hook).
  void close_draw_run() {
    if (open_run.empty()) return;
    auto run = std::move(open_run);
    open_run.clear();
    const bool parallel = run.size() >= parallel_run_draws;
    if (!queue.record_run(
            [run = std::move(run)](VkCommandBuffer command) { record_prepared_run(command, run); },
            parallel) &&
        error.empty())
      error = named(queue.error());
  }

  // Records `run` - draws on the same attachments - as one rendering
  // instance: API calls only, from the prepared values. Safe on any thread.
  static void record_prepared_run(VkCommandBuffer command, const std::vector<PreparedDraw>& run) {
    const auto& first = run.front();
    // Earlier rendering instances (in this command buffer or one submitted
    // before it) may have written these attachments: their loads here wait
    // for those writes.
    VkMemoryBarrier2 attachments{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    attachments.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    attachments.srcAccessMask =
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    attachments.dstStageMask = attachments.srcStageMask;
    attachments.dstAccessMask =
        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &attachments;
    vkCmdPipelineBarrier2(command, &dependency);

    std::array<VkRenderingAttachmentInfo, 4> colors{};
    for (std::uint32_t i = 0; i < first.color_count; ++i) {
      colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      if (first.colors[i]) {
        colors[i].imageView = first.colors[i];
        colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      } else {
        colors[i].imageView = VK_NULL_HANDLE;
        colors[i].imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colors[i].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      }
    }
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {first.width, first.height};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = first.color_count;
    rendering.pColorAttachments = first.color_count ? colors.data() : nullptr;
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (first.depth) {
      depth.imageView = first.depth;
      depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      rendering.pDepthAttachment = &depth;
      rendering.pStencilAttachment = &depth;
    }
    vkCmdBeginRendering(command, &rendering);
    VkPipeline bound_pipeline = VK_NULL_HANDLE;
    for (const auto& draw : run) {
      if (bound_pipeline != draw.pipeline) {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline);
        bound_pipeline = draw.pipeline;
      }
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.layout, 0, 1,
                              &draw.set, 1, &draw.constants_offset);
      vkCmdSetViewport(command, 0, 1, &draw.viewport);
      vkCmdSetScissor(command, 0, 1, &draw.scissor);
      vkCmdSetBlendConstants(command, draw.blend_constants.data());
      if (draw.stencil) {
        vkCmdSetStencilCompareMask(command, VK_STENCIL_FACE_FRONT_BIT, draw.stencil_front[0]);
        vkCmdSetStencilWriteMask(command, VK_STENCIL_FACE_FRONT_BIT, draw.stencil_front[1]);
        vkCmdSetStencilReference(command, VK_STENCIL_FACE_FRONT_BIT, draw.stencil_front[2]);
        vkCmdSetStencilCompareMask(command, VK_STENCIL_FACE_BACK_BIT, draw.stencil_back[0]);
        vkCmdSetStencilWriteMask(command, VK_STENCIL_FACE_BACK_BIT, draw.stencil_back[1]);
        vkCmdSetStencilReference(command, VK_STENCIL_FACE_BACK_BIT, draw.stencil_back[2]);
      }
      if (draw.indexed) {
        vkCmdBindIndexBuffer(command, draw.index_buffer, draw.index_offset, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(command, draw.element_count, 1, 0, 0, 0);
      } else {
        vkCmdDraw(command, draw.element_count, 1, 0, 0);
      }
    }
    vkCmdEndRendering(command);
  }
};

Backend::Backend() : impl_(std::make_unique<Impl>()) {}
Backend::~Backend() = default;
bool Backend::initialize(const ContextConfig& config) {
  impl_ = std::make_unique<Impl>();
  if (!impl_->context.initialize(config)) {
    impl_->error = impl_->context.error();
    return false;
  }
  if (!impl_->queue.initialize(impl_->context.device(),
                               impl_->context.graphics_queue(),
                               impl_->context.graphics_queue_family())) {
    impl_->error = impl_->queue.error();
    return false;
  }
  if (!impl_->resources.initialize(impl_->context.physical_device(),
                                   impl_->context.device())) {
    impl_->error = impl_->resources.error();
    return false;
  }
  // Guest shaders bind guest memory the way the device's descriptors allow.
  impl_->guest_segments = impl_->resources.segmentation();
  if (impl_->guest_segments.segmented())
    logging::Logger::instance().log(
        logging::Level::Info, "gpu",
        "Vulkan storage buffers span less than guest memory: bound as " +
            std::to_string(impl_->guest_segments.count) + " segments of " +
            std::to_string(impl_->guest_segments.segment_bytes >> 20u) + " MiB");
  // Ordered work and submissions close the open draw run first.
  impl_->queue.set_before_ordered_work([impl = impl_.get()] { impl->close_draw_run(); });
  // GPU timestamps around every batch, where the queue supports them.
  static_cast<void>(impl_->queue.enable_timestamps(impl_->context.physical_device()));
  impl_->open_pipeline_cache({});
  for (std::uint32_t i = 0; i < CommandQueue::kFrameCount; ++i) {
    if (!impl_->transient_uploads[i].initialize(
            impl_->context.physical_device(), impl_->context.device(),
            Impl::kTransientUploadBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
        !impl_->transient_uploads[i].map(impl_->transient_upload_mappings[i])) {
      impl_->error = impl_->transient_uploads[i].error();
      return false;
    }
  }
  if (!impl_->resident_indices.initialize(
          impl_->context.physical_device(), impl_->context.device(), Impl::kResidentIndexBytes,
          VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
      !impl_->resident_indices.map(impl_->resident_index_mapping)) {
    impl_->error = impl_->resident_indices.error();
    return false;
  }
  impl_->attach_resident_indices();
  // Before any guest depth is written: every D24 write path then stores the
  // guest's exact codes on this device.
  if (!impl_->calibrate_depth_writes()) return false;
  impl_->ready = true;
  return true;
}
VkInstance Backend::instance() const noexcept {
  return impl_ ? impl_->context.instance() : VK_NULL_HANDLE;
}

std::optional<Unorm24DepthCalibration> Backend::unorm24_depth_calibration() const {
  return impl_ ? impl_->unorm24_depth_calibration : std::nullopt;
}

bool Backend::configure_presentation(VkSurfaceKHR surface,
                                     const PresentationConfig& config,
                                     bool take_surface_ownership) {
  if (!impl_->ready) return false;
  if (!impl_->presentation.initialize(impl_->context, impl_->queue, surface,
                                      config, take_surface_ownership)) {
    impl_->error = impl_->presentation.error();
    return false;
  }
  return true;
}

void Backend::begin_submission(memory::AddressSpace& memory, Edram& edram) {
  impl_->begin_submission(memory, edram);
}
void Backend::consume(const ir::Command& command) {
  impl_->consume(command);
}
void Backend::end_submission() { impl_->end_submission(); }
void Backend::end_submission_deferred() { impl_->end_submission(false); }
bool Backend::make_guest_memory_cpu_visible(std::uint32_t physical_address,
                                            std::uint32_t size) {
  return impl_->make_guest_memory_cpu_visible(physical_address, size);
}

bool Backend::make_edram_canonical() {
  if (!impl_->ready || !impl_->edram) return false;
  if (!impl_->make_edram_canonical()) return false;
  return true;
}

bool Backend::invalidate_edram_native_state() {
  return impl_->invalidate_edram_native_state();
}

void Backend::sync_guest_reads() { impl_->sync_guest_reads(); }
void Backend::prefetch_shaders(std::span<const ir::Command> commands) {
  if (impl_) impl_->prefetch_shaders(commands);
}
void Backend::end_guest_read_sync() { impl_->end_guest_read_sync(); }
bool Backend::publish_gpu_writes(bool wait) { return impl_->publish_gpu_writes(wait); }
void Backend::set_resolve_readback(ResolveReadback policy) { impl_->resolve_readback = policy; }
bool Backend::set_parallel_recording(unsigned threads, std::uint32_t min_run_draws) {
  if (!impl_ || !impl_->queue.set_recording_threads(threads)) return false;
  impl_->parallel_run_draws = (std::max)(1u, min_run_draws);
  return true;
}

std::uint64_t Backend::parallel_runs_recorded() const noexcept {
  return impl_ ? impl_->queue.parallel_runs() : 0u;
}

Backend::GpuTiming Backend::gpu_timing() {
  if (!impl_ || !impl_->queue.wait_idle()) return {};
  impl_->queue.collect_timestamps();
  return {impl_->queue.timed_batches(), impl_->queue.timed_gpu_ns()};
}

Backend::ShaderPrefetchStatistics Backend::shader_prefetch_statistics() const noexcept {
  if (!impl_) return {};
  return {impl_->prefetched_variants, impl_->prefetched_variant_hits,
          impl_->variants_compiled_at_draw};
}

void Backend::set_shader_cache_directory(const std::string& directory) {
  impl_->shader_cache.set_disk_directory(directory);
  if (impl_->ready)
    impl_->open_pipeline_cache(directory.empty()
                                   ? std::filesystem::path{}
                                   : std::filesystem::path(directory) / "vulkan_pipelines.bin");
}

bool Backend::capture_scanout(const PresentationFrame& frame, std::uint32_t width,
                              std::uint32_t height, PreparedPresentationFrame& output) {
  return impl_->capture_scanout(frame, width, height, output);
}

PresentStatus Backend::present(const PresentationFrame& frame) {
  return impl_->present(frame);
}

bool Backend::resize_presentation(std::uint32_t width, std::uint32_t height) {
  return impl_->resize_presentation(width, height);
}

bool Backend::presentation_ready() const noexcept {
  return impl_->presentation.ready();
}
GpuPerformanceCounters Backend::performance_counters() const noexcept {
  return impl_->performance_counters();
}

std::vector<GpuFailureReason> Backend::failure_reasons() const {
  return impl_->failure_log.reasons();
}

std::vector<GpuResolveKind> Backend::resolve_census() const {
  return impl_->resolve_census.kinds();
}

GpuUnsupportedCounters Backend::unsupported_counters() const noexcept {
  return impl_->unsupported_counters();
}

GpuShaderCoverage Backend::shader_coverage() const noexcept {
  return impl_->shader_coverage();
}
bool Backend::ready() const noexcept { return impl_->ready; }
std::size_t Backend::command_count() const noexcept { return impl_->command_count; }
std::size_t Backend::draw_count() const noexcept { return impl_->draw_count; }
std::size_t Backend::compiled_shader_count() const noexcept {
  return impl_->compiled_shader_count;
}
std::size_t Backend::pipeline_state_count() const noexcept {
  return impl_->pipeline_states.size();
}
bool Backend::resource_layout_ready() const noexcept {
  return impl_->resources.ready();
}
std::size_t Backend::realized_texture_count() const noexcept {
  return impl_->textures.size();
}
std::size_t Backend::realized_render_target_count() const noexcept {
  return impl_->render_targets.size();
}
std::uint32_t Backend::tallest_render_target_rows() const noexcept {
  std::uint32_t rows = 0;
  for (const auto& [key, image] : impl_->render_targets)
    if (image) rows = (std::max)(rows, image->height());
  return rows;
}
const DeviceProperties& Backend::device_properties() const noexcept {
  return impl_->context.properties();
}
const std::string& Backend::error() const noexcept { return impl_->error; }

}  // namespace xenon::gpu::vulkan

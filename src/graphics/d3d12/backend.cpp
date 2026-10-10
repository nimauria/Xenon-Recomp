#include "xenon/gpu/d3d12/backend.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <deque>
#include <variant>
#include <span>
#include <unordered_set>
#include <unordered_map>

#include "xenon/gpu/d3d12/command_queue.hpp"
#include "xenon/gpu/d3d12/depth_target.hpp"
#include "xenon/gpu/d3d12/guest_memory_mirror.hpp"
#include "xenon/gpu/d3d12/pipeline.hpp"
#include "xenon/gpu/d3d12/presentation.hpp"
#include "xenon/gpu/d3d12/resource_layout.hpp"
#include "xenon/gpu/d3d12/render_target.hpp"
#include "xenon/gpu/d3d12/texture.hpp"
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

namespace xenon::gpu::d3d12 {

struct BackendApi {
  static constexpr std::string_view kName = "D3D12";
  using Context = d3d12::Context;
  using CommandQueue = d3d12::CommandQueue;
  using PresentationSwapchain = d3d12::PresentationSwapchain;
  using GuestMemoryMirror = d3d12::GuestMemoryMirror;
  using ResourceLayout = d3d12::ResourceLayout;
  using Buffer = d3d12::Buffer;
  using TextureImage = d3d12::TextureImage;
  using RenderTargetImage = d3d12::RenderTargetImage;
  using DepthTargetImage = d3d12::DepthTargetImage;
  using GraphicsPipeline = d3d12::GraphicsPipeline;
  using Format = DXGI_FORMAT;
  static constexpr DXGI_FORMAT kUndefinedFormat = DXGI_FORMAT_UNKNOWN;
  static DXGI_FORMAT native_color_format(ColorRenderTargetFormat format) {
    return color_render_target_format(format);
  }
  // Every color slot below the attachment count is bound, real or dummy; see
  // Backend::Impl::fill_unbound_color_slots().
  static DXGI_FORMAT color_format(const d3d12::RenderTargetImage* target) {
    return target->format();
  }
  // D3D12 has no FRONT_AND_BACK cull mode.
  static constexpr bool kEmulatesFrontAndBackCull = true;
#ifdef XENON_HAS_DXC
  static constexpr ShaderBinaryFormat kShaderBinaryFormat =
      ShaderBinaryFormat::Dxil;
  static constexpr std::string_view kShaderBinaryName = "DXIL";
#endif
};

class Backend::Impl : public detail::BackendCore<Backend::Impl, BackendApi> {
 public:
  ~Impl() {
    // Native draws may still be using render targets, textures, descriptor
    // snapshots and upload arenas owned by this Impl. Drain them before member
    // destruction starts.
    (void)queue.wait_idle();
  }
  std::unordered_map<std::uint64_t, std::unique_ptr<RenderTargetImage>> dummy_render_targets{};

  bool create_render_target(RenderTargetImage& image,
                            const EdramSurfaceLayout& surface,
                            ColorRenderTargetFormat format) {
    return image.initialize(context.device(), surface, format);
  }
  bool fill_unbound_color_slots(const DrawResourceState& state,
                                std::uint32_t height, std::size_t color_count,
                                std::array<RenderTargetImage*, 4>& active_targets,
                                std::array<std::uint8_t, 4>& active_write_masks,
                                std::array<BlendState, 4>& active_blends) {
    // D3D12 does not expose Vulkan-style undefined dynamic-rendering slots.
    // Preserve sparse Xenos export locations by binding per-slot throwaway
    // RTVs with a zero write mask. This keeps SV_TargetN attached to native
    // RT slot N without compacting or aliasing a real EDRAM surface.
    for (std::size_t slot = 0; slot < color_count; ++slot) {
      if (active_targets[slot]) continue;
      EdramSurfaceLayout dummy_surface{
          0, state.raster.surface_pitch, height,
          static_cast<MsaaSamples>(state.raster.msaa_samples_log2), false,
          false};
      const auto key = dummy_surface.hash() ^ 0xD00D000000000000ull ^
                       (std::uint64_t(slot) << 56u);
      if (!dummy_render_targets.contains(key)) {
        auto image = std::make_unique<RenderTargetImage>();
        if (!image->initialize(context.device(), dummy_surface,
                               ColorRenderTargetFormat::R8G8B8A8)) {
          error = image->error();
          return false;
        }
        dummy_render_targets.emplace(key, std::move(image));
      }
      active_targets[slot] = dummy_render_targets.at(key).get();
      active_write_masks[slot] = 0;
      active_blends[slot] = {};
    }
    return true;
  }
  bool create_depth_target(DepthTargetImage& image,
                           const EdramSurfaceLayout& surface,
                           DepthRenderTargetFormat format) {
    return image.initialize(context.device(), surface, format);
  }
  bool create_texture(TextureImage& image, const DecodedTexture& decoded) {
    return image.initialize(context.device(), queue, decoded);
  }
  bool bind_texture(std::uint32_t slot, const TextureDescriptor& descriptor,
                    TextureImage& image) {
    return resources.bind_texture(slot, descriptor.dimension, image.resource(),
                                  image.format(), descriptor.mip_max_level + 1u,
                                  descriptor);
  }
  ID3D12RootSignature* pipeline_root() { return resources.root_signature(); }
  std::uint32_t color_render_width(const DrawResourceState&,
                                   const RenderTargetImage* target) {
    return target->width();
  }
  std::uint32_t color_render_height(const DrawResourceState&,
                                    const RenderTargetImage* target) {
    return target->height();
  }
  void forget_backend_render_targets() { dummy_render_targets.clear(); }
  bool initialize_mirror(memory::AddressSpace& guest_memory) {
    return mirror.initialize(context.device(), queue, guest_memory);
  }
  bool bind_guest_memory() {
    return resources.bind_guest_memory(mirror.resource(),
                                       memory::kPhysicalMemorySize);
  }
  std::uint64_t pipeline_cache_misses() const { return pipeline_states.size(); }
  void add_backend_unsupported_counters(GpuUnsupportedCounters& counters) const {
    counters.unsupported_sampler_behaviors +=
        resources.unsupported_sampler_behaviors();
  }
  bool record_draw(const DrawRecording& draw, bool& record_failed) {
    return queue.execute_async([&](ID3D12GraphicsCommandList* list,
                                   std::uint32_t frame_index,
                                   std::uint32_t draw_slot) {
      resources.prepare_draw(frame_index, draw_slot);
      D3D12_INDEX_BUFFER_VIEW index_view{};
      std::size_t index_offset = 0;
      if (!upload_draw_indices(draw, frame_index, draw_slot, index_offset)) {
        record_failed = true;
        return;
      }
      if (draw.batch.indexed) {
        index_view.BufferLocation =
            transient_uploads[frame_index].resource()->GetGPUVirtualAddress() +
            index_offset;
        index_view.SizeInBytes = static_cast<UINT>(draw.index_byte_size);
        index_view.Format = DXGI_FORMAT_R32_UINT;
      }
      mirror.prepare_shader_access(list, draw.memexport_writable);
      list->SetPipelineState(draw.pipeline.pipeline());
      list->SetGraphicsRootSignature(resources.root_signature());
      ID3D12DescriptorHeap* heaps[]{
          resources.resource_heap(frame_index, draw_slot),
          resources.sampler_heap(frame_index, draw_slot)};
      list->SetDescriptorHeaps(2, heaps);
      list->SetGraphicsRootConstantBufferView(
          0, resources.constants_resource(frame_index, draw_slot)->GetGPUVirtualAddress());
      list->SetGraphicsRootDescriptorTable(
          1, resources.resource_heap(frame_index, draw_slot)
                 ->GetGPUDescriptorHandleForHeapStart());
      list->SetGraphicsRootDescriptorTable(
          2, resources.sampler_heap(frame_index, draw_slot)
                 ->GetGPUDescriptorHandleForHeapStart());
      list->SetGraphicsRootDescriptorTable(
          3, resources.memory_export_handle(frame_index, draw_slot));
      list->OMSetBlendFactor(draw.state.blend_constant.data());
      const auto& guest_viewport = draw.state.raster.viewport;
      const auto x_scale = std::abs(guest_viewport.x_scale);
      const auto y_scale = std::abs(guest_viewport.y_scale);
      const bool has_viewport = x_scale > 0.0001f && y_scale > 0.0001f;
      const auto depth_a = (std::clamp)(guest_viewport.z_offset, 0.0f, 1.0f);
      const auto depth_b = (std::clamp)(
          guest_viewport.z_offset + guest_viewport.z_scale, 0.0f, 1.0f);
      const D3D12_VIEWPORT viewport = has_viewport
          ? D3D12_VIEWPORT{guest_viewport.x_offset - x_scale,
                           guest_viewport.y_offset - y_scale,
                           x_scale * 2.0f, y_scale * 2.0f,
                           (std::min)(depth_a, depth_b),
                           (std::max)(depth_a, depth_b)}
          : D3D12_VIEWPORT{0.0f, 0.0f, float(draw.render_width),
                           float(draw.render_height), 0.0f, 1.0f};
      const D3D12_RECT scissor{draw.scissor_left, draw.scissor_top,
                               draw.scissor_right, draw.scissor_bottom};
      list->RSSetViewports(1, &viewport);
      list->RSSetScissorRects(1, &scissor);
      std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 4> rtvs{};
      for (std::size_t i = 0; i < draw.color_count; ++i)
        rtvs[i] = draw.targets[i]->rtv();
      const auto dsv = draw.depth ? draw.depth->dsv()
                                    : D3D12_CPU_DESCRIPTOR_HANDLE{};
      list->OMSetRenderTargets(static_cast<UINT>(draw.color_count),
                               draw.color_count ? rtvs.data() : nullptr, FALSE,
                               draw.depth ? &dsv : nullptr);
      if (draw.depth && draw.state.depth_target.stencil_enabled) {
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList8> list8;
        if (draw.state.depth_target.backface_stencil_enabled &&
            SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&list8)))) {
          list8->OMSetFrontAndBackStencilRef(
              draw.state.depth_target.stencil_reference,
              draw.state.depth_target.stencil_back_reference);
        } else {
          list->OMSetStencilRef(draw.state.depth_target.stencil_reference);
        }
      }
      list->IASetPrimitiveTopology(draw.pipeline.native_topology());
      if (draw.batch.indexed) {
        list->IASetIndexBuffer(&index_view);
        list->DrawIndexedInstanced(
            static_cast<UINT>(draw.batch.indices.size()), 1, 0, 0, 0);
      } else {
        list->DrawInstanced(draw.batch.vertex_count, 1, 0, 0);
      }
    });
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
  if (!impl_->queue.initialize(impl_->context.device())) {
    impl_->error = impl_->queue.error();
    return false;
  }
  if (!impl_->resources.initialize(impl_->context.device())) {
    impl_->error = impl_->resources.error();
    return false;
  }
  for (std::uint32_t i = 0; i < CommandQueue::kFrameCount; ++i) {
    if (!impl_->transient_uploads[i].initialize(
            impl_->context.device(), Impl::kTransientUploadBytes,
            D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ) ||
        !impl_->transient_uploads[i].map(impl_->transient_upload_mappings[i])) {
      impl_->error = impl_->transient_uploads[i].error();
      return false;
    }
  }
  impl_->ready = true;
  return true;
}

bool Backend::configure_presentation(void* native_window,
                                     const PresentationConfig& config) {
  if (!impl_->ready) return false;
  if (!impl_->presentation.initialize(impl_->context, impl_->queue,
                                      native_window, config)) {
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
const DeviceProperties& Backend::device_properties() const noexcept {
  return impl_->context.properties();
}
const std::string& Backend::error() const noexcept { return impl_->error; }

}  // namespace xenon::gpu::d3d12

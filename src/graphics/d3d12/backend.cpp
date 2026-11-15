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
#include "xenon/gpu/depth_format.hpp"
#include "xenon/gpu/d3d12/edram_transfer.hpp"
#include "xenon/gpu/d3d12/gpu_resolve.hpp"
#include "xenon/gpu/d3d12/guest_memory_mirror.hpp"
#include "xenon/gpu/d3d12/pipeline.hpp"
#include "xenon/gpu/d3d12/presentation.hpp"
#include "xenon/gpu/d3d12/resource_layout.hpp"
#include "xenon/gpu/d3d12/scanout.hpp"
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
  // BLEND_FACTOR reads the blend constant's RGB; guest CONSTANT_ALPHA needs
  // its alpha (shared_blend_factor_constant).
  static constexpr bool kSharedBlendFactorConstant = true;
  static constexpr bool kSupportsDirectColorTransfer = true;
  static constexpr bool kSupportsDirectDepthTransfer = true;
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
    if (pipeline_library) pipeline_library->save();
  }

  // Persistent pipeline library (GPU V3): pipelines this device built in
  // earlier sessions load from their stored driver state instead of being
  // compiled again. Opened beside the shader cache once it has a directory
  // (set_shader_cache_directory()); saved when the backend is destroyed or
  // the directory changes. Each pipeline created through it holds it.
  std::shared_ptr<PipelineLibrary> pipeline_library{};
  [[nodiscard]] std::shared_ptr<PipelineLibrary> native_pipeline_cache() const {
    return pipeline_library;
  }
  void open_pipeline_library(const std::string& directory) {
    if (pipeline_library) pipeline_library->save();
    pipeline_library.reset();
    if (directory.empty() || !ready) return;
    pipeline_library =
        PipelineLibrary::open(context.device(),
                              std::filesystem::path(directory) / "d3d12_pipelines.bin",
                              resources.root_signature_hash());
  }
  std::unordered_map<std::uint64_t, std::unique_ptr<RenderTargetImage>> dummy_render_targets{};
  EdramColorTransfer edram_color_transfer{};
  EdramDepthTransfer edram_depth_transfer{};
  GpuResolver gpu_resolver{};

  bool gpu_color_resolve(RenderTargetImage& owner, const GpuResolveConstants& constants) {
    if (gpu_resolver.resolve(context.device(), queue, mirror, owner, constants)) return true;
    error = named(gpu_resolver.error());
    return false;
  }
  bool gpu_color_clear(RenderTargetImage& owner, const GpuClearConstants& constants) {
    if (gpu_resolver.clear(context.device(), queue, owner, constants)) return true;
    error = named(gpu_resolver.error());
    return false;
  }
  bool gpu_depth_resolve(DepthTargetImage& owner, const GpuResolveConstants& constants) {
    if (gpu_resolver.resolve_depth(context.device(), queue, mirror, owner, constants)) return true;
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

  bool presentation_srgb_target() const { return false; }
  PresentStatus present_scanout(const ScanoutConstants& constants) {
    constexpr auto format = PresentationSwapchain::format();
    if (!scanout.prepare(context.device(), format)) {
      error = named(scanout.error());
      return PresentStatus::Error;
    }
    return presentation.present(
        [&](ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE target) {
          scanout.record(list, mirror, target, format, constants);
        });
  }
  bool capture_scanout_image(const ScanoutConstants& constants,
                             PreparedPresentationFrame& output) {
    if (scanout.capture(context.device(), queue, mirror, constants, output)) return true;
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
    if (edram_depth_transfer.transfer(context.device(), queue, source, destination, constants,
                                      regions))
      return true;
    error = named(edram_depth_transfer.error());
    return false;
  }
  template <typename Source>
  bool convert_color_tiles(Source& source, RenderTargetImage& destination,
                           const EdramTransferConstants& constants,
                           std::span<const EdramSurfaceRegion> regions) {
    std::string source_error;
    const auto view = edram_transfer_source(source, source_error);
    if (!view.heap) {
      error = named(source_error);
      return false;
    }
    if (edram_color_transfer.convert(context.device(), queue, view, destination, constants,
                                     regions))
      return true;
    error = named(edram_color_transfer.error());
    return false;
  }
  template <typename Source>
  bool convert_depth_tiles(Source& source, DepthTargetImage& destination,
                           const EdramTransferConstants& constants,
                           std::span<const EdramSurfaceRegion> regions) {
    std::string source_error;
    const auto view = edram_transfer_source(source, source_error);
    if (!view.heap) {
      error = named(source_error);
      return false;
    }
    if (edram_depth_transfer.convert(context.device(), queue, view, destination, constants,
                                     regions))
      return true;
    error = named(edram_depth_transfer.error());
    return false;
  }

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
  GpuTextureLoader texture_loader{};
  // Loads `layout` into `image` on the GPU, from `capture` (a sync-point copy
  // of guest memory at `capture_base`) or, when that is empty, from the
  // guest-memory mirror. A new or differently shaped image is created first.
  bool load_texture(TextureImage& image, const TextureLayout& layout,
                    std::span<const std::byte> capture, std::uint32_t capture_base) {
    if (!image.matches(layout) && !image.create(context.device(), layout)) {
      error = named(image.error());
      return false;
    }
    const bool loaded =
        capture.empty()
            ? texture_loader.load_from_mirror(context.device(), queue, mirror, image, layout)
            : texture_loader.load_from_bytes(context.device(), queue, image, layout, capture,
                                             capture_base);
    if (!loaded) error = named(texture_loader.error());
    return loaded;
  }
  // A texture mod's texels, uploaded once and waited for (the first draw
  // that samples it waits one copy).
  bool create_replacement_texture(TextureImage& image, const DecodedTexture& texture) {
    return image.initialize(context.device(), queue, texture);
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
  // Diagnostic (the GPU trace's draw probe): the next recorded draw is wrapped
  // in a pipeline statistics query, so a draw that leaves its targets
  // untouched shows whether its vertices ran, its primitives survived
  // clipping and its pixels were shaded.
  void request_draw_statistics() {
    if (!statistics_heap) {
      D3D12_QUERY_HEAP_DESC heap_desc{};
      heap_desc.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
      heap_desc.Count = 1;
      const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
      D3D12_RESOURCE_DESC buffer{};
      buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      buffer.Width = sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
      buffer.Height = 1;
      buffer.DepthOrArraySize = 1;
      buffer.MipLevels = 1;
      buffer.SampleDesc.Count = 1;
      buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      if (FAILED(context.device()->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&statistics_heap))) ||
          FAILED(context.device()->CreateCommittedResource(
              &readback_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
              nullptr, IID_PPV_ARGS(&statistics_readback)))) {
        statistics_heap.Reset();
        statistics_readback.Reset();
        return;
      }
    }
    statistics_requested = true;
  }
  [[nodiscard]] std::string draw_statistics() {
    if (!statistics_recorded) return " statistics unavailable";
    statistics_recorded = false;
    if (!queue.wait_idle()) return " statistics: GPU wait failed";
    D3D12_QUERY_DATA_PIPELINE_STATISTICS data{};
    void* mapped = nullptr;
    const D3D12_RANGE range{0, sizeof(data)};
    if (FAILED(statistics_readback->Map(0, &range, &mapped))) return " statistics: map failed";
    std::memcpy(&data, mapped, sizeof(data));
    const D3D12_RANGE none{0, 0};
    statistics_readback->Unmap(0, &none);
    return " statistics: IA vertices " + std::to_string(data.IAVertices) + ", IA primitives " +
           std::to_string(data.IAPrimitives) + ", VS invocations " +
           std::to_string(data.VSInvocations) + ", GS primitives " +
           std::to_string(data.GSPrimitives) + ", clipper in " +
           std::to_string(data.CInvocations) + " out " + std::to_string(data.CPrimitives) +
           ", PS invocations " + std::to_string(data.PSInvocations);
  }
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> statistics_heap;
  Microsoft::WRL::ComPtr<ID3D12Resource> statistics_readback;
  bool statistics_requested{};
  bool statistics_recorded{};

  // GPU V3 draw recording. A draw is prepared here, in order: its slot's
  // descriptor tables and constants are snapshotted, its indices uploaded,
  // any barrier it needs recorded as ordered work - every decision made and
  // every value copied into a PreparedDraw that refers to nothing mutable.
  // Consecutive prepared draws form a run, recorded by record_prepared_run()
  // (a pure function of the run) when ordered work or a submission closes it:
  // here into the primary segment when it is short, or on a recording worker
  // into a segment of its own (CommandQueue::record_run()).
  struct PreparedDraw {
    ID3D12PipelineState* pipeline{};
    ID3D12RootSignature* root_signature{};
    D3D12_PRIMITIVE_TOPOLOGY topology{D3D_PRIMITIVE_TOPOLOGY_UNDEFINED};
    ID3D12DescriptorHeap* resource_heap{};
    ID3D12DescriptorHeap* sampler_heap{};
    ResourceLayout::DrawTables tables{};
    bool indexed{};
    D3D12_INDEX_BUFFER_VIEW index_view{};
    UINT element_count{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 4> rtvs{};
    UINT color_count{};
    bool has_depth{};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissor{};
    std::array<float, 4> blend_factor{};
    bool stencil{};
    bool separate_back_stencil{};
    UINT stencil_reference{};
    UINT stencil_back_reference{};
    bool statistics{};
  };
  std::vector<PreparedDraw> open_run{};
  // Runs at least this long are recorded on a worker (shorter ones cost more
  // to hand over than to record here).
  std::uint32_t parallel_run_draws{8};

  bool record_draw(const DrawRecording& draw, bool& record_failed) {
    // The guest-memory mirror's barrier, when this draw needs one, is ordered
    // work: it ends the open run and goes before the draw.
    if (mirror.needs_shader_access_barrier(draw.memexport_writable) &&
        !queue.execute_async([&](ID3D12GraphicsCommandList* list, std::uint32_t, std::uint32_t) {
          mirror.prepare_shader_access(list, draw.memexport_writable);
        }))
      return false;
    std::uint32_t frame_index = 0;
    std::uint32_t draw_slot = 0;
    if (!queue.reserve_slot(frame_index, draw_slot)) return false;
    PreparedDraw prepared{};
    prepared.tables = resources.prepare_draw(frame_index, draw_slot);
    std::size_t index_offset = 0;
    if (!upload_draw_indices(draw, frame_index, index_offset)) {
      record_failed = true;
      return true;
    }
    prepared.pipeline = draw.pipeline.pipeline();
    prepared.root_signature = resources.root_signature();
    prepared.topology = draw.pipeline.native_topology();
    prepared.resource_heap = resources.resource_heap(frame_index);
    prepared.sampler_heap = resources.sampler_heap(frame_index);
    prepared.indexed = draw.batch.indexed;
    if (draw.batch.indexed) {
      prepared.index_view.BufferLocation =
          transient_uploads[frame_index].resource()->GetGPUVirtualAddress() + index_offset;
      prepared.index_view.SizeInBytes = static_cast<UINT>(draw.index_byte_size);
      prepared.index_view.Format = DXGI_FORMAT_R32_UINT;
      prepared.element_count = static_cast<UINT>(draw.batch.indices.size());
    } else {
      prepared.element_count = draw.batch.vertex_count;
    }
    prepared.color_count = static_cast<UINT>(draw.color_count);
    for (std::size_t i = 0; i < draw.color_count; ++i) prepared.rtvs[i] = draw.targets[i]->rtv();
    prepared.has_depth = draw.depth != nullptr;
    if (draw.depth) prepared.dsv = draw.depth->dsv();
    // The guest X/Y viewport transform runs in the vertex shader; the host
    // viewport is the whole render target. Its depth range is the host
    // depth transform's, which the vertex shader's Z part keeps ordered.
    // A D24FS8 target holds half the guest depth on the host (see
    // host_viewport_depth), so its draws use the halved range too.
    const auto depth_format = draw.depth && draw.depth->requires_float24_conversion()
                                  ? DepthRenderTargetFormat::D24FS8
                                  : DepthRenderTargetFormat::D24S8;
    prepared.viewport = {0.0f, 0.0f, float(draw.render_width), float(draw.render_height),
                         host_viewport_depth(depth_format, draw.z_min),
                         host_viewport_depth(depth_format, draw.z_max)};
    prepared.scissor = {draw.scissor_left, draw.scissor_top, draw.scissor_right,
                        draw.scissor_bottom};
    std::copy(draw.blend_constant.begin(), draw.blend_constant.end(),
              prepared.blend_factor.begin());
    prepared.stencil = draw.depth && draw.state.depth_target.stencil_enabled;
    prepared.separate_back_stencil = draw.state.depth_target.backface_stencil_enabled;
    prepared.stencil_reference = draw.state.depth_target.stencil_reference;
    prepared.stencil_back_reference = draw.state.depth_target.stencil_back_reference;
    prepared.statistics = statistics_requested;
    if (statistics_requested) {
      statistics_requested = false;
      statistics_recorded = true;
    }
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
            [this, run = std::move(run)](ID3D12GraphicsCommandList* list) {
              record_prepared_run(list, run);
            },
            parallel) &&
        error.empty())
      error = named(queue.error());
  }

  // Records `run` into `list`: API calls only, from the prepared values, with
  // state the list already holds from the run's previous draw not set again.
  // Safe on any thread (it reads nothing but the run and immutable objects).
  void record_prepared_run(ID3D12GraphicsCommandList* list,
                           const std::vector<PreparedDraw>& run) const {
    ID3D12PipelineState* bound_pipeline = nullptr;
    ID3D12RootSignature* bound_root = nullptr;
    ID3D12DescriptorHeap* bound_heaps[2]{};
    D3D12_PRIMITIVE_TOPOLOGY bound_topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList8> list8;
    bool list8_queried = false;
    for (const auto& draw : run) {
      if (bound_pipeline != draw.pipeline) {
        list->SetPipelineState(draw.pipeline);
        bound_pipeline = draw.pipeline;
      }
      if (bound_root != draw.root_signature) {
        list->SetGraphicsRootSignature(draw.root_signature);
        bound_root = draw.root_signature;
      }
      if (bound_heaps[0] != draw.resource_heap || bound_heaps[1] != draw.sampler_heap) {
        ID3D12DescriptorHeap* heaps[]{draw.resource_heap, draw.sampler_heap};
        list->SetDescriptorHeaps(2, heaps);
        bound_heaps[0] = draw.resource_heap;
        bound_heaps[1] = draw.sampler_heap;
      }
      list->SetGraphicsRootConstantBufferView(0, draw.tables.constants);
      list->SetGraphicsRootDescriptorTable(1, draw.tables.resources);
      list->SetGraphicsRootDescriptorTable(2, draw.tables.samplers);
      list->SetGraphicsRootDescriptorTable(3, draw.tables.memory_export);
      list->OMSetBlendFactor(draw.blend_factor.data());
      list->RSSetViewports(1, &draw.viewport);
      list->RSSetScissorRects(1, &draw.scissor);
      list->OMSetRenderTargets(draw.color_count, draw.color_count ? draw.rtvs.data() : nullptr,
                               FALSE, draw.has_depth ? &draw.dsv : nullptr);
      if (draw.stencil) {
        if (draw.separate_back_stencil && !list8_queried) {
          list8_queried = true;
          (void)list->QueryInterface(IID_PPV_ARGS(&list8));
        }
        if (draw.separate_back_stencil && list8)
          list8->OMSetFrontAndBackStencilRef(draw.stencil_reference, draw.stencil_back_reference);
        else
          list->OMSetStencilRef(draw.stencil_reference);
      }
      if (bound_topology != draw.topology) {
        list->IASetPrimitiveTopology(draw.topology);
        bound_topology = draw.topology;
      }
      if (draw.statistics)
        list->BeginQuery(statistics_heap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
      if (draw.indexed) {
        list->IASetIndexBuffer(&draw.index_view);
        list->DrawIndexedInstanced(draw.element_count, 1, 0, 0, 0);
      } else {
        list->DrawInstanced(draw.element_count, 1, 0, 0);
      }
      if (draw.statistics) {
        list->EndQuery(statistics_heap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
        list->ResolveQueryData(statistics_heap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1,
                               statistics_readback.Get(), 0);
      }
    }
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
  // Ordered work and submissions close the open draw run first.
  impl_->queue.set_before_ordered_work([impl = impl_.get()] { impl->close_draw_run(); });
  for (std::uint32_t i = 0; i < CommandQueue::kFrameCount; ++i) {
    if (!impl_->transient_uploads[i].initialize(
            impl_->context.device(), Impl::kTransientUploadBytes,
            D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ) ||
        !impl_->transient_uploads[i].map(impl_->transient_upload_mappings[i])) {
      impl_->error = impl_->transient_uploads[i].error();
      return false;
    }
  }
  // Before any guest depth is written: every D24 write path then stores the
  // guest's exact codes on this device.
  if (!impl_->calibrate_depth_writes()) return false;
  impl_->ready = true;
  return true;
}

std::optional<Unorm24DepthCalibration> Backend::unorm24_depth_calibration() const {
  return impl_ ? impl_->unorm24_depth_calibration : std::nullopt;
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

Backend::PipelineLibraryStatistics Backend::pipeline_library_statistics() const noexcept {
  if (!impl_ || !impl_->pipeline_library) return {};
  return {true, impl_->pipeline_library->loaded(), impl_->pipeline_library->stored()};
}

void Backend::set_shader_cache_directory(const std::string& directory) {
  impl_->shader_cache.set_disk_directory(directory);
  impl_->open_pipeline_library(directory);
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

}  // namespace xenon::gpu::d3d12

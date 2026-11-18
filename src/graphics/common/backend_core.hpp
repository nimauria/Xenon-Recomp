#pragma once

// State, EDRAM ownership and command consumption shared by the Vulkan and
// D3D12 backends. Each backend's Backend::Impl derives from
// BackendCore<Impl, Api>. Api names the backend's native types and constants:
//
//   struct Api {
//     static constexpr std::string_view kName = "Vulkan";
//     using Context = ...; using CommandQueue = ...;
//     using PresentationSwapchain = ...; using GuestMemoryMirror = ...;
//     using ResourceLayout = ...; using Buffer = ...;
//     using TextureImage = ...; using RenderTargetImage = ...;
//     using DepthTargetImage = ...; using GraphicsPipeline = ...;
//     using Format = ...;                     // native color/depth format
//     static constexpr Format kUndefinedFormat = ...;
//     static Format color_format(const RenderTargetImage* target);
//     static Format native_color_format(ColorRenderTargetFormat format);
//     static constexpr bool kEmulatesFrontAndBackCull = ...;
//     static constexpr bool kSharedBlendFactorConstant = ...;  // one RGBA blend-factor constant
//     static constexpr bool kSupportsDirectColorTransfer = ...;
//     static constexpr bool kSupportsDirectDepthTransfer = ...;
//     // Impl::load_canonical_color/depth() exist (canonical EDRAM loads on
//     // the GPU, edram_canonical_load_supported() formats):
//     static constexpr bool kSupportsCanonicalGpuLoad = ...;
//     // Impl::store_canonical_color/depth() exist (ownership flushes to
//     // canonical EDRAM on the GPU, the same formats):
//     static constexpr bool kSupportsCanonicalGpuStore = ...;
//     // With XENON_HAS_DXC:
//     static constexpr ShaderBinaryFormat kShaderBinaryFormat = ...;
//     static constexpr std::string_view kShaderBinaryName = "SPIR-V";
//   };
//
// Impl supplies the steps whose native calls differ (see "Backend hooks"
// below). It keeps its own destructor so the queue drains before any member,
// including Impl's own, is destroyed.

#include <algorithm>
#include <array>
#include <bit>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "xenon/gpu/backend.hpp"
#include "xenon/logging/activity_trace.hpp"
#include "xenon/logging/probe_log.hpp"
#include "xenon/gpu/edram.hpp"
#include "xenon/gpu/edram_ownership.hpp"
#include "xenon/gpu/depth_format.hpp"
#include "xenon/gpu/edram_surface.hpp"
#include "xenon/gpu/ir.hpp"
#include "xenon/gpu/presentation.hpp"
#include "xenon/gpu/primitive_processor.hpp"
#include "xenon/gpu/index_cache.hpp"
#include "xenon/gpu/guest_memory_segments.hpp"
#include "xenon/gpu/resident_range_allocator.hpp"
#include "xenon/gpu/resource_ir.hpp"
#include "xenon/gpu/gpu_resolve.hpp"
#include "xenon/gpu/perf_stats.hpp"
#include <thread>
#include "xenon/gpu/guest_read_sync.hpp"
#include "xenon/gpu/scanout.hpp"
#include "xenon/gpu/texture.hpp"
#include "xenon/gpu/texture_replacement.hpp"
#include "xenon/gpu/transient_arena.hpp"
#include "xenon/gpu/vertex_position_evaluator.hpp"
#include "xenon/gpu/blend_constant.hpp"
#include "xenon/logging/logger.hpp"
#include "xenon/memory/address_space.hpp"
#ifdef XENON_HAS_DXC
#include "xenon/gpu/dxc_shader_compiler.hpp"
#include "xenon/gpu/shader_compile_queue.hpp"
#include "xenon/gpu/shader_translation.hpp"
#endif

namespace xenon::gpu::detail {

template <typename Derived, typename Api>
class BackendCore {
 public:
  using Context = typename Api::Context;
  using CommandQueue = typename Api::CommandQueue;
  using PresentationSwapchain = typename Api::PresentationSwapchain;
  using GuestMemoryMirror = typename Api::GuestMemoryMirror;
  using ResourceLayout = typename Api::ResourceLayout;
  using Buffer = typename Api::Buffer;
  using TextureImage = typename Api::TextureImage;
  using RenderTargetImage = typename Api::RenderTargetImage;
  using DepthTargetImage = typename Api::DepthTargetImage;
  using GraphicsPipeline = typename Api::GraphicsPipeline;

  // Backend hooks. Derived implements these:
  //   bool create_render_target(RenderTargetImage&, const EdramSurfaceLayout&,
  //                             ColorRenderTargetFormat);
  //   bool fill_unbound_color_slots(const DrawResourceState&, std::uint32_t height,
  //                                 std::size_t color_count,
  //                                 std::array<RenderTargetImage*, 4>&,
  //                                 std::array<std::uint8_t, 4>& write_masks,
  //                                 std::array<BlendState, 4>& blends);
  //   bool create_depth_target(DepthTargetImage&, const EdramSurfaceLayout&,
  //                            DepthRenderTargetFormat);
  //   bool load_texture(TextureImage&, const TextureLayout&,
  //                     std::span<const std::byte> capture, std::uint32_t capture_base);
  //   bool bind_texture(std::uint32_t slot, const TextureDescriptor&, TextureImage&);
  //   bool create_replacement_texture(TextureImage&, const DecodedTexture&);
  //   <root object> pipeline_root();
  //   std::uint32_t color_render_width(const DrawResourceState&, const RenderTargetImage*);
  //   std::uint32_t color_render_height(const DrawResourceState&, const RenderTargetImage*);
  //   bool record_draw(const DrawRecording&, bool& record_failed);
  //   bool gpu_color_resolve(RenderTargetImage&, const GpuResolvePlan&);
  //   bool gpu_color_clear(RenderTargetImage&, const GpuClearConstants&);
  //   With kSupportsDirectColorTransfer / kSupportsDirectDepthTransfer, the
  //   GPU EDRAM tile transfers between native images (same format, then
  //   converting from either kind of image):
  //   bool transfer_color_tiles(RenderTargetImage&, RenderTargetImage&,
  //                             const EdramTransferConstants&, regions);
  //   bool transfer_depth_tiles(DepthTargetImage&, DepthTargetImage&, ...);
  //   bool convert_color_tiles(RenderTargetImage& or DepthTargetImage&,
  //                            RenderTargetImage&, ...);
  //   bool convert_depth_tiles(RenderTargetImage& or DepthTargetImage&,
  //                            DepthTargetImage&, ...);
  //   bool presentation_srgb_target() const;
  //   PresentStatus present_scanout(const ScanoutConstants&);
  //   bool capture_scanout_image(const ScanoutConstants&, PreparedPresentationFrame&);
  //   void forget_backend_render_targets();      // EDRAM was rebound
  //   bool initialize_mirror(memory::AddressSpace&);
  //   bool bind_guest_memory();                  // mirror -> descriptor layout
  //   std::uint64_t pipeline_cache_misses() const;
  //   void add_backend_unsupported_counters(GpuUnsupportedCounters&) const;

  // Everything a backend needs to record one draw. Built after the pipeline
  // is resolved; references stay valid for the record_draw() call only.
  struct DrawRecording {
    const DrawResourceState& state;
    const ProcessedPrimitiveBatch& batch;
    const GraphicsPipeline& pipeline;
    const std::array<RenderTargetImage*, 4>& targets;
    std::size_t color_count;
    DepthTargetImage* depth;
    std::size_t index_byte_size;
    bool memexport_writable;
    std::uint32_t render_width;
    std::uint32_t render_height;
    std::int32_t scissor_left;
    std::int32_t scissor_top;
    std::int32_t scissor_right;
    std::int32_t scissor_bottom;
    // The host viewport's depth range in guest depth units
    // (host_depth_transform); the backend halves it for a D24FS8 target.
    float z_min;
    float z_max;
    // The blend constant to set: the guest's, or on a backend with one
    // shared blend-factor constant, shared_blend_factor_constant()'s.
    std::array<float, 4> blend_constant;
  };

  void begin_submission(memory::AddressSpace& new_memory, Edram& new_edram);
  void consume(const ir::Command& command);
  // `submit` false leaves the open batch for a later submission (see
  // Backend::end_submission_deferred()).
  void end_submission(bool submit = true);
  bool make_guest_memory_cpu_visible(std::uint32_t physical_address,
                                     std::uint32_t size);
  // See Backend::publish_gpu_writes().
  bool publish_gpu_writes(bool wait);
  // When resolves are copied back into guest memory (Backend::set_resolve_readback()).
  ResolveReadback resolve_readback{ResolveReadback::Always};
  bool invalidate_edram_native_state();
  PresentStatus present(const PresentationFrame& frame);
  // Plans scan-out of `frame` to a width x height target and makes its source
  // bytes current in the mirror.
  bool prepare_scanout(const PresentationFrame& frame, std::uint32_t width,
                       std::uint32_t height, bool srgb_target, ScanoutPlan& plan);
  // Scan-out drawn on the GPU into an offscreen image and read back.
  bool capture_scanout(const PresentationFrame& frame, std::uint32_t width,
                       std::uint32_t height, PreparedPresentationFrame& output);
  // Records a new window size; present() applies it (see the definition).
  bool resize_presentation(std::uint32_t width, std::uint32_t height);
  bool apply_pending_resize();
  std::mutex pending_resize_mutex;
  std::optional<std::pair<std::uint32_t, std::uint32_t>> pending_resize;
  GpuPerformanceCounters performance_counters() const noexcept;
  GpuUnsupportedCounters unsupported_counters() const noexcept;
  GpuShaderCoverage shader_coverage() const noexcept;
  // See gpu::Backend::sync_guest_reads().
  void sync_guest_reads();
  void end_guest_read_sync() noexcept { guest_reads.end(); }

  static std::string named(std::string_view message) {
    std::string result(Api::kName);
    result += ' ';
    result += message;
    return result;
  }

  static constexpr std::size_t kTransientUploadBytes = 16u * 1024u * 1024u;
  Context context{};
  CommandQueue queue{};
  PresentationSwapchain presentation{};
  GuestMemoryMirror mirror{};
  ResourceLayout resources{};
  std::array<Buffer, CommandQueue::kFrameCount> transient_uploads{};
  std::array<std::span<std::byte>, CommandQueue::kFrameCount>
      transient_upload_mappings{};
  // GPU-resident processed indices: a cached conversion is copied here once
  // and drawn from until the index cache drops it (attach_resident_indices).
  static constexpr std::size_t kResidentIndexBytes = ProcessedIndexCache::kDefaultCapacityBytes;
  Buffer resident_indices{};
  std::span<std::byte> resident_index_mapping{};
  ResidentRangeAllocator resident_allocator{};
  struct ResidentRange {
    std::uint64_t offset{};
    std::uint64_t size{};
  };
  std::unordered_map<std::uint64_t, ResidentRange> resident_ranges{};
  std::uint64_t next_resident_key{1};
  // Called by the backend once `resident_indices` is created and mapped: the
  // cache's dropped batches release their ranges once the GPU is past them.
  void attach_resident_indices() {
    resident_allocator.reset(resident_index_mapping.size());
    index_cache.set_erase_listener([this](const ProcessedPrimitiveBatch& batch) {
      const auto found = resident_ranges.find(batch.resident_key);
      if (found == resident_ranges.end()) return;
      resident_allocator.release(found->second.offset, found->second.size,
                                 queue.retirement_value());
      resident_ranges.erase(found);
    });
  }
  std::array<TransientArena, CommandQueue::kFrameCount> transient_arenas = [] {
    std::array<TransientArena, CommandQueue::kFrameCount> arenas;
    for (auto& arena : arenas) arena.reset_capacity(kTransientUploadBytes);
    return arenas;
  }();
  ResourceStateTracker resource_state{};
  std::unordered_set<std::uint64_t> pipeline_states{};
  std::unordered_map<std::uint64_t, std::unique_ptr<TextureImage>> textures{};
  std::unordered_map<std::uint64_t, std::unique_ptr<RenderTargetImage>> render_targets{};
  std::unordered_map<std::uint64_t, EdramOwnerId> render_target_owners{};
  std::unordered_map<EdramOwnerId, RenderTargetImage*> owner_render_targets{};
  std::unordered_map<std::uint64_t, std::unique_ptr<DepthTargetImage>> depth_targets{};
  std::unordered_map<std::uint64_t, EdramOwnerId> depth_target_owners{};
  std::unordered_map<EdramOwnerId, DepthTargetImage*> owner_depth_targets{};
  std::unordered_map<std::uint64_t, std::unique_ptr<GraphicsPipeline>> pipelines{};
  // Pipelines the backend's persistent cache supplied (statistics are read
  // from other threads).
  std::atomic<std::uint64_t> pipeline_cache_hits{};
  std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> shader_textures{};
  // Per shader, the filters each fetch constant it samples is fetched with
  // (ShaderReflection::texture_filter_overrides).
  std::unordered_map<std::uint64_t, std::vector<TextureFilterOverride>> shader_texture_filters{};
  // Program hashes consume_shader_load() has handled (lowered and compiled,
  // or found unlowerable); a repeated load of one is a no-op.
  std::unordered_set<std::uint64_t> handled_shader_loads{};
  TextureDirtyTracker texture_dirty{};
  // Converted DMA index buffers, reused while their guest bytes are unchanged
  // (index_cache.hpp); and the draw being consumed's host primitives, shared
  // by its render-target bounds and its recording (draw_primitives()).
  // How guest shaders bind guest memory (one binding unless the backend's
  // descriptors cannot span it; set once, before any shader is lowered).
  GuestMemorySegmentation guest_segments{};
  [[nodiscard]] ShaderLoweringOptions with_guest_segments(ShaderLoweringOptions options) const {
    options.guest_memory_segment_shift = guest_segments.shift;
    options.guest_memory_segment_count = static_cast<std::uint8_t>(guest_segments.count);
    return options;
  }
  ProcessedIndexCache index_cache{};
  std::shared_ptr<const ProcessedPrimitiveBatch> draw_batch{};
  ProcessedPrimitiveBatch draw_batch_storage{};
  std::vector<std::uint32_t> primitive_scratch{};
  std::vector<std::byte> index_snapshot_scratch{};
  bool draw_batch_ready{};
  std::string draw_batch_error{};
  // CPU vertex evaluation plans by vertex shader hash (decoded shaders are
  // content-addressed and immutable, so a plan never goes stale).
  std::unordered_map<std::uint64_t, std::shared_ptr<const VertexEvaluatorPlan>> evaluator_plans{};
  // A write the GPU made to guest memory (resolve, memory export): converted
  // indices read from it are no longer current.
  void note_gpu_guest_write(std::uint32_t address, std::uint32_t size) {
    if (address == 0u && size >= memory::kPhysicalMemorySize) index_cache.clear();
    else index_cache.invalidate(address, size);
  }
  // Texture mods (texture_replacement.hpp): dumping guest-authored textures
  // and sampling replacements in their place.
  TextureReplacements texture_mods{TextureReplacements::from_environment()};
  struct ReplacementTexture {
    std::unique_ptr<TextureImage> image{};  // null: the file was unusable
    TextureDescriptor descriptor{};         // its extent and mip chain
  };
  // By content hash, loaded once per run.
  std::unordered_map<std::uint64_t, ReplacementTexture> replacement_textures{};
  // By texture cache key: the replacement its current contents select.
  std::unordered_map<std::uint64_t, const ReplacementTexture*> replaced_texture_keys{};
  void note_texture_contents(std::uint64_t key, const TextureLayout& layout,
                             std::span<const std::byte> capture, std::uint32_t base,
                             std::uint32_t size);
  // Texture reads as of GPU/CPU sync points (sync_guest_reads()).
  GuestReadSync guest_reads{};
  memory::AddressSpace* memory{};
  Edram* edram{};
  EdramOwnershipTracker edram_ownership{};
  EdramOwnerId next_edram_owner{1};
  std::size_t command_count{};
  std::size_t draw_count{};
  std::size_t compiled_shader_count{};
  GpuPerformanceCounters performance{};
  // Why draws and resolves failed (see GpuFailureLog); each failure is also
  // written to the "gpu" error log, rate-limited per reason.
  GpuFailureLog failure_log{};
  GpuResolveCensus resolve_census{};
  // Part 7 of the AC6 Runtime Readiness pass - see GpuUnsupportedCounters's
  // doc comment. Incremented at the point each category is actually
  // detected, never inferred after the fact from error strings.
  GpuUnsupportedCounters unsupported{};
  // Part 9 - see GpuShaderCoverage's doc comment. Only the primary
  // ir::ShaderLoad path counts as a "discovered" shader (decoded_shaders
  // already keys on that); the float20-depth/writable-guest-memory variant
  // lowerings are re-lowerings of an already-discovered shader, not new
  // discoveries, so they are not double-counted here.
  std::uint64_t shader_translation_failures{};
  std::chrono::steady_clock::time_point submission_started{};
  std::deque<std::pair<std::uint64_t, std::shared_ptr<void>>> retired_resources{};

  // Frees what the GPU has finished with. The completion query is a driver
  // call (a fence/timeline read); with nothing retired it is not made.
  void collect_retired_resources() {
    if (retired_resources.empty()) return;
    const auto completed = queue.completed_value();
    while (!retired_resources.empty() &&
           retired_resources.front().first <= completed) {
      retired_resources.pop_front();
    }
  }

  template <typename T>
  void retire_resource(std::unique_ptr<T> resource) {
    if (!resource) return;
    collect_retired_resources();
    const auto fence_value = queue.retirement_value();
    if (!fence_value || queue.completed_value() >= fence_value) return;
    retired_resources.emplace_back(
        fence_value, std::shared_ptr<void>(std::move(resource)));
  }
#ifdef XENON_HAS_DXC
  std::shared_ptr<DxcShaderCompiler> compiler{std::make_shared<DxcShaderCompiler>()};
  ShaderCache shader_cache{compiler};
  // GPU V3: shaders lowered and compiled on worker threads as soon as the
  // frontend decodes their loads (prefetch_shaders()); null with
  // XENON_GPU_ASYNC_SHADERS=0, when each is compiled where it is consumed.
  // Declared after shader_cache, which its workers use: destroyed first.
  std::unique_ptr<ShaderCompileQueue> shader_prefetch{make_shader_prefetch_queue(shader_cache)};
  static std::unique_ptr<ShaderCompileQueue> make_shader_prefetch_queue(ShaderCache& cache) {
    if (const char* value = std::getenv("XENON_GPU_ASYNC_SHADERS");
        value && value[0] == '0' && value[1] == '\0')
      return nullptr;
    // A few workers: DXC is CPU-heavy, and the guest's own threads run
    // beside them (a handheld gets one).
    unsigned workers = std::clamp(std::thread::hardware_concurrency() / 4u, 1u, 6u);
    if (const char* value = std::getenv("XENON_GPU_SHADER_THREADS"); value && *value) {
      const auto parsed = std::strtoul(value, nullptr, 10);
      if (parsed >= 1u && parsed <= 32u) workers = static_cast<unsigned>(parsed);
    }
    return std::make_unique<ShaderCompileQueue>(cache, workers);
  }
  // How guest shaders compile. SPIR-V is only legalized: the Vulkan driver
  // optimizes it when it builds the pipeline anyway, and DXC's own SPIR-V
  // optimizer cost about three quarters of a mission shader's compile (AC6:
  // 46 ms on average with it off and registers as variables, 174 ms before).
  // DXIL is optimized: the D3D12 driver expects DXC's optimized output, and
  // it compiles faster that way.
  static ShaderCompileOptions guest_shader_options() {
    ShaderCompileOptions options{};
    options.format = Api::kShaderBinaryFormat;
    options.optimize = Api::kShaderBinaryFormat != ShaderBinaryFormat::Spirv;
    return options;
  }
  // Shader variants a draw's state selects (consume_draw()): the key each
  // is cached under and the lowering it takes. Shared by the draw, which
  // uses them, and prefetch_shaders(), which predicts them.
  static constexpr std::uint64_t writable_variant_key(std::uint64_t shader_hash) noexcept {
    return shader_hash ^ 0x4D454D4558505257ull;
  }
  static ShaderLoweringOptions writable_variant_options() noexcept {
    ShaderLoweringOptions options{};
    options.force_guest_memory_rw = true;
    return options;
  }
  struct PixelVariant {
    std::uint64_t key{};
    ShaderLoweringOptions options{};
  };
  // D24FS8 depth output (float20e4 rounding, per sample under MSAA) and
  // PsParamGen (SQ_PROGRAM_CNTL.PARAM_GEN); nullopt when the draw needs
  // neither and samples the pixel shader as loaded.
  static std::optional<PixelVariant> pixel_variant(std::uint64_t pixel_hash, bool float20_depth,
                                                   MsaaSamples samples, bool memexport_writable,
                                                   std::uint8_t param_gen,
                                                   bool point_primitive) noexcept {
    if (!float20_depth && param_gen == ShaderProgramState::kNoParamGen) return std::nullopt;
    const bool per_sample = float20_depth && samples != MsaaSamples::X1;
    PixelVariant variant{};
    variant.key =
        pixel_hash ^ (float20_depth ? 0xD24F20E4D24F20E4ull : 0ull) ^
        (per_sample ? 0x8000000000000000ull : 0ull) ^
        (memexport_writable ? 0x0400000000000000ull : 0ull) ^
        (param_gen != ShaderProgramState::kNoParamGen
             ? 0x5041524D47454E00ull ^ (std::uint64_t(param_gen) << 40u) ^
                   (point_primitive ? 0x0000100000000000ull : 0ull)
             : 0ull);
    if (float20_depth)
      variant.options.pixel_depth_output = PixelDepthOutputMode::Float20e4NearestEven;
    variant.options.force_sample_frequency = per_sample;
    variant.options.force_guest_memory_rw = memexport_writable;
    variant.options.param_gen_interpolator = param_gen;
    variant.options.param_gen_point = point_primitive;
    return variant;
  }
  // A shader variant for a draw: from a prefetch worker when the frontend
  // predicted it (prefetch_variants(); waited for if still compiling),
  // otherwise lowered and compiled here - queued behind other compiles it
  // would wait longer.
  struct VariantResult {
    std::shared_ptr<const LoweredShader> lowered{};
    std::shared_ptr<const CompiledShader> compiled{};
    std::string error{};
  };
  VariantResult prepare_variant(std::uint64_t key, const DecodedShader& decoded,
                                const ShaderLoweringOptions& lowering) {
    ShaderCompileOptions options{};
    options = guest_shader_options();
    VariantResult result{};
    if (shader_prefetch && shader_prefetch->prefetched(key)) {
      ++prefetched_variant_hits;
      const auto wait_start = std::chrono::steady_clock::now();
      auto prepared = shader_prefetch->prepared(key);
      perf::add(perf::Counter::ShaderWaitNs,
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now() - wait_start)
                                               .count()));
      perf::add(perf::Counter::ShaderWaits, 1u);
      if (!prepared || !prepared->lowered) {
        result.error = prepared && !prepared->error.empty()
                           ? prepared->error
                           : std::string("shader prefetch ended before the variant was prepared");
        return result;
      }
      result.lowered = std::move(prepared->lowered);
      result.compiled = std::move(prepared->compiled);
    } else {
      ++variants_compiled_at_draw;
      result.lowered = std::make_shared<const LoweredShader>(
          HlslShaderLowerer::lower(decoded, with_guest_segments(lowering)));
    }
    if (result.lowered->complete && !result.compiled)
      result.compiled = shader_cache.get_or_compile(*result.lowered, options);
    return result;
  }
  // Frontend-thread state of prefetch_shaders(): registers and decoded
  // shaders as of the IR handed over so far, and the variant combinations
  // already predicted. Never touched by consumption.
  ResourceStateTracker prefetch_registers{};
  std::unordered_map<std::uint64_t, std::shared_ptr<const DecodedShader>> prefetch_decoded{};
  std::unordered_set<std::uint64_t> prefetch_variants_seen{};
  std::uint64_t prefetched_variants{};       // frontend thread
  std::uint64_t prefetched_variant_hits{};   // backend thread
  std::uint64_t variants_compiled_at_draw{};  // backend thread
  void prefetch_shaders(std::span<const ir::Command> commands) {
    if (!shader_prefetch) return;
    ShaderCompileOptions options{};
    options = guest_shader_options();
    for (const auto& command : commands) {
      if (const auto* write = std::get_if<ir::RegisterWrite>(&command)) {
        prefetch_registers.apply(*write);
        continue;
      }
      if (const auto* load = std::get_if<ir::ShaderLoad>(&command)) {
        if (!load->decoded) continue;
        prefetch_decoded[load->program.hash()] = load->decoded;
        shader_prefetch->prefetch(
            load->program.hash(),
            [decoded = load->decoded, lowering = with_guest_segments({})] {
              return HlslShaderLowerer::lower(*decoded, lowering);
            },
            options);
        continue;
      }
      if (const auto* draw = std::get_if<ir::DrawPacket>(&command)) prefetch_variants(*draw);
    }
  }
  // Starts the variants a draw will select (consume_draw()) as it is handed
  // over. A prediction from the registers alone: a draw whose depth target
  // turns out unbound compiles a variant it does not use, and one predicted
  // wrongly waits for its variant where it is consumed, as without prefetch.
  void prefetch_variants(const ir::DrawPacket& draw) {
    if (!draw.vertex_shader.valid || !draw.pixel_shader.valid) return;
    const auto vertex = prefetch_decoded.find(draw.vertex_shader.hash);
    const auto pixel = prefetch_decoded.find(draw.pixel_shader.hash);
    if (vertex == prefetch_decoded.end() || pixel == prefetch_decoded.end()) return;
    // The registers variants depend on: RB_SURFACE_INFO (MSAA), RB_DEPTH_INFO
    // (format), RB_DEPTHCONTROL, SQ_PROGRAM_CNTL and SQ_CONTEXT_MISC (param
    // gen). A combination seen before is not predicted again.
    auto mix = [](std::uint64_t hash, std::uint64_t value) {
      hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6u) + (hash >> 2u);
      return hash;
    };
    std::uint64_t combination = mix(draw.vertex_shader.hash, draw.pixel_shader.hash);
    combination = mix(combination, (prefetch_registers.register_value(0x2000u) >> 16u) & 3u);
    combination = mix(combination, (prefetch_registers.register_value(0x2002u) >> 16u) & 1u);
    combination = mix(combination, prefetch_registers.register_value(0x2200u) & 0x7Fu);
    combination = mix(combination, prefetch_registers.register_value(0x2180u));
    combination = mix(combination, prefetch_registers.register_value(0x2181u));
    combination = mix(combination, draw.primitive_type == PrimitiveType::PointList);
    if (prefetch_variants_seen.size() >= 65536u) prefetch_variants_seen.clear();
    if (!prefetch_variants_seen.insert(combination).second) return;

    ShaderCompileOptions options{};
    options = guest_shader_options();
    auto start = [&](std::uint64_t key, const std::shared_ptr<const DecodedShader>& decoded,
                     const ShaderLoweringOptions& lowering) {
      ++prefetched_variants;
      shader_prefetch->prefetch(
          key,
          [decoded, lowering = with_guest_segments(lowering)] {
            return HlslShaderLowerer::lower(*decoded, lowering);
          },
          options);
    };
    const bool vertex_memexport = vertex->second->reflection.memory_exports != 0;
    const bool pixel_memexport = pixel->second->reflection.memory_exports != 0;
    const bool memexport_writable = vertex_memexport || pixel_memexport;
    if (memexport_writable) {
      if (!vertex_memexport)
        start(writable_variant_key(draw.vertex_shader.hash), vertex->second,
              writable_variant_options());
      if (!pixel_memexport)
        start(writable_variant_key(draw.pixel_shader.hash), pixel->second,
              writable_variant_options());
    }
    const auto state = prefetch_registers.snapshot_targets();
    const bool float20_depth =
        !state.program.multipass() &&
        state.depth_target.format == static_cast<std::uint8_t>(DepthRenderTargetFormat::D24FS8) &&
        (state.depth_target.test_enabled || state.depth_target.write_enabled);
    if (const auto variant = pixel_variant(
            draw.pixel_shader.hash, float20_depth,
            static_cast<MsaaSamples>(state.raster.msaa_samples_log2), memexport_writable,
            state.program.param_gen_interpolator(),
            draw.primitive_type == PrimitiveType::PointList))
      start(variant->key, pixel->second, variant->options);
  }
  std::unordered_map<std::uint64_t, std::shared_ptr<const CompiledShader>> shaders{};
  std::unordered_map<std::uint64_t, DecodedShader> decoded_shaders{};
  std::unordered_map<std::uint64_t, std::shared_ptr<const CompiledShader>>
      float20_depth_shaders{};
  std::unordered_map<std::uint64_t, std::shared_ptr<const CompiledShader>>
      writable_guest_memory_shaders{};
  std::shared_ptr<const CompiledShader> rectangle_list_shader{};
  std::shared_ptr<const CompiledShader> point_sprite_shader{};
#endif
  std::string error{};
  bool ready{};

  // How this device's D24 depth writes encode a code, measured once the
  // queue is up (calibrate_depth_writes) and given to every depth image
  // created after it: uploads, GPU clears and EDRAM transfers into D24S8
  // images then store the guest's exact code. Empty until measured.
  Unorm24DepthWrite unorm24_depth_write{Unorm24DepthWrite::Midpoint};
  std::optional<Unorm24DepthCalibration> unorm24_depth_calibration{};

  // Writes calibration words into a D24S8 probe image through the depth
  // upload path (the same SV_Depth write every D24 path uses) under each
  // encoding, reads them back, and keeps the first exact one. Without DXC
  // no shader writes depth, so nothing needs measuring.
  bool calibrate_depth_writes() {
#ifdef XENON_HAS_DXC
    // Whole 80x16 tiles: 4 x 13 tiles, 66560 codes.
    constexpr std::uint32_t kWidth = 320u, kHeight = 208u;
    const EdramSurfaceLayout surface{0u, kWidth, kHeight, MsaaSamples::X1, false, true};
    DepthTargetImage probe;
    if (!self().create_depth_target(probe, surface, DepthRenderTargetFormat::D24S8)) {
      error = named("D24 depth-write calibration image: " + probe.error());
      return false;
    }
    const auto words = unorm24_depth_calibration_words(kWidth * kHeight);
    const auto measured = calibrate_unorm24_depth_write(
        words, [&](Unorm24DepthWrite write, std::span<const std::uint32_t> source,
                   std::vector<std::uint32_t>& stored) {
          probe.set_unorm24_depth_write(write);
          std::uint32_t pitch{};
          return probe.upload_sample(queue, 0u, 0u, 0u, kWidth, kHeight, source, kWidth * 4u) &&
                 probe.readback_sample(queue, 0u, 0u, 0u, kWidth, kHeight, stored, pitch);
        });
    if (!measured) {
      error = named("D24 depth-write calibration failed: " + probe.error());
      return false;
    }
    unorm24_depth_calibration = measured;
    unorm24_depth_write = measured->write;
    const auto name = unorm24_depth_write_name(measured->write);
    if (measured->exact)
      logging::append_probe_log("gpu_device_diag.log", "%.*s D24 depth writes: %.*s (exact)\n",
                                int(Api::kName.size()), Api::kName.data(), int(name.size()),
                                name.data());
    else
      logging::append_probe_log(
          "gpu_device_diag.log",
          "%.*s D24 depth writes: no exact encoding; %.*s leaves %u of %zu codes wrong "
          "(0x%06X stored as 0x%06X)\n",
          int(Api::kName.size()), Api::kName.data(), int(name.size()), name.data(),
          measured->mismatches, words.size(), measured->first_expected >> 8u,
          measured->first_stored >> 8u);
#endif
    return true;
  }

  // Diagnostic: time spent in each step of a whole-surface EDRAM ownership
  // transfer (these run on the GPU pump thread), logged when slow.
  struct TransferTiming {
    const char* name;
    std::array<const char*, 3> steps;
    std::uint32_t width{}, height{}, samples{}, format{};
    std::array<long long, 3> ms{};
    std::uint64_t start_ns{xenon::logging::activity_trace_enabled()
                               ? xenon::logging::activity_now_ns()
                               : 0u};
    std::chrono::steady_clock::time_point started{std::chrono::steady_clock::now()};
    std::uint64_t bytes{};
    template <typename Step>
    bool step(std::size_t index, Step&& run) {
      const auto start = std::chrono::steady_clock::now();
      const bool ok = run();
      ms[index] += std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
      return ok;
    }
    ~TransferTiming() {
      // perf_frame.log: every move through canonical storage on the CPU.
      perf::add(perf::Counter::EdramCpuTransfers, 1u);
      perf::add(perf::Counter::EdramCpuBytes, bytes);
      perf::add(perf::Counter::EdramCpuNs,
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now() - started)
                                               .count()));
      // Activity trace: every move through canonical storage on the CPU.
      if (xenon::logging::activity_trace_enabled()) {
        const auto key = xenon::logging::intern_activity(
            {"edram", std::string("cpu ") + name, {"width", "height", "samples", "format"},
             false});
        xenon::logging::record_activity(key, {width, height, samples, format}, 0, start_ns,
                                        xenon::logging::activity_now_ns() - start_ns);
      }
      if (ms[0] + ms[1] + ms[2] < 20) return;
      xenon::logging::append_probe_log(
          "ir_timing_diag.log", "%s %ux%u x%u format %u: %s %lld ms, %s %lld ms, %s %lld ms\n",
          name,
          width, height, samples, format, steps[0], ms[0], steps[1], ms[1], steps[2], ms[2]);
    }
  };

  // Moving EDRAM contents between a native image and canonical storage, one
  // rectangle at a time. A whole-surface move passes one rectangle; an
  // ownership change passes only the tiles that change hands (see
  // edram_tile_regions()), which is what keeps aliasing render targets from
  // copying entire surfaces through the CPU on every switch.
  bool color_to_canonical(RenderTargetImage& image,
                          std::span<const EdramSurfaceRegion> regions) {
    if (canonical_gpu_store_applies(image)) return native_to_canonical_on_gpu(image, regions);
    const auto& surface = image.surface();
    const auto sample_count = 1u << static_cast<unsigned>(surface.msaa);
    TransferTiming timing{"color flush", {"readback", "convert", "store"},
                          image.width(), image.height(), sample_count,
                          static_cast<std::uint32_t>(image.guest_format())};
    for (const auto& region : regions) {
      const auto left = std::uint32_t(region.left), top = std::uint32_t(region.top);
      const auto width = std::uint32_t(region.right - region.left);
      const auto height = std::uint32_t(region.bottom - region.top);
      const auto canonical_pitch = width * (surface.is_64bpp ? 8u : 4u);
      std::vector<std::byte> canonical(std::size_t(canonical_pitch) * height);
      timing.bytes += std::uint64_t{canonical_pitch} * height * sample_count;
      for (std::uint32_t sample = 0; sample < sample_count; ++sample) {
        std::vector<std::byte> pixels;
        std::uint32_t pitch{};
        if (!timing.step(0, [&] {
              return image.readback_sample(queue, sample, left, top, left + width, top + height,
                                           pixels, pitch);
            }) ||
            !timing.step(1, [&] {
              return host_color_to_edram(image.guest_format(), width, height, pixels, pitch,
                                         canonical, canonical_pitch);
            }) ||
            !timing.step(2, [&] {
              return store_edram_raw(*edram, surface, left, top, width, height, sample, canonical,
                                     canonical_pitch);
            })) {
          error = image.error().empty() ? named("EDRAM ownership sample readback failed")
                                        : image.error();
          return false;
        }
      }
    }
    return true;
  }

  bool depth_to_canonical(DepthTargetImage& image,
                          std::span<const EdramSurfaceRegion> regions) {
    if (canonical_gpu_store_applies(image)) return native_to_canonical_on_gpu(image, regions);
    const auto& surface = image.surface();
    const auto sample_count = 1u << static_cast<unsigned>(surface.msaa);
    TransferTiming timing{"depth flush", {"readback", "-", "store"},
                          image.width(), image.height(), sample_count,
                          static_cast<std::uint32_t>(image.guest_format())};
    for (const auto& region : regions) {
      const auto left = std::uint32_t(region.left), top = std::uint32_t(region.top);
      const auto width = std::uint32_t(region.right - region.left);
      const auto height = std::uint32_t(region.bottom - region.top);
      timing.bytes += std::uint64_t{width} * height * 4u * sample_count;
      for (std::uint32_t sample = 0; sample < sample_count; ++sample) {
        std::vector<std::uint32_t> pixels;
        std::uint32_t pitch{};
        if (!timing.step(0, [&] {
              return image.readback_sample(queue, sample, left, top, left + width, top + height,
                                           pixels, pitch);
            }) ||
            !timing.step(2, [&] {
              return store_edram_raw(*edram, surface, left, top, width, height, sample,
                                     std::as_bytes(std::span(pixels)), width * 4u);
            })) {
          error = image.error().empty() ? named("EDRAM depth ownership sample readback failed")
                                        : image.error();
          return false;
        }
      }
    }
    return true;
  }

  // Canonical EDRAM loaded into a native image on the GPU: the regions'
  // tiles are copied out of the CPU store as they are (no per-sample work on
  // the CPU) and each destination sample unpacks its EDRAM word in a shader
  // (make_edram_canonical_color_load_shader). For the formats
  // edram_canonical_load_supported() accepts; others take the CPU
  // conversion below. XENON_EDRAM_GPU_CANONICAL=0 (development A/B) sends
  // every load through the CPU.
  static bool canonical_gpu_loads_enabled() {
    static const bool enabled = [] {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
      const char* value = std::getenv("XENON_EDRAM_GPU_CANONICAL");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
      return !(value && value[0] == '0' && value[1] == '\0');
    }();
    return enabled;
  }
  template <typename Image>
  [[nodiscard]] bool canonical_gpu_load_applies(const Image& image) const {
    if constexpr (!Api::kSupportsCanonicalGpuLoad) {
      return false;
    } else {
      constexpr bool depth = std::is_same_v<Image, DepthTargetImage>;
      return edram && canonical_gpu_loads_enabled() &&
             edram_canonical_load_supported(
                 {depth, static_cast<std::uint8_t>(image.guest_format())});
    }
  }
  template <typename Image>
  bool canonical_to_native_on_gpu(Image& image, std::span<const EdramSurfaceRegion> regions) {
    if constexpr (!Api::kSupportsCanonicalGpuLoad) {
      error = named("canonical EDRAM loads on the GPU are unavailable on this backend");
      return false;
    } else {
      const auto started = std::chrono::steady_clock::now();
      std::vector<std::uint16_t> tiles;
      for (const auto& region : regions) {
        const auto covered = covered_edram_tiles(image.surface(), region);
        tiles.insert(tiles.end(), covered.begin(), covered.end());
      }
      std::sort(tiles.begin(), tiles.end());
      tiles.erase(std::unique(tiles.begin(), tiles.end()), tiles.end());
      // The mapping's destination half is the image's own surface; the
      // source half is unused by the canonical shader.
      const auto constants = make_edram_transfer_constants(image.surface(), image.host_msaa(),
                                                           image.surface(), image.host_msaa());
      bool loaded = false;
      if constexpr (std::is_same_v<Image, DepthTargetImage>)
        loaded = self().load_canonical_depth(image, *edram, tiles, constants, regions);
      else
        loaded = self().load_canonical_color(image, *edram, tiles, constants, regions);
      if (!loaded) return false;
      perf::add(perf::Counter::EdramGpuLoads, 1u);
      perf::add(perf::Counter::EdramGpuLoadBytes, std::uint64_t{tiles.size()} * Edram::kTileBytes);
      perf::add(perf::Counter::EdramGpuLoadNs,
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now() - started)
                                               .count()));
      ++performance.gpu_canonical_edram_loads;
      return true;
    }
  }

  // The reverse, an ownership flush on the GPU: each sample of the regions is
  // packed into its EDRAM word in a compute shader
  // (make_edram_canonical_store_shader) and the covered tiles are read back
  // once into the CPU store, instead of a readback per sample and a CPU
  // conversion. Words of those tiles the regions do not cover keep the CPU
  // store's value. Same formats and switch as the loads.
  template <typename Image>
  [[nodiscard]] bool canonical_gpu_store_applies(const Image& image) const {
    if constexpr (!Api::kSupportsCanonicalGpuStore) {
      return false;
    } else {
      constexpr bool depth = std::is_same_v<Image, DepthTargetImage>;
      return edram && gpu_canonical_flushes.load(std::memory_order_relaxed) &&
             canonical_gpu_loads_enabled() &&
             edram_canonical_load_supported(
                 {depth, static_cast<std::uint8_t>(image.guest_format())});
    }
  }
  // set_gpu_canonical_edram_flushes(): false sends flushes through the CPU.
  std::atomic<bool> gpu_canonical_flushes{true};
  template <typename Image>
  bool native_to_canonical_on_gpu(Image& image, std::span<const EdramSurfaceRegion> regions) {
    if constexpr (!Api::kSupportsCanonicalGpuStore) {
      error = named("canonical EDRAM flushes on the GPU are unavailable on this backend");
      return false;
    } else {
      const auto started = std::chrono::steady_clock::now();
      std::vector<std::uint16_t> tiles;
      for (const auto& region : regions) {
        const auto covered = covered_edram_tiles(image.surface(), region);
        tiles.insert(tiles.end(), covered.begin(), covered.end());
      }
      std::sort(tiles.begin(), tiles.end());
      tiles.erase(std::unique(tiles.begin(), tiles.end()), tiles.end());
      if (tiles.empty()) return true;
      // The mapping's EDRAM half is the image's own surface; the store
      // shader takes the region from the constants' source half.
      const auto constants = make_edram_transfer_constants(image.surface(), image.host_msaa(),
                                                           image.surface(), image.host_msaa());
      bool stored = false;
      if constexpr (std::is_same_v<Image, DepthTargetImage>)
        stored = self().store_canonical_depth(image, *edram, tiles, constants, regions);
      else
        stored = self().store_canonical_color(image, *edram, tiles, constants, regions);
      if (!stored) return false;
      perf::add(perf::Counter::EdramGpuStores, 1u);
      perf::add(perf::Counter::EdramGpuStoreBytes,
                std::uint64_t{tiles.size()} * Edram::kTileBytes);
      perf::add(perf::Counter::EdramGpuStoreNs,
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now() - started)
                                               .count()));
      ++performance.gpu_canonical_edram_stores;
      return true;
    }
  }

  bool canonical_to_color(RenderTargetImage& image,
                          std::span<const EdramSurfaceRegion> regions) {
    if (canonical_gpu_load_applies(image)) return canonical_to_native_on_gpu(image, regions);
    const auto& surface = image.surface();
    const auto sample_count = 1u << static_cast<unsigned>(surface.msaa);
    TransferTiming timing{"color acquire", {"load", "convert", "upload"},
                          image.width(), image.height(), sample_count,
                          static_cast<std::uint32_t>(image.guest_format())};
    for (const auto& region : regions) {
      const auto left = std::uint32_t(region.left), top = std::uint32_t(region.top);
      const auto width = std::uint32_t(region.right - region.left);
      const auto height = std::uint32_t(region.bottom - region.top);
      const auto canonical_pitch = width * (surface.is_64bpp ? 8u : 4u);
      const auto host_pitch = width * color_host_bytes_per_pixel(image.guest_format());
      std::vector<std::byte> canonical(std::size_t(canonical_pitch) * height);
      std::vector<std::byte> pixels(std::size_t(host_pitch) * height);
      timing.bytes += std::uint64_t{canonical_pitch} * height * sample_count;
      for (std::uint32_t sample = 0; sample < sample_count; ++sample) {
        if (!timing.step(0, [&] {
              return resolve_edram_raw(*edram, surface, left, top, width, height, sample,
                                       canonical, canonical_pitch);
            }) ||
            !timing.step(1, [&] {
              return edram_color_to_host(image.guest_format(), width, height, canonical,
                                         canonical_pitch, pixels, host_pitch);
            }) ||
            !timing.step(2, [&] {
              return image.upload_sample(queue, sample, left, top, left + width, top + height,
                                         pixels, host_pitch);
            })) {
          error = image.error().empty() ? named("canonical EDRAM sample upload failed")
                                        : image.error();
          return false;
        }
      }
    }
    return true;
  }

  bool canonical_to_depth(DepthTargetImage& image,
                          std::span<const EdramSurfaceRegion> regions) {
    if (canonical_gpu_load_applies(image)) return canonical_to_native_on_gpu(image, regions);
    const auto& surface = image.surface();
    const auto sample_count = 1u << static_cast<unsigned>(surface.msaa);
    TransferTiming timing{"depth acquire", {"load", "-", "upload"},
                          image.width(), image.height(), sample_count,
                          static_cast<std::uint32_t>(image.guest_format())};
    for (const auto& region : regions) {
      const auto left = std::uint32_t(region.left), top = std::uint32_t(region.top);
      const auto width = std::uint32_t(region.right - region.left);
      const auto height = std::uint32_t(region.bottom - region.top);
      std::vector<std::uint32_t> pixels(std::size_t(width) * height);
      timing.bytes += std::uint64_t{width} * height * 4u * sample_count;
      for (std::uint32_t sample = 0; sample < sample_count; ++sample) {
        if (!timing.step(0, [&] {
              return resolve_edram_raw(*edram, surface, left, top, width, height, sample,
                                       std::as_writable_bytes(std::span(pixels)), width * 4u);
            }) ||
            !timing.step(2, [&] {
              return image.upload_sample(queue, sample, left, top, left + width, top + height,
                                         pixels, width * 4u);
            })) {
          error = image.error().empty() ? named("canonical EDRAM depth sample upload failed")
                                        : image.error();
          return false;
        }
      }
    }
    return true;
  }

  static std::array<EdramSurfaceRegion, 1> whole_image(std::uint32_t width,
                                                       std::uint32_t height) {
    return {EdramSurfaceRegion{0, 0, std::int32_t(width), std::int32_t(height)}};
  }

  bool flush_color_owner(EdramOwnerId owner) {
    const auto found = owner_render_targets.find(owner);
    if (!edram || found == owner_render_targets.end() || !found->second) {
      error = named("EDRAM alias transfer has no native color owner");
      return false;
    }
    auto* image = found->second;
    if (!color_to_canonical(*image, whole_image(image->width(), image->height()))) return false;
    edram_ownership.release(owner);
    return true;
  }

  bool flush_depth_owner(EdramOwnerId owner) {
    const auto found = owner_depth_targets.find(owner);
    if (!edram || found == owner_depth_targets.end() || !found->second) {
      error = named("EDRAM alias transfer has no native depth owner");
      return false;
    }
    auto* image = found->second;
    if (!depth_to_canonical(*image, whole_image(image->width(), image->height()))) return false;
    edram_ownership.release(owner);
    return true;
  }

  bool flush_owner(EdramOwnerId owner) {
    if (owner_render_targets.contains(owner)) return flush_color_owner(owner);
    if (owner_depth_targets.contains(owner)) return flush_depth_owner(owner);
    error = named("EDRAM alias transfer has no native owner");
    return false;
  }

  // Returns only `tiles` from `owner`'s image to canonical storage; the owner
  // keeps the rest. A set that does not map to whole pixels flushes the owner
  // entirely instead.
  bool flush_owner_tiles(EdramOwnerId owner, std::span<const std::uint16_t> tiles) {
    std::vector<EdramSurfaceRegion> regions;
    if (const auto color = owner_render_targets.find(owner);
        color != owner_render_targets.end() && color->second) {
      if (!edram || !edram_tile_regions(color->second->surface(), tiles, regions))
        return flush_color_owner(owner);
      if (!color_to_canonical(*color->second, regions)) return false;
    } else if (const auto depth = owner_depth_targets.find(owner);
               depth != owner_depth_targets.end() && depth->second) {
      if (!edram || !edram_tile_regions(depth->second->surface(), tiles, regions))
        return flush_depth_owner(owner);
      if (!depth_to_canonical(*depth->second, regions)) return false;
    } else {
      error = named("EDRAM alias transfer has no native owner");
      return false;
    }
    edram_ownership.release_tiles(owner, tiles);
    return true;
  }

  bool make_edram_canonical() {
    if (!edram) {
      error = named("canonical EDRAM checkpoint has no bound EDRAM");
      return false;
    }

    // Snapshot owner IDs before flushing because each flush releases all tiles
    // held by that owner. Owner IDs are unique across color and depth images.
    std::vector<EdramOwnerId> owners;
    owners.reserve(owner_render_targets.size() + owner_depth_targets.size());
    for (const auto& [owner, image] : owner_render_targets) {
      if (image && edram_ownership.owned_tile_count(owner))
        owners.push_back(owner);
    }
    for (const auto& [owner, image] : owner_depth_targets) {
      if (image && edram_ownership.owned_tile_count(owner))
        owners.push_back(owner);
    }
    for (const auto owner : owners) {
      if (edram_ownership.owned_tile_count(owner) && !flush_owner(owner))
        return false;
    }
    return true;
  }

  // Gives `requested_owner` the tiles of `ownership_surface`. Only tiles held
  // elsewhere move. A native owner the requested image can copy from directly
  // (`direct_supported`) hands its tiles over on the GPU (`direct`, which
  // releases them); other native owners return exactly the overlapping tiles
  // to canonical storage, and just those tiles are uploaded into the
  // requested image. Tiles it already owns stay in place.
  template <typename Image, typename ToNative, typename DirectSupported, typename Direct>
  bool acquire_ownership(EdramOwnerId requested_owner, Image& requested,
                         const EdramSurfaceLayout& ownership_surface, ToNative to_native,
                         bool (BackendCore::*flush_self)(EdramOwnerId), const char* stale,
                         DirectSupported direct_supported, Direct direct) {
    // A draw that keeps rendering to the target that already holds these
    // tiles - most draws - changes nothing: no tile list, no plan.
    if (edram_ownership.known_to_hold(requested_owner, ownership_surface)) return true;
    auto plan = edram_ownership.plan(requested_owner, ownership_surface);
    if (!plan.valid) return false;
    if (plan.changes.empty()) {
      edram_ownership.note_holds(requested_owner, ownership_surface);
      return true;
    }
    // Which changes go directly. The rest arrive through canonical storage and
    // must still map to whole pixels of the requested image: otherwise that
    // image is reloaded whole below, over anything copied in directly, so
    // then nothing is.
    std::vector<bool> direct_change(plan.changes.size(), false);
    std::vector<std::uint16_t> incoming;
    for (std::size_t i = 0; i < plan.changes.size(); ++i) {
      const auto& change = plan.changes[i];
      direct_change[i] = change.previous_owner != kCanonicalEdramOwner &&
                         direct_supported(change.previous_owner, change.tiles);
      if (!direct_change[i]) {
        incoming.insert(incoming.end(), change.tiles.begin(), change.tiles.end());
        if (change.previous_owner != kCanonicalEdramOwner)
          note_cpu_transfer(change.previous_owner, requested, change.tiles);
      }
    }
    std::sort(incoming.begin(), incoming.end());
    std::vector<EdramSurfaceRegion> regions;
    const bool incoming_whole = incoming.empty() ||
                                edram_tile_regions(requested.surface(), incoming, regions);
    if (!incoming_whole) {
      std::fill(direct_change.begin(), direct_change.end(), false);
      incoming.clear();
      for (const auto& change : plan.changes)
        incoming.insert(incoming.end(), change.tiles.begin(), change.tiles.end());
      std::sort(incoming.begin(), incoming.end());
    }
    for (std::size_t i = 0; i < plan.changes.size(); ++i) {
      const auto& change = plan.changes[i];
      if (direct_change[i]) {
        if (!direct(change.previous_owner, change.tiles)) return false;
      } else if (change.previous_owner != kCanonicalEdramOwner &&
                 !flush_owner_tiles(change.previous_owner, change.tiles)) {
        return false;
      }
    }
    plan = edram_ownership.plan(requested_owner, ownership_surface);
    if (plan.requires_preservation()) {
      error = named("EDRAM ownership transfer did not reach canonical storage");
      return false;
    }
    if (incoming.empty()) {
      // Everything arrived directly.
    } else if (edram_tile_regions(requested.surface(), incoming, regions)) {
      if (!(this->*to_native)(requested, regions)) return false;
    } else {
      // Not whole pixels: make everything this image holds canonical, then
      // upload the whole image.
      if (edram_ownership.owned_tile_count(requested_owner) &&
          !(this->*flush_self)(requested_owner)) return false;
      plan = edram_ownership.plan(requested_owner, ownership_surface);
      if (!(this->*to_native)(requested, whole_image(requested.width(), requested.height())))
        return false;
    }
    if (!edram_ownership.commit(plan)) {
      error = named(stale);
      ++unsupported.unexpected_ownership_transitions;
      return false;
    }
    edram_ownership.note_holds(requested_owner, ownership_surface);
    return true;
  }

  bool acquire_color_ownership(std::uint64_t key,
                               RenderTargetImage& requested,
                               const EdramSurfaceLayout& ownership_surface) {
    const auto owner_it = render_target_owners.find(key);
    if (!edram || owner_it == render_target_owners.end()) {
      error = named("EDRAM color surface has no ownership identity");
      return false;
    }
    return acquire_ownership(
        owner_it->second, requested, ownership_surface, &BackendCore::canonical_to_color,
        &BackendCore::flush_color_owner, "EDRAM ownership plan became stale",
        [&](EdramOwnerId previous, std::span<const std::uint16_t> tiles) {
          return direct_color_transfer_supported(previous, requested, tiles);
        },
        [&](EdramOwnerId previous, std::span<const std::uint16_t> tiles) {
          return direct_color_transfer(previous, requested, tiles);
        });
  }

  // Probe logs only (edram_cpu_transfers.log): an ownership change between
  // two native images that goes through canonical storage on the CPU, with
  // both views and why the GPU transfer refused it. Each distinct case is
  // logged at its 1st, 2nd, 4th, 8th... occurrence so a per-frame case shows
  // its rate without flooding the log.
  static std::string describe_transfer_image(const RenderTargetImage& image) {
    const auto& s = image.surface();
    char text[160];
    std::snprintf(text, sizeof(text), "color fmt%u base%u pitch%u h%u msaa%u host%u",
                  unsigned(image.guest_format()), unsigned(s.base_tile), unsigned(s.pitch_pixels),
                  unsigned(s.height_pixels), unsigned(s.msaa), unsigned(image.host_msaa()));
    return text;
  }
  static std::string describe_transfer_image(const DepthTargetImage& image) {
    const auto& s = image.surface();
    char text[160];
    std::snprintf(text, sizeof(text), "depth fmt%u base%u pitch%u h%u msaa%u host%u",
                  unsigned(image.guest_format()), unsigned(s.base_tile), unsigned(s.pitch_pixels),
                  unsigned(s.height_pixels), unsigned(s.msaa), unsigned(image.host_msaa()));
    return text;
  }
  template <typename Requested>
  void note_cpu_transfer(EdramOwnerId previous, const Requested& requested,
                         std::span<const std::uint16_t> tiles) {
    if (!xenon::logging::probe_logs_enabled()) return;
    std::string source;
    const char* reason = "no native source";
    const auto classify = [&](const auto& image) {
      source = describe_transfer_image(image);
      if (!edram_direct_transfer_supported(image.surface(), requested.surface(), tiles))
        reason = "source does not hold the tiles whole (or 64bpp parity)";
      else if (!same_view_transfer(image, requested) &&
               !edram_converting_transfer_supported(transfer_endpoint(image),
                                                    transfer_endpoint(requested)))
        reason = "format pair has no converting transfer";
      else
        reason = "backend has no GPU transfer";
    };
    if (const auto color = owner_render_targets.find(previous);
        color != owner_render_targets.end() && color->second)
      classify(*color->second);
    else if (const auto depth = owner_depth_targets.find(previous);
             depth != owner_depth_targets.end() && depth->second)
      classify(*depth->second);
    const auto key = source + " -> " + describe_transfer_image(requested) + ": " + reason;
    const auto count = ++cpu_transfer_cases[key];
    if ((count & (count - 1u)) == 0u)
      xenon::logging::append_probe_log("edram_cpu_transfers.log", "#%llu %s (%zu tiles)\n",
                                       static_cast<unsigned long long>(count), key.c_str(),
                                       tiles.size());
  }
  std::unordered_map<std::string, std::uint64_t> cpu_transfer_cases{};

  static EdramTransferEndpoint transfer_endpoint(const RenderTargetImage& image) noexcept {
    return {false, static_cast<std::uint8_t>(image.guest_format())};
  }
  static EdramTransferEndpoint transfer_endpoint(const DepthTargetImage& image) noexcept {
    return {true, static_cast<std::uint8_t>(image.guest_format())};
  }

  // Whether `previous` - a native color or depth image - can hand `tiles`
  // to `requested` on the GPU: it holds them whole, and either shares
  // `requested`'s format (a plain copy) or both views have a converting
  // transfer (each sample's EDRAM word repacked in the shader).
  template <typename Requested>
  bool gpu_transfer_supported(EdramOwnerId previous, const Requested& requested,
                              std::span<const std::uint16_t> tiles) const {
    const auto supported = [&](const auto& source) {
      if (static_cast<const void*>(&source) == static_cast<const void*>(&requested) ||
          !edram_direct_transfer_supported(source.surface(), requested.surface(), tiles))
        return false;
      return same_view_transfer(source, requested) ||
             edram_converting_transfer_supported(transfer_endpoint(source),
                                                 transfer_endpoint(requested));
    };
    if (const auto color = owner_render_targets.find(previous);
        color != owner_render_targets.end() && color->second)
      return supported(*color->second);
    if (const auto depth = owner_depth_targets.find(previous);
        depth != owner_depth_targets.end() && depth->second)
      return supported(*depth->second);
    return false;
  }

  // Images whose host storage the same-format transfer copies sample for
  // sample.
  static bool same_view_transfer(const RenderTargetImage& source,
                                 const RenderTargetImage& destination) noexcept {
    return source.guest_format() == destination.guest_format();
  }
  static bool same_view_transfer(const DepthTargetImage& source,
                                 const DepthTargetImage& destination) noexcept {
    return source.guest_format() == destination.guest_format() &&
           source.format() == destination.format();
  }
  static bool same_view_transfer(const RenderTargetImage&, const DepthTargetImage&) noexcept {
    return false;
  }
  static bool same_view_transfer(const DepthTargetImage&, const RenderTargetImage&) noexcept {
    return false;
  }

  // Hands `tiles` from `previous` to `requested` on the GPU (see
  // gpu_transfer_supported), then releases them from `previous`.
  template <typename Requested>
  bool gpu_transfer(EdramOwnerId previous, Requested& requested,
                    std::span<const std::uint16_t> tiles) {
    static const auto activity_key = xenon::logging::intern_activity(
        {"edram", "gpu transfer", {"tiles", "to_depth", "to_format", ""}, false});
    xenon::logging::ActivityScope activity(
        activity_key, {tiles.size(), std::is_same_v<Requested, DepthTargetImage> ? 1u : 0u,
                       static_cast<std::uint64_t>(requested.guest_format()), 0u});
    std::vector<EdramSurfaceRegion> regions;
    if (!edram_tile_regions(requested.surface(), tiles, regions)) {
      error = named("direct EDRAM transfer tiles are not whole pixels");
      return false;
    }
    const auto run = [&](auto& source) {
      const auto constants = make_edram_transfer_constants(
          source.surface(), source.host_msaa(), requested.surface(), requested.host_msaa());
      if (same_view_transfer(source, requested)) {
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(source)>, Requested>) {
          if constexpr (std::is_same_v<Requested, RenderTargetImage>)
            return self().transfer_color_tiles(source, requested, constants, regions);
          else
            return self().transfer_depth_tiles(source, requested, constants, regions);
        }
      }
      if constexpr (std::is_same_v<Requested, RenderTargetImage>)
        return self().convert_color_tiles(source, requested, constants, regions);
      else
        return self().convert_depth_tiles(source, requested, constants, regions);
    };
    bool moved = false;
    if (const auto color = owner_render_targets.find(previous);
        color != owner_render_targets.end() && color->second) {
      moved = run(*color->second);
    } else if (const auto depth = owner_depth_targets.find(previous);
               depth != owner_depth_targets.end() && depth->second) {
      moved = run(*depth->second);
    } else {
      error = named("direct EDRAM transfer has no native source");
      return false;
    }
    if (!moved) return false;
    edram_ownership.release_tiles(previous, tiles);
    ++performance.direct_edram_transfers;
    return true;
  }

  // A native image holding the tiles whole - color or depth, of this format
  // or another the converting transfer takes - hands them over on the GPU,
  // sample by sample through the EDRAM address mapping, instead of a
  // readback, conversion and upload through canonical storage.
  bool direct_color_transfer_supported(EdramOwnerId previous, const RenderTargetImage& requested,
                                       std::span<const std::uint16_t> tiles) const {
    if constexpr (!Api::kSupportsDirectColorTransfer) return false;
    return gpu_transfer_supported(previous, requested, tiles);
  }

  // The native color image owning every EDRAM tile `region` of `surface`
  // covers, when one image owns them all and holds them whole; null when
  // any is canonical or the tiles are split between owners.
  RenderTargetImage* single_color_owner(const EdramSurfaceLayout& surface,
                                        const EdramSurfaceRegion& region) const {
    const auto tiles = covered_edram_tiles(surface, region);
    if (tiles.empty()) return nullptr;
    const auto owner = edram_ownership.owner(tiles.front());
    if (owner == kCanonicalEdramOwner) return nullptr;
    for (const auto tile : tiles)
      if (edram_ownership.owner(tile) != owner) return nullptr;
    const auto found = owner_render_targets.find(owner);
    if (found == owner_render_targets.end() || !found->second) return nullptr;
    if (!edram_direct_transfer_supported(found->second->surface(), surface, tiles))
      return nullptr;
    return found->second;
  }

  // Why single_color_owner() found no image, for the CPU-path resolve probe.
  std::string describe_missing_color_owner(const EdramSurfaceLayout& surface,
                                           const EdramSurfaceRegion& region) const {
    const auto tiles = covered_edram_tiles(surface, region);
    if (tiles.empty()) return "the rectangle covers no EDRAM tile";
    std::size_t canonical = 0;
    std::set<EdramOwnerId> owners;
    for (const auto tile : tiles) {
      const auto owner = edram_ownership.owner(tile);
      if (owner == kCanonicalEdramOwner) ++canonical;
      else owners.insert(owner);
    }
    if (owners.empty()) return "every tile is canonical (no native image holds them)";
    if (canonical || owners.size() > 1u)
      return std::to_string(canonical) + " canonical tiles and " + std::to_string(owners.size()) +
             " owners";
    const auto found = owner_render_targets.find(*owners.begin());
    if (found == owner_render_targets.end() || !found->second)
      return "the owner is not a color image (a depth image holds the tiles)";
    const auto& held = found->second->surface();
    return "the owner's view cannot be read directly (owner pitch " +
           std::to_string(held.pitch_pixels) + " msaa " +
           std::to_string(static_cast<unsigned>(held.msaa)) + " 64bpp " +
           std::to_string(held.is_64bpp ? 1 : 0) + ", resolve pitch " +
           std::to_string(surface.pitch_pixels) + " msaa " +
           std::to_string(static_cast<unsigned>(surface.msaa)) + ")";
  }

  // The same for a depth image: the native depth image owning every tile
  // `region` of `surface` covers and holding them whole, or null.
  DepthTargetImage* single_depth_owner(const EdramSurfaceLayout& surface,
                                       const EdramSurfaceRegion& region) const {
    const auto tiles = covered_edram_tiles(surface, region);
    if (tiles.empty()) return nullptr;
    const auto owner = edram_ownership.owner(tiles.front());
    if (owner == kCanonicalEdramOwner) return nullptr;
    for (const auto tile : tiles)
      if (edram_ownership.owner(tile) != owner) return nullptr;
    const auto found = owner_depth_targets.find(owner);
    if (found == owner_depth_targets.end() || !found->second) return nullptr;
    if (!edram_direct_transfer_supported(found->second->surface(), surface, tiles))
      return nullptr;
    return found->second;
  }

  bool direct_color_transfer(EdramOwnerId previous, RenderTargetImage& requested,
                             std::span<const std::uint16_t> tiles) {
    if constexpr (!Api::kSupportsDirectColorTransfer) {
      error = named("direct EDRAM color transfer is unavailable on this backend");
      return false;
    } else {
      return gpu_transfer(previous, requested, tiles);
    }
  }

  bool acquire_depth_ownership(std::uint64_t key,
                               DepthTargetImage& requested,
                               const EdramSurfaceLayout& ownership_surface) {
    const auto owner_it = depth_target_owners.find(key);
    if (!edram || owner_it == depth_target_owners.end()) {
      error = named("EDRAM depth surface has no ownership identity");
      return false;
    }
    return acquire_ownership(
        owner_it->second, requested, ownership_surface, &BackendCore::canonical_to_depth,
        &BackendCore::flush_depth_owner, "depth EDRAM ownership plan became stale",
        [&](EdramOwnerId previous, std::span<const std::uint16_t> tiles) {
          return direct_depth_transfer_supported(previous, requested, tiles);
        },
        [&](EdramOwnerId previous, std::span<const std::uint16_t> tiles) {
          return direct_depth_transfer(previous, requested, tiles);
        });
  }

  // Depth images take tiles on the GPU as color images do: a depth image
  // changing hands went through a readback, a CPU repack and an upload with a
  // queue drain each time, and AC6 moves one whole-EDRAM depth surface
  // between D24S8, D24FS8 and color views every frame, which held its first
  // cutscene below one frame a second.
  bool direct_depth_transfer_supported(EdramOwnerId previous, const DepthTargetImage& requested,
                                       std::span<const std::uint16_t> tiles) const {
    if constexpr (!Api::kSupportsDirectDepthTransfer) return false;
    return gpu_transfer_supported(previous, requested, tiles);
  }

  bool direct_depth_transfer(EdramOwnerId previous, DepthTargetImage& requested,
                             std::span<const std::uint16_t> tiles) {
    if constexpr (!Api::kSupportsDirectDepthTransfer) {
      error = named("direct EDRAM depth transfer is unavailable on this backend");
      return false;
    } else {
      return gpu_transfer(previous, requested, tiles);
    }
  }

  bool make_region_canonical(const EdramSurfaceLayout& surface,
                             EdramSurfaceRegion region) {
    if (!edram || !surface.valid()) {
      error = named("Xenos resolve clear has an invalid EDRAM surface");
      return false;
    }
    std::unordered_set<EdramOwnerId> owners;
    for (const auto tile : covered_edram_tiles(surface, region)) {
      const auto owner = edram_ownership.owner(tile);
      if (owner != kCanonicalEdramOwner) owners.insert(owner);
    }
    for (const auto owner : owners)
      if (!flush_owner(owner)) return false;
    return true;
  }

  // The draws since the last resolve, kept compactly so that when a resolve
  // first comes out all zero, gpu.log shows what was drawn into it: targets,
  // depth/stencil state, write mask and the textures each draw sampled.
  struct DrawTrailTexture {
    std::uint8_t slot{};
    std::uint8_t format{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t base_address{};
    std::uint32_t pitch{};
    std::uint16_t swizzle{};
    std::int8_t exp_adjust{};
    std::uint8_t signs{};
    std::uint8_t endian{};
    bool tiled{};
  };
  struct DrawTrailEntry {
    std::uint64_t vertex_shader{};
    std::uint64_t pixel_shader{};
    std::uint32_t index_count{};
    std::uint8_t primitive{};
    std::uint16_t color_base{};
    std::uint8_t color_format{};
    std::uint8_t color_mask{};
    bool color_enabled{};
    bool blend{};
    std::uint16_t depth_base{};
    std::uint8_t depth_format{};
    bool depth_test{};
    bool depth_write{};
    std::uint8_t depth_function{};
    bool stencil{};
    bool alpha_test{};
    std::uint16_t pitch{};
    std::uint8_t msaa_log2{};
    std::int32_t scissor[4]{};
    float viewport[6]{};
    bool window_offset{};
    std::int32_t window_offset_xy[2]{};
    std::int32_t drawable_bottom{};
    // PA_CL_VTE_CNTL, PA_CL_CLIP_CNTL and PA_SU_SC_MODE_CNTL as set.
    std::uint32_t vte_control{};
    std::uint32_t clip_control{};
    std::uint32_t mode_control{};
    // RB_BLENDCONTROL0 and RB_COLORCONTROL as set: which blend, and the
    // alpha test and alpha-to-coverage behind a translucent draw's look.
    std::uint32_t blend_control{};
    std::uint32_t color_control{};
    std::array<DrawTrailTexture, 4> textures{};
    std::uint8_t texture_count{};
    // The highest pixel-shader float constants the shader reads, as set.
    std::array<std::uint16_t, 4> ps_constant_index{};
    std::array<std::array<float, 4>, 4> ps_constant_value{};
    std::uint8_t ps_constant_count{};
    // The same for the vertex shader, and its first vertex buffer.
    std::array<std::uint16_t, 4> vs_constant_index{};
    std::array<std::array<float, 4>, 4> vs_constant_value{};
    std::uint8_t vs_constant_count{};
    std::uint32_t vertex_buffer{};
    std::uint32_t vertex_buffer_dwords{};
    std::uint8_t vertex_buffer_slot{};
    // Its first 12 dwords as the draw was consumed (big-endian words).
    std::array<std::uint32_t, 12> vertex_words{};
    std::uint8_t vertex_word_count{};
  };
  static constexpr std::size_t kDrawTrailLength = 48;
  std::array<DrawTrailEntry, kDrawTrailLength> draw_trail{};
  std::size_t draw_trail_count{};

  // The lowest pixel row (exclusive) `draw` can reach. ResourceIrBuilder
  // bounds a draw that clips against a transformed viewport; one with
  // clipping disabled or the viewport transform off positions its vertices
  // in render-target pixels itself (Direct3D's EDRAM clears and resolve
  // rectangles), and is left at the scissor - 8192 rows when the scissor test
  // is off, which sized AC6's clear targets to all of EDRAM and moved every
  // tile's ownership for each clear (55-990 ms each). For such a draw of up
  // to kMaxEvaluatedVertices vertices, its vertex shader runs on the CPU
  // (VertexPositionEvaluator) and the draw reaches the lowest vertex. A
  // shader that cannot be evaluated keeps the scissor bound and is counted.
  static constexpr std::uint32_t kMaxEvaluatedVertices = 64;
  [[nodiscard]] std::int32_t drawable_bottom_for(const ir::DrawPacket& draw,
                                                 const DrawResourceState& state) {
    const auto scissor_bound = state.raster.drawable_bottom;
    const auto vte = resource_state.register_value(0x2206u);
    const bool viewport_bounded = !state.raster.clip_disabled && (vte & 0x4u) && (vte & 0x8u);
    // Points grow into sprites of their exported or register size; they keep
    // the scissor bound.
    if (viewport_bounded || !memory || !draw.vertex_shader.valid || draw.index_count == 0u ||
        draw.index_count > kMaxEvaluatedVertices || scissor_bound <= 0 ||
        draw.primitive_type == PrimitiveType::PointList)
      return scissor_bound;
    const auto decoded = decoded_shaders.find(draw.vertex_shader.hash);
    if (decoded == decoded_shaders.end()) return scissor_bound;
    auto& plan = evaluator_plans[draw.vertex_shader.hash];
    if (!plan) plan = std::make_shared<const VertexEvaluatorPlan>(decoded->second);
    if (!plan->supported()) {
      ++unsupported.unsupported_shader_features;
      return scissor_bound;
    }
    // The vertices the host would run: the processed batch's indices, or
    // 0..n-1 for a non-indexed one - the same batch the draw records.
    const auto* batch_pointer = draw_primitives(draw, state);
    if (!batch_pointer) return scissor_bound;
    const auto& batch = *batch_pointer;
    if (batch.topology == HostPrimitiveTopology::PointList) return scissor_bound;
    const std::span<const std::uint32_t> indexed_order(batch.indices);
    const auto order_at = [&](std::size_t i) {
      return batch.indexed ? indexed_order[i] : static_cast<std::uint32_t>(i);
    };
    const std::size_t order_size = batch.indexed ? indexed_order.size() : batch.vertex_count;
    // At most kMaxEvaluatedVertices distinct vertices: fixed storage.
    std::array<std::uint32_t, kMaxEvaluatedVertices> vertex_storage{};
    std::size_t vertex_count = 0;
    for (std::size_t i = 0; i < order_size; ++i) {
      const auto vertex = order_at(i);
      const auto end = vertex_storage.begin() + vertex_count;
      const auto at = std::lower_bound(vertex_storage.begin(), end, vertex);
      if (at != end && *at == vertex) continue;
      if (vertex_count == kMaxEvaluatedVertices) return scissor_bound;
      std::move_backward(at, end, end + 1);
      *at = vertex;
      ++vertex_count;
    }
    const std::span<const std::uint32_t> vertices(vertex_storage.data(), vertex_count);
    if (vertices.empty()) return scissor_bound;
    perf::add(perf::Counter::BoundsEvaluations, 1u);
    const perf::ScopedTime bounds_time(perf::Counter::BoundsNs);

    std::array<std::byte, kShaderConstantBufferBytes> constants{};
    if (!resource_state.write_constant_buffer(constants)) return scissor_bound;
    // Vertex data as the GPU will read it: CPU writes folded in, GPU-written
    // (memexported) data brought back.
    const GuestPhysicalReader reader = [&](std::uint32_t address, std::span<std::byte> out) {
      if (std::uint64_t{address} + out.size() > memory::kPhysicalMemorySize) return false;
      if (!mirror.make_cpu_visible(address, static_cast<std::uint32_t>(out.size()),
                                   memory::GpuRangeUsage::VertexBuffer))
        return false;
      return memory->copy_physical_range(address, out);
    };
    float lowest = -INFINITY;
    // Each vertex's pixel, in `vertices` order (sorted, for lookup below).
    std::array<std::array<float, 2>, kMaxEvaluatedVertices> pixel_storage{};
    for (std::size_t i = 0; i < vertices.size(); ++i) {
      const auto evaluated =
          VertexPositionEvaluator::evaluate(*plan, constants, vertices[i], reader);
      if (!evaluated.valid) {
        ++unsupported.unsupported_shader_features;
        return scissor_bound;
      }
      pixel_storage[i] = evaluated.pixel;
      lowest = (std::max)(lowest, evaluated.pixel[1]);
    }
    const auto pixel_of = [&](std::uint32_t vertex) -> const std::array<float, 2>& {
      return pixel_storage[static_cast<std::size_t>(
          std::lower_bound(vertices.begin(), vertices.end(), vertex) - vertices.begin())];
    };
    // A rectangle list's fourth corner, as the expansion geometry shader
    // makes it (make_rectangle_list_geometry_shader): A + B - C, with A-B the
    // longest side. Off an axis-aligned rectangle it can reach lower.
    if (batch.requires_rectangle_expansion) {
      for (std::size_t i = 0; i + 2u < order_size; i += 3u) {
        const std::array<std::array<float, 2>, 3> p{pixel_of(order_at(i)),
                                                     pixel_of(order_at(i + 1u)),
                                                     pixel_of(order_at(i + 2u))};
        const auto distance = [&](std::size_t u, std::size_t v) {
          const float dx = p[u][0] - p[v][0], dy = p[u][1] - p[v][1];
          return dx * dx + dy * dy;
        };
        const float d01 = distance(0, 1), d12 = distance(1, 2), d20 = distance(2, 0);
        std::size_t a = 2, b = 0, c = 1;
        if (d01 >= d12 && d01 >= d20) a = 0, b = 1, c = 2;
        else if (d12 >= d20) a = 1, b = 2, c = 0;
        lowest = (std::max)(lowest, p[a][1] + p[b][1] - p[c][1]);
      }
    }
    // Rows whose pixel centres lie above the lowest point.
    if (!std::isfinite(lowest)) return scissor_bound;
    if (!(lowest > 0.0f)) return (std::min)(scissor_bound, 1);
    const auto rows = static_cast<std::int32_t>(std::ceil((std::min)(lowest, 16384.0f)));
    return (std::min)(scissor_bound, (std::max)(1, rows));
  }

  // The host primitives of the draw being consumed: computed once per draw
  // (draw_batch_ready is cleared when consume_draw() starts) and shared by
  // its render-target bounds and its recording. A DMA index buffer comes
  // from the processed-index cache while its guest bytes are unchanged;
  // otherwise its bytes are copied out once - after any GPU-written part is
  // made CPU-visible - converted, and cached. Null, with draw_batch_error,
  // when the draw's primitives cannot be produced.
  const ProcessedPrimitiveBatch* draw_primitives(const ir::DrawPacket& draw,
                                                 const DrawResourceState& state) {
    if (draw_batch_ready) return draw_batch ? draw_batch.get() : nullptr;
    draw_batch_ready = true;
    draw_batch.reset();
    draw_batch_error.clear();
    const PrimitiveProcessingOptions options{state.primitive_assembly.reset_enabled,
                                             state.primitive_assembly.reset_index};
    if (draw.source != DrawSource::Dma) {
      if (!process_primitives_into(draw, {}, options, 0u, draw_batch_storage, primitive_scratch)) {
        draw_batch_error = draw_batch_storage.error;
        return nullptr;
      }
      // Non-owning: the storage lives as long as the backend.
      draw_batch = std::shared_ptr<const ProcessedPrimitiveBatch>(
          std::shared_ptr<const ProcessedPrimitiveBatch>{}, &draw_batch_storage);
      return draw_batch.get();
    }
    if (!memory) {
      draw_batch_error = named("indexed draw has no guest memory");
      return nullptr;
    }
    const auto& buffer = draw.index_buffer;
    const auto bytes_per_index = buffer.format == IndexFormat::UInt32 ? 4u : 2u;
    const auto count = (std::min)(draw.index_count, buffer.length_bytes / bytes_per_index);
    const auto index_bytes = std::uint64_t{count} * bytes_per_index;
    if (!buffer.valid || index_bytes > UINT32_MAX ||
        std::uint64_t{buffer.physical_address} + index_bytes > memory::kPhysicalMemorySize) {
      draw_batch_error = named("index snapshot range is invalid");
      return nullptr;
    }
    const auto source_bytes = static_cast<std::uint32_t>(index_bytes);
    // Bytes the GPU wrote come back first; such a buffer is converted afresh
    // and not cached (it changed outside the page epochs the cache checks).
    bool cacheable = true;
    if (source_bytes && mirror.has_gpu_dirty(buffer.physical_address, source_bytes)) {
      cacheable = false;
      index_cache.invalidate(buffer.physical_address, source_bytes);
      if (!mirror.make_cpu_visible(buffer.physical_address, source_bytes,
                                   memory::GpuRangeUsage::IndexBuffer)) {
        draw_batch_error = mirror.error();
        return nullptr;
      }
    }
    const IndexCacheKey key{buffer.physical_address,
                            count,
                            buffer.format,
                            buffer.endian,
                            draw.primitive_type,
                            options.reset_enabled,
                            options.reset_index};
    // Taken before the bytes are read: a write racing the copy has a newer
    // epoch, so the entry is found stale next time.
    const auto epoch = memory->coherency().current_epoch();
    if (cacheable) {
      if (auto hit = index_cache.find(key, source_bytes, memory->coherency(), epoch)) {
        perf::add(perf::Counter::IndexCacheHits, 1u);
        draw_batch = std::move(hit);
        return draw_batch.get();
      }
    }
    perf::add(perf::Counter::IndexCacheMisses, 1u);
    index_snapshot_scratch.resize(source_bytes);
    if (!memory->copy_physical_range(buffer.physical_address, index_snapshot_scratch)) {
      draw_batch_error = named("index snapshot failed");
      return nullptr;
    }
    auto batch = std::make_shared<ProcessedPrimitiveBatch>();
    if (!process_primitives_into(draw, index_snapshot_scratch, options, buffer.physical_address,
                                 *batch, primitive_scratch)) {
      draw_batch_error = batch->error;
      return nullptr;
    }
    perf::add(perf::Counter::IndexBytesConverted, batch->indices.size() * sizeof(std::uint32_t));
    if (cacheable) {
      batch->resident_key = next_resident_key++;
      index_cache.insert(key, source_bytes, epoch, batch);
    }
    draw_batch = std::move(batch);
    return draw_batch.get();
  }

  void note_draw(const ir::DrawPacket& draw, const DrawResourceState& state) {
    auto& entry = draw_trail[draw_trail_count % kDrawTrailLength];
    ++draw_trail_count;
    entry = {};
    entry.vertex_shader = draw.vertex_shader.hash;
    entry.pixel_shader = draw.pixel_shader.hash;
    entry.index_count = draw.index_count;
    entry.primitive = static_cast<std::uint8_t>(draw.primitive_type);
    const auto& color = state.color_targets[0];
    entry.color_base = color.base_tile;
    entry.color_format = color.format;
    entry.color_mask = color.write_mask;
    entry.color_enabled = color.enabled;
    entry.blend = color.blend.enabled;
    entry.depth_base = state.depth_target.base_tile;
    entry.depth_format = state.depth_target.format;
    entry.depth_test = state.depth_target.test_enabled;
    entry.depth_write = state.depth_target.write_enabled;
    entry.depth_function = static_cast<std::uint8_t>(state.depth_target.function);
    entry.stencil = state.depth_target.stencil_enabled;
    entry.alpha_test = state.pixel_control.alpha_test_enabled;
    entry.pitch = state.raster.surface_pitch;
    entry.msaa_log2 = state.raster.msaa_samples_log2;
    entry.scissor[0] = state.raster.scissor_left;
    entry.scissor[1] = state.raster.scissor_top;
    entry.scissor[2] = state.raster.scissor_right;
    entry.scissor[3] = state.raster.scissor_bottom;
    for (std::size_t slot = 0; slot < state.textures.size() &&
                               entry.texture_count < entry.textures.size();
         ++slot) {
      const auto& texture = state.textures[slot];
      if (!texture) continue;
      auto& kept = entry.textures[entry.texture_count++];
      kept = {static_cast<std::uint8_t>(slot), texture->format, texture->width, texture->height,
              texture->base_address};
      kept.pitch = texture->pitch;
      kept.swizzle = texture->swizzle;
      kept.exp_adjust = texture->exp_adjust;
      kept.signs = static_cast<std::uint8_t>(texture->signs[0] | (texture->signs[1] << 2) |
                                             (texture->signs[2] << 4) | (texture->signs[3] << 6));
      kept.endian = static_cast<std::uint8_t>(texture->endian);
      kept.tiled = texture->tiled;
    }
    const auto& viewport = state.raster.viewport;
    entry.viewport[0] = viewport.x_scale;
    entry.viewport[1] = viewport.x_offset;
    entry.viewport[2] = viewport.y_scale;
    entry.viewport[3] = viewport.y_offset;
    entry.viewport[4] = viewport.z_scale;
    entry.viewport[5] = viewport.z_offset;
    entry.window_offset = state.raster.vertex_window_offset_enabled;
    entry.window_offset_xy[0] = state.raster.window_offset_x;
    entry.window_offset_xy[1] = state.raster.window_offset_y;
    entry.drawable_bottom = state.raster.drawable_bottom;
    entry.vte_control = resource_state.register_value(0x2206u);
    entry.clip_control = resource_state.register_value(0x2204u);
    entry.mode_control = resource_state.register_value(0x2205u);
    entry.blend_control = resource_state.register_value(0x2201u);
    entry.color_control = resource_state.register_value(0x2202u);
    if (const auto pixel = decoded_shaders.find(draw.pixel_shader.hash);
        pixel != decoded_shaders.end()) {
      const auto& used = pixel->second.reflection.float_constants;
      const auto first = used.size() > entry.ps_constant_index.size()
                             ? used.size() - entry.ps_constant_index.size()
                             : 0u;
      for (auto i = first; i < used.size(); ++i) {
        const auto index = used[i];
        auto& value = entry.ps_constant_value[entry.ps_constant_count];
        for (std::uint32_t c = 0; c < 4; ++c) {
          // Pixel-shader c[n] is ALU constant 256 + n: registers 0x4400 + 4n.
          value[c] = std::bit_cast<float>(
              resource_state.register_value(0x4000u + (256u + (index & 255u)) * 4u + c));
        }
        entry.ps_constant_index[entry.ps_constant_count++] = index;
      }
    }
    if (const auto vertex = decoded_shaders.find(draw.vertex_shader.hash);
        vertex != decoded_shaders.end()) {
      const auto& used = vertex->second.reflection.float_constants;
      const auto first = used.size() > entry.vs_constant_index.size()
                             ? used.size() - entry.vs_constant_index.size()
                             : 0u;
      for (auto i = first; i < used.size(); ++i) {
        const auto index = used[i];
        auto& value = entry.vs_constant_value[entry.vs_constant_count];
        for (std::uint32_t c = 0; c < 4; ++c) {
          // Vertex-shader c[n] is ALU constant n: registers 0x4000 + 4n.
          value[c] = std::bit_cast<float>(
              resource_state.register_value(0x4000u + (index & 255u) * 4u + c));
        }
        entry.vs_constant_index[entry.vs_constant_count++] = index;
      }
    }
    // The highest valid fetch constant: where the D3D runtime puts the
    // vertices of a user-pointer draw (fetch constant 95, slot 31/2).
    for (std::size_t slot = state.vertex_buffers.size(); slot-- > 0 && !entry.vertex_buffer;) {
      for (std::size_t sub = state.vertex_buffers[slot].size(); sub-- > 0;) {
        const auto& buffer = state.vertex_buffers[slot][sub];
        if (!buffer || !buffer->valid) continue;
        entry.vertex_buffer = buffer->physical_address;
        entry.vertex_buffer_dwords = buffer->size_dwords;
        entry.vertex_buffer_slot = static_cast<std::uint8_t>(slot * 3u + sub);
        break;
      }
    }
    if (entry.vertex_buffer && memory) {
      const auto count = (std::min)(std::uint32_t{12}, entry.vertex_buffer_dwords);
      std::array<std::byte, 48> bytes{};
      if (memory->copy_physical_range(entry.vertex_buffer & 0x1FFFFFFFu,
                                      std::span(bytes).first(count * 4u))) {
        for (std::uint32_t d = 0; d < count; ++d) {
          entry.vertex_words[d] =
              (std::uint32_t(std::to_integer<std::uint8_t>(bytes[d * 4])) << 24) |
              (std::uint32_t(std::to_integer<std::uint8_t>(bytes[d * 4 + 1])) << 16) |
              (std::uint32_t(std::to_integer<std::uint8_t>(bytes[d * 4 + 2])) << 8) |
              std::uint32_t(std::to_integer<std::uint8_t>(bytes[d * 4 + 3]));
        }
        entry.vertex_word_count = static_cast<std::uint8_t>(count);
      }
    }
    if (tracing()) trace("#" + std::to_string(trace_index) + " " + describe_draw(entry));
  }

  // XENON_GPU_TRACE=<width>x<height>: from the first color resolve of that
  // size on, the next XENON_GPU_TRACE_EVENTS (default 3000) draws and
  // resolves go in order to gpu_trace.log in the run's error folder, every
  // resolve with its byte lanes, so a frame that ends black shows the pass
  // where its picture was lost. Development only.
  //
  // XENON_GPU_TRACE_TRIGGER=<file>: a trace starts only at such a resolve
  // while <file> exists, and deletes it, so a trace can be taken of the
  // moment something is on screen (a mission, not the logos). Each trace goes
  // to its own gpu_trace_<n>.log, headed with the steady-clock time the
  // XENON_FRAME_DUMP images are named by; creating the file again takes
  // another.
  struct GpuTraceConfig {
    std::uint32_t width{}, height{}, events{};
    std::filesystem::path trigger{};
  };
  static const GpuTraceConfig& gpu_trace_config() {
    static const GpuTraceConfig config = [] {
      GpuTraceConfig parsed{};
      const char* value = std::getenv("XENON_GPU_TRACE");
      unsigned width{}, height{};
      if (!value || std::sscanf(value, "%ux%u", &width, &height) != 2) return parsed;
      const char* events = std::getenv("XENON_GPU_TRACE_EVENTS");
      parsed.width = width;
      parsed.height = height;
      parsed.events = events && *events ? std::uint32_t(std::strtoul(events, nullptr, 10)) : 3000u;
      if (const char* trigger = std::getenv("XENON_GPU_TRACE_TRIGGER"); trigger && *trigger)
        parsed.trigger = trigger;
      return parsed;
    }();
    return config;
  }
  [[nodiscard]] bool tracing() const noexcept { return trace_events_left != 0; }
  void trace(const std::string& line) {
    if (!trace_events_left) return;
    if (!trace_file) {
      const auto directory = xenon::logging::Logger::instance().error_directory();
      const auto name = gpu_trace_config().trigger.empty()
                            ? std::string("gpu_trace.log")
                            : "gpu_trace_" + std::to_string(traces_taken) + ".log";
      const auto path = (directory.empty() ? std::filesystem::path{}
                                           : std::filesystem::path(directory)) /
                        name;
      trace_file = std::make_unique<std::ofstream>(path, std::ios::trunc);
      const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
      *trace_file << "(trace starts at steady " << now_ms << " ms)\n";
    }
    *trace_file << line << '\n';
    ++trace_index;
    if (--trace_events_left == 0) {
      *trace_file << "(trace ends)\n";
      trace_file->flush();
      trace_file.reset();
      // With a trigger file, another trace is taken when it appears again.
      if (!gpu_trace_config().trigger.empty()) trace_started = false;
    }
  }
  void maybe_start_trace(bool depth, std::uint32_t width, std::uint32_t height) {
    const auto& config = gpu_trace_config();
    if (trace_started || depth || !config.events || width != config.width ||
        height != config.height)
      return;
    if (!config.trigger.empty()) {
      std::error_code error;
      if (!std::filesystem::exists(config.trigger, error)) return;
      std::filesystem::remove(config.trigger, error);
    }
    trace_started = true;
    ++traces_taken;
    trace_events_left = config.events;
  }
  // XENON_GPU_TRACE_IMAGES=1 (with XENON_GPU_TRACE): while a trace runs, the
  // destination of every colour resolve to an 8_8_8_8, 16_16_FLOAT,
  // 16_16_16_16_FLOAT or 32_FLOAT texture is also written as
  // trace<n>_<event>_<address>_<w>x<h>.bmp beside the trace, untiled and
  // endian-swapped as the guest would read it, so an intermediate render
  // target (a particle buffer, a picture-in-picture) can be looked at as the
  // frame built it. Float formats are scaled to the image's own range.
  // Development only: it waits for the GPU's copy into guest memory, which
  // the resolve had queued anyway.
  static bool trace_images_enabled() {
    static const bool enabled = [] {
      const char* value = std::getenv("XENON_GPU_TRACE_IMAGES");
      return value && *value == '1';
    }();
    return enabled;
  }
  [[nodiscard]] static float trace_half_to_float(std::uint16_t half) noexcept {
    const std::uint32_t sign = (half >> 15u) & 1u, exponent = (half >> 10u) & 31u,
                        mantissa = half & 1023u;
    float value;
    if (exponent == 0u) value = std::ldexp(static_cast<float>(mantissa), -24);
    else if (exponent == 31u) value = mantissa ? std::nanf("") : INFINITY;
    else value = std::ldexp(static_cast<float>(mantissa | 1024u), static_cast<int>(exponent) - 25);
    return sign ? -value : value;
  }
  void trace_resolve_image(const CopyResolveState& copy, const ResolveRectangle& rectangle) {
    if (!tracing() || !trace_images_enabled() || !memory || copy.destination_array) return;
    const auto format = copy.destination_format;
    if (format != 6u && format != 31u && format != 32u && format != 36u) return;
    const auto width = static_cast<std::uint32_t>(rectangle.right - rectangle.left);
    const auto height = static_cast<std::uint32_t>(rectangle.bottom - rectangle.top);
    // Intermediate targets only (up to 640x720): whole frames are what
    // XENON_FRAME_DUMP already saves, at 3.7 MB a resolve here.
    if (!width || !height || std::uint64_t(width) * height > 640u * 720u) return;
    if (!mirror.complete_cpu_visibility(true)) return;
    const auto bytes_per_pixel = format == 32u ? 8u : 4u;
    const auto pitch_aligned = (std::uint32_t(copy.destination_pitch) + 31u) & ~31u;
    // Each pixel's value as up to four floats, in the guest's component order.
    std::vector<std::array<float, 4>> pixels(std::size_t(width) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
      for (std::uint32_t x = 0; x < width; ++x) {
        const auto tiled = tiled_offset_2d(static_cast<std::uint32_t>(rectangle.left) + x,
                                           static_cast<std::uint32_t>(rectangle.top) + y,
                                           pitch_aligned, bytes_per_pixel);
        const auto address = std::uint64_t(copy.destination_base) + tiled;
        std::array<std::byte, 16> block{};
        if (address + bytes_per_pixel > memory::kPhysicalMemorySize ||
            !memory->copy_physical_range(static_cast<std::uint32_t>(address & ~15ull), block))
          return;
        apply_endian128(block, copy.destination_endian);
        const auto* p = block.data() + (address & 15ull);
        const auto u8 = [&](std::size_t i) { return std::to_integer<std::uint8_t>(p[i]); };
        const auto u16 = [&](std::size_t i) {
          return static_cast<std::uint16_t>(u8(i) | (u8(i + 1) << 8u));
        };
        auto& out = pixels[std::size_t(y) * width + x];
        if (format == 6u) {
          for (std::size_t c = 0; c < 4; ++c) out[c] = u8(c) / 255.0f;
        } else if (format == 31u) {
          out = {trace_half_to_float(u16(0)), trace_half_to_float(u16(2)), 0.0f, 1.0f};
        } else if (format == 32u) {
          for (std::size_t c = 0; c < 4; ++c) out[c] = trace_half_to_float(u16(c * 2));
        } else {
          std::uint32_t word = u8(0) | (u8(1) << 8u) | (u8(2) << 16u) | (std::uint32_t(u8(3)) << 24u);
          const auto value = std::bit_cast<float>(word);
          out = {value, value, value, 1.0f};
        }
      }
    }
    // Float formats: scale the finite colour values to 0..1 over the image.
    float low = 0.0f, high = 1.0f;
    if (format != 6u) {
      low = INFINITY;
      high = -INFINITY;
      for (const auto& pixel : pixels)
        for (std::size_t c = 0; c < 3; ++c)
          if (std::isfinite(pixel[c])) {
            low = (std::min)(low, pixel[c]);
            high = (std::max)(high, pixel[c]);
          }
      if (!(high > low)) {
        low = 0.0f;
        high = 1.0f;
      }
    }
    std::vector<std::uint8_t> bmp;
    const auto put16 = [&](std::uint32_t v) {
      bmp.push_back(static_cast<std::uint8_t>(v));
      bmp.push_back(static_cast<std::uint8_t>(v >> 8u));
    };
    const auto put32 = [&](std::uint32_t v) {
      put16(v);
      put16(v >> 16u);
    };
    const auto pixel_bytes = width * height * 4u;
    bmp.push_back('B');
    bmp.push_back('M');
    put32(54u + pixel_bytes);
    put32(0u);
    put32(54u);
    put32(40u);
    put32(width);
    put32(static_cast<std::uint32_t>(-static_cast<std::int32_t>(height)));  // top-down
    put16(1u);
    put16(32u);
    put32(0u);
    put32(pixel_bytes);
    put32(2835u);
    put32(2835u);
    put32(0u);
    put32(0u);
    const auto to_byte = [&](float v) {
      const float scaled = (v - low) / (high - low);
      return static_cast<std::uint8_t>(std::isfinite(scaled) ? std::clamp(scaled, 0.0f, 1.0f) * 255.0f + 0.5f
                                                             : 255.0f);
    };
    for (const auto& pixel : pixels) {
      bmp.push_back(to_byte(pixel[2]));
      bmp.push_back(to_byte(pixel[1]));
      bmp.push_back(to_byte(pixel[0]));
      bmp.push_back(255u);
    }
    const auto directory = xenon::logging::Logger::instance().error_directory();
    char name[96];
    std::snprintf(name, sizeof(name), "trace%u_%llu_%08X_%ux%u_f%u.bmp", traces_taken,
                  static_cast<unsigned long long>(trace_index), copy.destination_base, width,
                  height, unsigned(format));
    std::ofstream file((directory.empty() ? std::filesystem::path{}
                                          : std::filesystem::path(directory)) /
                           name,
                       std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bmp.data()), static_cast<std::streamsize>(bmp.size()));
  }
  std::unique_ptr<std::ofstream> trace_file{};
  std::uint32_t trace_events_left{};
  std::uint64_t trace_index{};
  std::uint32_t traces_taken{};
  bool trace_started{};

  void log_draw_trail() {
    const auto kept = (std::min)(draw_trail_count, kDrawTrailLength);
    auto& logger = xenon::logging::Logger::instance();
    logger.log(xenon::logging::Level::Warning, "gpu",
               "  the " + std::to_string(draw_trail_count) + " draw(s) before it" +
                   (draw_trail_count > kept ? " (last " + std::to_string(kept) + " shown)" : "") +
                   ":");
    for (std::size_t i = draw_trail_count - kept; i < draw_trail_count; ++i)
      logger.log(xenon::logging::Level::Warning, "gpu",
                 "  " + describe_draw(draw_trail[i % kDrawTrailLength]));
  }

  [[nodiscard]] static std::string describe_draw(const DrawTrailEntry& e) {
    char line[2048];
    int length = std::snprintf(
        line, sizeof(line),
        "draw vs=%016llX ps=%016llX prim=%u indices=%u color base=%u fmt=%u mask=0x%X%s%s "
        "depth base=%u fmt=%u test=%d write=%d func=%u stencil=%d alpha_test=%d pitch=%u "
        "msaa=%u scissor=(%d,%d)-(%d,%d) textures:",
        static_cast<unsigned long long>(e.vertex_shader),
        static_cast<unsigned long long>(e.pixel_shader), unsigned(e.primitive), e.index_count,
        unsigned(e.color_base), unsigned(e.color_format), unsigned(e.color_mask),
        e.color_enabled ? "" : " (disabled)", e.blend ? " blend" : "", unsigned(e.depth_base),
        unsigned(e.depth_format), int(e.depth_test), int(e.depth_write),
        unsigned(e.depth_function), int(e.stencil), int(e.alpha_test), unsigned(e.pitch),
        unsigned(e.msaa_log2), e.scissor[0], e.scissor[1], e.scissor[2], e.scissor[3]);
    for (std::uint8_t t = 0; t < e.texture_count && length > 0 &&
                             length < static_cast<int>(sizeof(line));
         ++t) {
      const auto& texture = e.textures[t];
      length += std::snprintf(line + length, sizeof(line) - static_cast<std::size_t>(length),
                              " [%u] fmt=%u %ux%u @%08X pitch=%u swz=0x%03X exp=%d signs=0x%02X "
                              "endian=%u tiled=%d",
                              unsigned(texture.slot), unsigned(texture.format), texture.width,
                              texture.height, texture.base_address, texture.pitch,
                              unsigned(texture.swizzle), int(texture.exp_adjust),
                              unsigned(texture.signs), unsigned(texture.endian),
                              int(texture.tiled));
    }
    if (length > 0 && length < static_cast<int>(sizeof(line))) {
      length += std::snprintf(line + length, sizeof(line) - static_cast<std::size_t>(length),
                              " viewport scale=(%g,%g,%g) offset=(%g,%g,%g) window_offset=%d(%d,%d)"
                              " drawable_bottom=%d vte=0x%X clip=0x%X mode=0x%X"
                              " blendctl=0x%08X colorctl=0x%08X",
                              double(e.viewport[0]), double(e.viewport[2]), double(e.viewport[4]),
                              double(e.viewport[1]), double(e.viewport[3]), double(e.viewport[5]),
                              int(e.window_offset), e.window_offset_xy[0], e.window_offset_xy[1],
                              e.drawable_bottom, unsigned(e.vte_control),
                              unsigned(e.clip_control), unsigned(e.mode_control),
                              unsigned(e.blend_control), unsigned(e.color_control));
    }
    for (std::uint8_t c = 0; c < e.ps_constant_count && length > 0 &&
                             length < static_cast<int>(sizeof(line));
         ++c) {
      const auto& v = e.ps_constant_value[c];
      length += std::snprintf(line + length, sizeof(line) - static_cast<std::size_t>(length),
                              " c%u=(%g,%g,%g,%g)", unsigned(e.ps_constant_index[c]),
                              double(v[0]), double(v[1]), double(v[2]), double(v[3]));
    }
    for (std::uint8_t c = 0; c < e.vs_constant_count && length > 0 &&
                             length < static_cast<int>(sizeof(line));
         ++c) {
      const auto& v = e.vs_constant_value[c];
      length += std::snprintf(line + length, sizeof(line) - static_cast<std::size_t>(length),
                              " vs c%u=(%g,%g,%g,%g)", unsigned(e.vs_constant_index[c]),
                              double(v[0]), double(v[1]), double(v[2]), double(v[3]));
    }
    if (e.vertex_buffer && length > 0 && length < static_cast<int>(sizeof(line))) {
      // The first 12 dwords as big-endian floats (8-in-32 is the usual
      // vertex endianness) as the draw saw them: for a rectangle list, its
      // three corners.
      length += std::snprintf(line + length, sizeof(line) - static_cast<std::size_t>(length),
                              " vb%u @%08X (%u dwords):", unsigned(e.vertex_buffer_slot),
                              e.vertex_buffer, e.vertex_buffer_dwords);
      for (std::uint8_t d = 0; d < e.vertex_word_count && length < static_cast<int>(sizeof(line));
           ++d) {
        length += std::snprintf(line + length, sizeof(line) - static_cast<std::size_t>(length),
                                " %g", double(std::bit_cast<float>(e.vertex_words[d])));
      }
    }
    return line;
  }

  // XENON_PROBE_LOGS only: full-frame (>= 1280x720) colour resolves go to
  // frontbuffer_diag.log with the route that produced them and a hash of the
  // guest bytes at their destination afterwards, on the steady clock the
  // presented-frame dump uses, so a stale presented frame can be traced to
  // the resolve (or its source image) that wrote it.
  void probe_full_frame_resolve(const char* route, std::uint32_t destination,
                                const ResolveRectangle& rectangle, std::uint64_t source_key,
                                std::uint32_t source_height) {
    if (!xenon::logging::probe_logs_enabled() || !memory) return;
    if (rectangle.right - rectangle.left < 1280 || rectangle.bottom - rectangle.top < 720) return;
    constexpr std::uint32_t kHashedBytes = 1280u * 720u * 4u;
    std::vector<std::byte> bytes(kHashedBytes);
    std::uint64_t hash = 0;
    if (memory->copy_physical_range(destination, bytes)) {
      hash = 1469598103934665603ull;
      for (std::size_t i = 0; i + 8u <= bytes.size(); i += 8u) {
        std::uint64_t word{};
        std::memcpy(&word, bytes.data() + i, 8u);
        hash = (hash ^ word) * 1099511628211ull;
      }
    }
    static std::atomic<std::uint64_t> sequence{};
    const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    xenon::logging::append_probe_log(
        "frontbuffer_diag.log",
        "t=%lld.%03lld resolve #%llu route=%s dest=0x%08X rect=(%d,%d)-(%d,%d) "
        "source=%016llX source_height=%u hash=%016llX\n",
        static_cast<long long>(now_us / 1000), static_cast<long long>(now_us % 1000),
        static_cast<unsigned long long>(sequence.fetch_add(1u) + 1u), route,
        static_cast<unsigned>(destination), rectangle.left, rectangle.top, rectangle.right,
        rectangle.bottom, static_cast<unsigned long long>(source_key), source_height,
        static_cast<unsigned long long>(hash));
  }

  // Counts a written resolve in the census; the first all-zero output of a
  // kind also goes to gpu.log, since a pass that resolves to black is where a
  // black frame with no failed draws loses its image.
  void note_resolve(bool depth, std::uint8_t source_format, std::uint8_t destination_format,
                    const ResolveRectangle& rectangle, bool all_zero,
                    std::string_view detail, std::uint32_t destination,
                    std::span<const std::byte> bytes = {}) {
    const auto width = static_cast<std::uint32_t>(rectangle.right - rectangle.left);
    const auto height = static_cast<std::uint32_t>(rectangle.bottom - rectangle.top);
    const auto describe = [&](std::string_view what) {
      return std::string(depth ? "depth" : "color") + " resolve of render-target format " +
             std::to_string(source_format) + " to texture format " +
             std::to_string(destination_format) + ", " + std::to_string(width) + "x" +
             std::to_string(height) + " at 0x" + hex_word(destination) + ", " +
             std::string(what) + " (" + std::string(detail) + ")";
    };
    // Which bytes of each 4-byte texel hold data: an image whose colour lanes
    // are all zero is black whatever its alpha says. Resolves are scanned
    // once in 16 (every one while tracing) to stay cheap: AC6 resolves dozens
    // of times a frame.
    std::array<std::size_t, 4> nonzero{};
    bool single_lane = false;
    maybe_start_trace(depth, width, height);
    const bool scanned = !all_zero && !bytes.empty() &&
                         (tracing() || (resolve_scan_counter++ % 16u) == 0u);
    if (scanned) {
      for (std::size_t i = 0; i < bytes.size(); ++i)
        if (bytes[i] != std::byte{}) ++nonzero[i % 4];
      single_lane = std::count_if(nonzero.begin(), nonzero.end(),
                                  [](std::size_t n) { return n != 0; }) == 1;
    }
    if (tracing()) {
      trace("#" + std::to_string(trace_index) + " " +
            describe(all_zero ? "only zeros"
                     : scanned
                         ? "byte lanes non-zero " + std::to_string(nonzero[0]) + "/" +
                               std::to_string(nonzero[1]) + "/" + std::to_string(nonzero[2]) +
                               "/" + std::to_string(nonzero[3]) + " of " +
                               std::to_string(bytes.size() / 4)
                         : "an image") +
            " from (" + std::to_string(rectangle.left) + "," + std::to_string(rectangle.top) +
            ")");
    }
    if (resolve_census.record(depth, source_format, destination_format, width, height,
                              all_zero, destination, single_lane)) {
      xenon::logging::Logger::instance().log(xenon::logging::Level::Warning, "gpu",
                                             describe("wrote only zeros (first of this kind)"));
      log_draw_trail();
    } else if (!all_zero && first_resolve_kinds_logged.size() < kFirstResolveKindsLogged &&
               first_resolve_kinds_logged
                   .insert((std::uint64_t{depth} << 63) ^ (std::uint64_t{source_format} << 56) ^
                           (std::uint64_t{destination_format} << 48) ^
                           (std::uint64_t{width} << 24) ^ height)
                   .second) {
      // A working pass of each kind, to compare a black one against.
      std::string lanes;
      if (scanned) {
        lanes = "; byte lanes non-zero " + std::to_string(nonzero[0]) + "/" +
                std::to_string(nonzero[1]) + "/" + std::to_string(nonzero[2]) + "/" +
                std::to_string(nonzero[3]) + " of " + std::to_string(bytes.size() / 4);
      }
      xenon::logging::Logger::instance().log(
          xenon::logging::Level::Warning, "gpu",
          describe("wrote an image (first of this kind" + lanes + ")"));
      log_draw_trail();
    }
  }
  static constexpr std::size_t kFirstResolveKindsLogged = 64;
  std::uint32_t resolve_scan_counter{};
  std::unordered_set<std::uint64_t> first_resolve_kinds_logged{};

  // XENON_SHADER_DUMP=<directory>: each guest shader, once, as
  // <hash>.<vs|ps>.hlsl - its microcode dwords as a comment, then the HLSL it
  // was translated to - so a pass that draws the wrong values can be read
  // next to the hash the draw trail names.
  void dump_shader(const ShaderProgram& program, std::string_view hlsl) {
    static const std::string directory = [] {
      const char* value = std::getenv("XENON_SHADER_DUMP");
      return std::string(value ? value : "");
    }();
    if (directory.empty() || !dumped_shaders.insert(program.hash()).second) return;
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    char name[48];
    std::snprintf(name, sizeof(name), "%016llX.%s.hlsl",
                  static_cast<unsigned long long>(program.hash()),
                  program.stage() == ShaderStage::Vertex ? "vs" : "ps");
    std::ofstream out(std::filesystem::path(directory) / name);
    out << "// Xenos microcode, " << program.dwords().size() << " dwords:\n//";
    std::size_t column = 0;
    for (const auto dword : program.dwords()) {
      out << ' ' << hex_word(dword);
      if (++column % 6 == 0) out << "\n//";
    }
    out << "\n\n" << hlsl;
  }
  std::unordered_set<std::uint64_t> dumped_shaders{};

  // Probe logs only (skinning_constants_diag.log): what a draw's vertex
  // shader reads when it addresses data it computes - constants indexed by an
  // address register, or vertex fetches indexed by a register other than the
  // vertex index (r0.x), which is how AC6 reads per-part transforms and
  // instance data. Logs the float constants and, for each such fetch, its
  // buffer's address, size and first elements as floats. Tells a contorted
  // model's cause apart: sane transforms there mean the shader reads them
  // wrong; garbage or stale ones mean the data never reached the GPU right.
  // Pair with XENON_SHADER_DUMP. The 1st, 16th and 256th draw of each of the
  // first eight such shaders.
  void probe_skinning_constants(const ir::ShaderReference& vertex_shader,
                                std::span<const std::byte> constants,
                                const auto& vertex_buffers) {
    if (!xenon::logging::probe_logs_enabled() || !vertex_shader.valid) return;
    const auto decoded = decoded_shaders.find(vertex_shader.hash);
    if (decoded == decoded_shaders.end()) return;
    std::vector<const VertexFetchInstruction*> computed;
    for (const auto& instruction : decoded->second.instructions) {
      if (instruction.kind != ShaderInstructionKind::VertexFetch) continue;
      const auto& fetch = instruction.vertex_fetch;
      if (fetch.mini_fetch || fetch.source_register != 0u || fetch.source_swizzle != 0u ||
          fetch.source_relative)
        computed.push_back(&fetch);
    }
    if (computed.empty() && !decoded->second.reflection.uses_dynamic_addressing) return;
    auto [entry, inserted] = skinning_probe_draws.try_emplace(vertex_shader.hash, 0u);
    if (inserted && skinning_probe_draws.size() > 8u) {
      skinning_probe_draws.erase(entry);
      return;
    }
    const auto draw_number = ++entry->second;
    if (draw_number != 1u && draw_number != 16u && draw_number != 256u) return;
    if (constants.size() < 256u * 16u) return;
    const auto log = [](const char* format, auto... values) {
      xenon::logging::append_probe_log("skinning_constants_diag.log", format, values...);
    };
    log("vs %016llX draw %u: float constants c0-c255\n",
        static_cast<unsigned long long>(vertex_shader.hash), unsigned(draw_number));
    for (std::uint32_t index = 0; index < 256u; ++index) {
      std::array<float, 4> value{};
      std::memcpy(value.data(), constants.data() + index * 16u, sizeof(value));
      log("  c%-3u % .6g % .6g % .6g % .6g\n", unsigned(index), value[0], value[1], value[2],
          value[3]);
    }
    for (const auto* fetch : computed) {
      const auto& buffer =
          vertex_buffers[fetch->fetch_constant & 31u][fetch->fetch_constant_select % 3u];
      log("  fetch %u.%u from r%u.%c%s%s stride %u offset %d format %u: ",
          unsigned(fetch->fetch_constant), unsigned(fetch->fetch_constant_select),
          unsigned(fetch->source_register), "xyzw"[fetch->source_swizzle & 3u],
          fetch->source_relative ? "+aL" : "", fetch->mini_fetch ? " (mini)" : "",
          unsigned(fetch->stride_dwords), int(fetch->offset_dwords),
          unsigned(fetch->data_format));
      if (!buffer || !buffer->valid) {
        log("no buffer\n");
        continue;
      }
      log("buffer 0x%08X, %u dwords, endian %u\n", buffer->physical_address,
          buffer->size_dwords, unsigned(buffer->endian));
      if (fetch->mini_fetch || !memory) continue;
      // The first four elements, each word swapped as the fetch swaps it.
      const auto stride = (std::max)(1u, std::uint32_t(fetch->stride_dwords));
      const auto words = (std::min)(buffer->size_dwords, stride * 4u);
      std::vector<std::byte> bytes(std::size_t(words) * 4u);
      if (!memory->copy_physical_range(buffer->physical_address, bytes)) continue;
      for (std::uint32_t element = 0; element * stride < words; ++element) {
        std::string line = "    [" + std::to_string(element) + "]";
        for (std::uint32_t word = 0; word < stride && element * stride + word < words; ++word) {
          std::uint32_t raw{};
          std::memcpy(&raw, bytes.data() + (std::size_t(element) * stride + word) * 4u, 4u);
          raw = gpu_swap(raw, buffer->endian);
          float value{};
          std::memcpy(&value, &raw, 4u);
          char text[24];
          std::snprintf(text, sizeof(text), " %.6g", value);
          line += text;
        }
        log("%s\n", line.c_str());
      }
    }
  }
  std::unordered_map<std::uint64_t, std::uint32_t> skinning_probe_draws{};

  // XENON_GPU_PROBE_FORMAT=<color render-target format>: the first few draws
  // into that format have their render target read back right after they are
  // recorded, so a pass that ends empty can be told apart as never drawn or
  // drawn and later lost. Development only; each probe waits for the GPU.
  template <typename Target>
  void probe_draw_output(const ir::DrawPacket& draw, std::uint8_t format,
                         std::size_t color_count, const std::array<Target*, 4>& targets,
                         auto* depth, std::uint8_t msaa_log2) {
    // During a GPU trace, the first multisampled draws have every host
    // sample of their color and depth targets read back, to tell a pass that
    // never rasterizes from one whose output is lost later.
    if (tracing() && msaa_log2 != 0 && trace_probes_left != 0) {
      --trace_probes_left;
      std::string line = "  probe after draw:";
      const auto count_nonzero = [](const auto& values) {
        return std::count_if(values.begin(), values.end(),
                             [](auto v) { return v != decltype(v){}; });
      };
      if (color_count && targets[0]) {
        const auto samples = 1u << static_cast<unsigned>(targets[0]->host_msaa());
        for (std::uint32_t s = 0; s < samples; ++s) {
          std::vector<std::byte> pixels;
          std::uint32_t pitch{};
          line += " color" + std::to_string(targets[0]->width()) + "x" +
                  std::to_string(targets[0]->height()) + " host sample " + std::to_string(s) + ": ";
          line += targets[0]->readback_native_sample(queue, s, 0, 0, targets[0]->width(),
                                                     targets[0]->height(), pixels, pitch)
                      ? std::to_string(count_nonzero(pixels)) + "/" + std::to_string(pixels.size()) +
                            " bytes non-zero"
                      : "readback failed";
        }
      }
      if (depth) {
        const auto samples = 1u << static_cast<unsigned>(depth->host_msaa());
        for (std::uint32_t s = 0; s < samples; ++s) {
          std::vector<std::uint32_t> words;
          std::uint32_t pitch{};
          line += " depth" + std::to_string(depth->width()) + "x" +
                  std::to_string(depth->height()) + " host sample " + std::to_string(s) + ": ";
          if (depth->readback_native_sample(queue, s, 0, 0, depth->width(), depth->height(), words,
                                            pitch)) {
            const auto [low, high] = std::minmax_element(words.begin(), words.end());
            line += std::to_string(count_nonzero(words)) + "/" + std::to_string(words.size()) +
                    " words non-zero, min 0x" + hex_word(words.empty() ? 0u : *low) +
                    " max 0x" + hex_word(words.empty() ? 0u : *high);
          } else {
            line += "readback failed";
          }
        }
      }
      if constexpr (requires { self().draw_statistics(); }) line += self().draw_statistics();
      trace(line);
    }
    static const int probe_format = [] {
      const char* value = std::getenv("XENON_GPU_PROBE_FORMAT");
      return value && *value ? std::atoi(value) : -1;
    }();
    if (probe_format < 0 || !color_count || !targets[0] || probes_left == 0) return;
    if (format != probe_format) return;
    --probes_left;
    std::vector<std::byte> pixels;
    std::uint32_t pitch{};
    const auto width = (std::min)(targets[0]->width(), 64u);
    const auto height = (std::min)(targets[0]->height(), 64u);
    std::string result;
    if (targets[0]->readback_sample(queue, 0, 0, 0, width, height, pixels, pitch)) {
      const auto nonzero = std::count_if(pixels.begin(), pixels.end(),
                                         [](std::byte b) { return b != std::byte{}; });
      result = std::to_string(nonzero) + " of " + std::to_string(pixels.size()) +
               " bytes non-zero";
    } else {
      result = "readback failed";
    }
    char shaders[192];
    std::snprintf(shaders, sizeof(shaders),
                  "vs=%016llX ps=%016llX indices=%u source=%u VGT offset=0x%X min=0x%X max=0x%X",
                  static_cast<unsigned long long>(draw.vertex_shader.hash),
                  static_cast<unsigned long long>(draw.pixel_shader.hash), draw.index_count,
                  static_cast<unsigned>(draw.source), resource_state.register_value(0x2102u),
                  resource_state.register_value(0x2101u), resource_state.register_value(0x2100u));
    xenon::logging::Logger::instance().log(
        xenon::logging::Level::Warning, "gpu",
        "post-draw probe, render-target format " + std::to_string(probe_format) + " " +
            std::to_string(width) + "x" + std::to_string(height) + " (" + shaders +
            "): " + result);
  }
  std::uint32_t probes_left{64};
  std::uint32_t trace_probes_left{6};

  // During a GPU trace, the first multisampled draws also report the vertex
  // data their shader fetches over the index range they draw: for each fetch,
  // its first vertex and how many of the range's dwords are non-zero, so a
  // draw whose triangles all vanish shows whether its inputs were empty.
  void trace_vertex_inputs(const ir::DrawPacket& draw, const DrawResourceState& state) {
    if (!tracing() || state.raster.msaa_samples_log2 == 0 || vertex_probes_left == 0 || !memory)
      return;
    --vertex_probes_left;
    const auto decoded = decoded_shaders.find(draw.vertex_shader.hash);
    if (decoded == decoded_shaders.end()) return;
    std::uint32_t low = 0, high = draw.index_count ? draw.index_count - 1u : 0u;
    const auto& indices = draw.index_buffer;
    if (indices.valid && indices.length_bytes) {
      const auto bytes_per_index = indices.format == IndexFormat::UInt32 ? 4u : 2u;
      std::vector<std::byte> raw((std::min)(indices.length_bytes, draw.index_count * bytes_per_index));
      if (!memory->copy_physical_range(indices.physical_address & 0x1FFFFFFFu, raw)) return;
      low = 0xFFFFFFFFu;
      high = 0;
      for (std::size_t i = 0; i + bytes_per_index <= raw.size(); i += bytes_per_index) {
        std::uint32_t value = 0;
        for (std::uint32_t b = 0; b < bytes_per_index; ++b)
          value = (value << 8u) | std::to_integer<std::uint32_t>(raw[i + b]);
        if (bytes_per_index == 4u && indices.endian == Endian::Swap16In32)
          value = (value >> 16u) | (value << 16u);
        low = (std::min)(low, value);
        high = (std::max)(high, value);
      }
    }
    const auto offset = resource_state.register_value(0x2102u);
    std::string line = "  vertex inputs: indices " + std::to_string(low) + ".." +
                       std::to_string(high) + " + offset " + std::to_string(offset);
    std::unordered_set<std::uint32_t> seen;
    for (const auto& instruction : decoded->second.instructions) {
      if (instruction.kind != ShaderInstructionKind::VertexFetch) continue;
      const auto& fetch = instruction.vertex_fetch;
      if (fetch.mini_fetch ||
          !seen.insert((std::uint32_t(fetch.fetch_constant) << 24u) |
                       (std::uint32_t(fetch.fetch_constant_select) << 16u) |
                       std::uint32_t(fetch.offset_dwords & 0xFFFF)).second)
        continue;
      const auto& buffer =
          state.vertex_buffers[fetch.fetch_constant & 31u][fetch.fetch_constant_select % 3u];
      line += " | fetch " + std::to_string(fetch.fetch_constant) + "." +
              std::to_string(fetch.fetch_constant_select) + " +" +
              std::to_string(fetch.offset_dwords) + " format " + std::to_string(fetch.data_format) +
              " stride " + std::to_string(fetch.stride_dwords);
      if (!buffer || !buffer->valid) {
        line += ": no buffer";
        continue;
      }
      const auto stride = (std::max)(1u, std::uint32_t(fetch.stride_dwords));
      const auto first = std::uint64_t(low + offset) * stride + std::uint32_t(fetch.offset_dwords);
      const auto count = (std::min)(std::uint64_t(high - low + 1u) * stride, std::uint64_t{65536});
      if (first + count > buffer->size_dwords) {
        line += ": range past the buffer (" + std::to_string(buffer->size_dwords) + " dwords)";
        continue;
      }
      std::vector<std::byte> words(static_cast<std::size_t>(count) * 4u);
      if (!memory->copy_physical_range(
              static_cast<std::uint32_t>((buffer->physical_address & 0x1FFFFFFFu) + first * 4u),
              words)) {
        line += ": unreadable";
        continue;
      }
      const auto nonzero = std::count_if(words.begin(), words.end(),
                                         [](std::byte b) { return b != std::byte{}; });
      line += " @" + hex_word(buffer->physical_address) + ": " + std::to_string(nonzero) + "/" +
              std::to_string(words.size()) + " bytes non-zero, first";
      for (std::size_t d = 0; d < 4 && d * 4u + 3u < words.size(); ++d) {
        const auto word = (std::to_integer<std::uint32_t>(words[d * 4]) << 24u) |
                          (std::to_integer<std::uint32_t>(words[d * 4 + 1]) << 16u) |
                          (std::to_integer<std::uint32_t>(words[d * 4 + 2]) << 8u) |
                          std::to_integer<std::uint32_t>(words[d * 4 + 3]);
        char value[48];
        std::snprintf(value, sizeof(value), " %08X(%g)", static_cast<unsigned>(word),
                      double(std::bit_cast<float>(word)));
        line += value;
      }
    }
    trace(line);
  }
  std::uint32_t vertex_probes_left{6};

  [[nodiscard]] static std::string hex_word(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "%08X", static_cast<unsigned>(value));
    return text;
  }

  // The native depth image a resolve's depth clear is drawn into: the depth
  // target of the resolved surface itself (as the resolve picks its source)
  // when it owns every tile the region covers - it holds the region by
  // construction - otherwise a single image holding all of those tiles whole.
  DepthTargetImage* depth_clear_owner(const DrawResourceState& state,
                                      const EdramSurfaceLayout& surface,
                                      const EdramSurfaceRegion& region) const {
    const auto tiles = covered_edram_tiles(surface, region);
    if (tiles.empty()) return nullptr;
    const auto format = static_cast<DepthRenderTargetFormat>(state.depth_target.format);
    for (const auto& [key, candidate] : depth_targets) {
      const auto& candidate_surface = candidate->surface();
      if (candidate_surface.base_tile != surface.base_tile ||
          candidate_surface.pitch_pixels != surface.pitch_pixels ||
          candidate_surface.msaa != surface.msaa || candidate->guest_format() != format ||
          candidate->height() < static_cast<std::uint32_t>(region.bottom))
        continue;
      const auto owner = depth_target_owners.find(key);
      if (owner == depth_target_owners.end()) continue;
      if (std::all_of(tiles.begin(), tiles.end(), [&](std::uint16_t tile) {
            return edram_ownership.owner(tile) == owner->second;
          }))
        return candidate.get();
    }
    return single_depth_owner(surface, region);
  }

  bool clear_depth_resolve_region(const DrawResourceState& state,
                                  const ResolveRectangle& rectangle,
                                  MsaaSamples samples) {
    if (!state.copy.depth_clear_enabled) return true;
    if (!state.raster.surface_pitch || rectangle.bottom <= 0) {
      error = named("Xenos post-resolve depth clear has an invalid surface extent");
      return false;
    }
    const EdramSurfaceLayout surface{
        state.depth_target.base_tile, state.raster.surface_pitch,
        static_cast<std::uint32_t>(rectangle.bottom), samples, false, true};
    const EdramSurfaceRegion region{rectangle.left, rectangle.top,
                                     rectangle.right, rectangle.bottom};
    // A native depth image owning the rectangle's tiles whole takes the clear
    // as a draw, on the GPU; flushing it to canonical EDRAM, clearing there
    // and loading it back cost AC6 tens of milliseconds per clearing resolve.
    if (auto* owner = depth_clear_owner(state, surface, region)) {
      GpuClearConstants constants{};
      std::uint8_t stencil{};
      if (plan_gpu_depth_clear(rectangle, state.copy.depth_clear,
                               static_cast<DepthRenderTargetFormat>(state.depth_target.format),
                               surface, owner->surface(), owner->host_msaa(),
                               owner->guest_format(), owner->unorm24_depth_write(),
                               constants, stencil)) {
        if (!self().gpu_depth_clear(*owner, constants, stencil)) return false;
        ++performance.gpu_depth_clears;
        return true;
      }
    }
    if (!make_region_canonical(surface, region)) return false;
    if (!clear_edram_surface_region(*edram, surface, rectangle.left,
                                    rectangle.top, rectangle.right,
                                    rectangle.bottom,
                                    {state.copy.depth_clear, 0u})) {
      error = named("regional depth resolve clear failed");
      return false;
    }
    edram_ownership.make_canonical_region(surface, region);
    return true;
  }

  // Submits the open batch when its share of the transient upload arena
  // cannot take `index_byte_size` more bytes, so the draw opens a new batch
  // (with an empty arena) instead of failing. Called before record_draw().
  bool make_transient_upload_room(std::size_t index_byte_size) {
    const auto frame = queue.open_frame();
    if (frame == UINT32_MAX || !index_byte_size ||
        transient_arenas[frame].fits(queue.retirement_value(), index_byte_size))
      return true;
    ++performance.transient_upload_flushes;
    if (!queue.flush()) {
      error = queue.error();
      return false;
    }
    return true;
  }

  // Places an indexed batch's indices where the draw reads them: a cached
  // batch in the GPU-resident buffer (`resident` set; copied there on its
  // first draw only), anything else - or a cached batch the resident buffer
  // has no room for - in its command batch's transient upload arena. Returns
  // the offset in the chosen buffer. Called from inside record_draw(), while
  // the batch is open.
  bool upload_draw_indices(const DrawRecording& draw, std::uint32_t frame_index,
                           std::size_t& offset, bool& resident) {
    resident = false;
    if (!draw.batch.indexed) return true;
    if (draw.batch.resident_key && !resident_index_mapping.empty()) {
      auto found = resident_ranges.find(draw.batch.resident_key);
      if (found == resident_ranges.end()) {
        resident_allocator.reclaim(queue.completed_value());
        if (const auto placed = resident_allocator.allocate(draw.index_byte_size)) {
          std::memcpy(resident_index_mapping.data() + *placed, draw.batch.indices.data(),
                      draw.index_byte_size);
          perf::add(perf::Counter::ResidentIndexBytes, draw.index_byte_size);
          found = resident_ranges.emplace(draw.batch.resident_key,
                                          ResidentRange{*placed, draw.index_byte_size})
                      .first;
        }
      }
      if (found != resident_ranges.end()) {
        perf::add(perf::Counter::ResidentIndexDraws, 1u);
        offset = static_cast<std::size_t>(found->second.offset);
        resident = true;
        return true;
      }
      // No room: this draw uploads them as an uncached one would.
    }
    const auto allocation = transient_arenas[frame_index].allocate(
        queue.retirement_value(), draw.index_byte_size);
    if (!allocation) {
      error = named("transient upload arena exhausted by one draw batch");
      return false;
    }
    std::memcpy(transient_upload_mappings[frame_index].data() + *allocation,
                draw.batch.indices.data(), draw.index_byte_size);
    offset = *allocation;
    return true;
  }

 private:
  Derived& self() { return static_cast<Derived&>(*this); }
  const Derived& self() const { return static_cast<const Derived&>(*this); }
  void consume_resolve(const DrawResourceState& state);
  void consume_draw(const ir::DrawPacket* draw, const DrawResourceState& state);
  void consume_shader_load(const ir::ShaderLoad* load);
};

}  // namespace xenon::gpu::detail

#include "graphics/common/backend_core_consume.hpp"
#include "graphics/common/backend_core_draw.hpp"
#include "graphics/common/backend_core_submission.hpp"

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
#include <memory>
#include <mutex>
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
#include "xenon/gpu/edram_surface.hpp"
#include "xenon/gpu/ir.hpp"
#include "xenon/gpu/presentation.hpp"
#include "xenon/gpu/primitive_processor.hpp"
#include "xenon/gpu/resource_ir.hpp"
#include "xenon/gpu/gpu_resolve.hpp"
#include "xenon/gpu/guest_read_sync.hpp"
#include "xenon/gpu/scanout.hpp"
#include "xenon/gpu/texture.hpp"
#include "xenon/gpu/transient_arena.hpp"
#include "xenon/gpu/blend_constant.hpp"
#include "xenon/logging/logger.hpp"
#include "xenon/memory/address_space.hpp"
#ifdef XENON_HAS_DXC
#include "xenon/gpu/dxc_shader_compiler.hpp"
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
  //   <root object> pipeline_root();
  //   std::uint32_t color_render_width(const DrawResourceState&, const RenderTargetImage*);
  //   std::uint32_t color_render_height(const DrawResourceState&, const RenderTargetImage*);
  //   bool record_draw(const DrawRecording&, bool& record_failed);
  //   bool gpu_color_resolve(RenderTargetImage&, const GpuResolveConstants&);
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
  bool invalidate_edram_native_state();
  PresentStatus present(const PresentationFrame& frame);
  // Plans scan-out of `frame` to a width x height target and makes its source
  // bytes current in the mirror.
  bool prepare_scanout(const PresentationFrame& frame, std::uint32_t width,
                       std::uint32_t height, bool srgb_target, ScanoutPlan& plan);
  // Scan-out drawn on the GPU into an offscreen image and read back.
  bool capture_scanout(const PresentationFrame& frame, std::uint32_t width,
                       std::uint32_t height, PreparedPresentationFrame& output);
  bool resize_presentation(std::uint32_t width, std::uint32_t height);
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
  std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> shader_textures{};
  TextureDirtyTracker texture_dirty{};
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

  void collect_retired_resources() {
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
    const auto& surface = image.surface();
    const auto sample_count = 1u << static_cast<unsigned>(surface.msaa);
    TransferTiming timing{"depth flush", {"readback", "-", "store"},
                          image.width(), image.height(), sample_count,
                          static_cast<std::uint32_t>(image.guest_format())};
    for (const auto& region : regions) {
      const auto left = std::uint32_t(region.left), top = std::uint32_t(region.top);
      const auto width = std::uint32_t(region.right - region.left);
      const auto height = std::uint32_t(region.bottom - region.top);
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

  bool canonical_to_color(RenderTargetImage& image,
                          std::span<const EdramSurfaceRegion> regions) {
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
    auto plan = edram_ownership.plan(requested_owner, ownership_surface);
    if (!plan.valid || plan.changes.empty()) return plan.valid;
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
  struct GpuTraceConfig {
    std::uint32_t width{}, height{}, events{};
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
      return parsed;
    }();
    return config;
  }
  [[nodiscard]] bool tracing() const noexcept { return trace_events_left != 0; }
  void trace(const std::string& line) {
    if (!trace_events_left) return;
    if (!trace_file) {
      const auto directory = xenon::logging::Logger::instance().error_directory();
      const auto path = (directory.empty() ? std::filesystem::path{}
                                           : std::filesystem::path(directory)) /
                        "gpu_trace.log";
      trace_file = std::make_unique<std::ofstream>(path, std::ios::trunc);
    }
    *trace_file << line << '\n';
    ++trace_index;
    if (--trace_events_left == 0) {
      *trace_file << "(trace ends)\n";
      trace_file->flush();
    }
  }
  void maybe_start_trace(bool depth, std::uint32_t width, std::uint32_t height) {
    const auto& config = gpu_trace_config();
    if (trace_started || depth || !config.events || width != config.width ||
        height != config.height)
      return;
    trace_started = true;
    trace_events_left = config.events;
  }
  std::unique_ptr<std::ofstream> trace_file{};
  std::uint32_t trace_events_left{};
  std::uint64_t trace_index{};
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
                              " drawable_bottom=%d vte=0x%X clip=0x%X mode=0x%X",
                              double(e.viewport[0]), double(e.viewport[2]), double(e.viewport[4]),
                              double(e.viewport[1]), double(e.viewport[3]), double(e.viewport[5]),
                              int(e.window_offset), e.window_offset_xy[0], e.window_offset_xy[1],
                              e.drawable_bottom, unsigned(e.vte_control),
                              unsigned(e.clip_control), unsigned(e.mode_control));
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
                               owner->guest_format(), constants, stencil)) {
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

  // Copies an indexed batch into its command batch's transient upload arena
  // and returns the 4-byte aligned offset it was written at. Called from
  // inside record_draw(), while the batch is open.
  bool upload_draw_indices(const DrawRecording& draw, std::uint32_t frame_index,
                           std::size_t& offset) {
    if (!draw.batch.indexed) return true;
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

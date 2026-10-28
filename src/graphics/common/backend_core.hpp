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
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "xenon/gpu/backend.hpp"
#include "xenon/logging/probe_log.hpp"
#include "xenon/gpu/edram.hpp"
#include "xenon/gpu/edram_ownership.hpp"
#include "xenon/gpu/edram_surface.hpp"
#include "xenon/gpu/ir.hpp"
#include "xenon/gpu/presentation.hpp"
#include "xenon/gpu/primitive_processor.hpp"
#include "xenon/gpu/resource_ir.hpp"
#include "xenon/gpu/gpu_resolve.hpp"
#include "xenon/gpu/scanout.hpp"
#include "xenon/gpu/texture.hpp"
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
  //   bool create_texture(TextureImage&, const DecodedTexture&);
  //   bool bind_texture(std::uint32_t slot, const TextureDescriptor&, TextureImage&);
  //   <root object> pipeline_root();
  //   std::uint32_t color_render_width(const DrawResourceState&, const RenderTargetImage*);
  //   std::uint32_t color_render_height(const DrawResourceState&, const RenderTargetImage*);
  //   bool record_draw(const DrawRecording&, bool& record_failed);
  //   bool gpu_color_resolve(RenderTargetImage&, const GpuResolveConstants&);
  //   bool gpu_color_clear(RenderTargetImage&, const GpuClearConstants&);
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
  };

  void begin_submission(memory::AddressSpace& new_memory, Edram& new_edram);
  void consume(const ir::Command& command);
  void end_submission();
  bool make_guest_memory_cpu_visible(std::uint32_t physical_address,
                                     std::uint32_t size);
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
  std::array<std::size_t, CommandQueue::kFrameCount> transient_upload_offsets{};
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
  memory::AddressSpace* memory{};
  Edram* edram{};
  EdramOwnershipTracker edram_ownership{};
  EdramOwnerId next_edram_owner{1};
  std::size_t command_count{};
  std::size_t draw_count{};
  std::size_t compiled_shader_count{};
  GpuPerformanceCounters performance{};
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
      if (!direct_change[i])
        incoming.insert(incoming.end(), change.tiles.begin(), change.tiles.end());
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

  // A color image of the same format holding the tiles whole hands them over
  // on the GPU, sample by sample through the EDRAM address mapping, instead
  // of a readback, conversion and upload through canonical storage.
  bool direct_color_transfer_supported(EdramOwnerId previous, const RenderTargetImage& requested,
                                       std::span<const std::uint16_t> tiles) const {
    if constexpr (!Api::kSupportsDirectColorTransfer) return false;
    const auto found = owner_render_targets.find(previous);
    if (found == owner_render_targets.end() || !found->second) return false;
    const auto& source = *found->second;
    return &source != &requested && source.guest_format() == requested.guest_format() &&
           edram_direct_transfer_supported(source.surface(), requested.surface(), tiles);
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

  bool direct_color_transfer(EdramOwnerId previous, RenderTargetImage& requested,
                             std::span<const std::uint16_t> tiles) {
    if constexpr (!Api::kSupportsDirectColorTransfer) {
      error = named("direct EDRAM color transfer is unavailable on this backend");
      return false;
    } else {
      auto& source = *owner_render_targets.at(previous);
      std::vector<EdramSurfaceRegion> regions;
      if (!edram_tile_regions(requested.surface(), tiles, regions)) {
        error = named("direct EDRAM transfer tiles are not whole pixels");
        return false;
      }
      const auto constants = make_edram_transfer_constants(
          source.surface(), source.host_msaa(), requested.surface(), requested.host_msaa());
      if (!self().transfer_color_tiles(source, requested, constants, regions)) return false;
      edram_ownership.release_tiles(previous, tiles);
      ++performance.direct_edram_transfers;
      return true;
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
    // Depth moves through canonical storage until it has a direct transfer.
    return acquire_ownership(
        owner_it->second, requested, ownership_surface, &BackendCore::canonical_to_depth,
        &BackendCore::flush_depth_owner, "depth EDRAM ownership plan became stale",
        [](EdramOwnerId, std::span<const std::uint16_t>) { return false; },
        [](EdramOwnerId, std::span<const std::uint16_t>) { return false; });
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

  // Copies an indexed batch into this frame's transient upload arena, which
  // restarts on the frame's first draw, and returns the 4-byte aligned offset
  // it was written at. Called from inside record_draw().
  bool upload_draw_indices(const DrawRecording& draw, std::uint32_t frame_index,
                           std::uint32_t draw_slot, std::size_t& offset) {
    if (draw_slot == 0) transient_upload_offsets[frame_index] = 0;
    if (!draw.batch.indexed) return true;
    const auto aligned_offset =
        (transient_upload_offsets[frame_index] + 3u) & ~std::size_t{3u};
    if (draw.index_byte_size >
        transient_upload_mappings[frame_index].size() -
            (std::min)(aligned_offset,
                       transient_upload_mappings[frame_index].size())) {
      error = named("transient upload arena exhausted by one draw batch");
      return false;
    }
    std::memcpy(transient_upload_mappings[frame_index].data() + aligned_offset,
                draw.batch.indices.data(), draw.index_byte_size);
    transient_upload_offsets[frame_index] =
        aligned_offset + draw.index_byte_size;
    offset = aligned_offset;
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

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>

#include <vulkan/vulkan.h>

#include "xenon/gpu/edram_surface.hpp"
#include "xenon/gpu/shader_translation.hpp"
#include "xenon/gpu/vulkan/command_queue.hpp"
#include "xenon/gpu/vulkan/depth_target.hpp"
#include "xenon/gpu/vulkan/render_target.hpp"

namespace xenon::gpu::vulkan {

// The descriptor-set and pipeline layouts of the transfer shaders for each
// kind of source image: a color source is one sampled image at binding 16
// (RenderTargetImage::kSampledSourceStages), a depth source its depth and
// stencil at bindings 16 and 17 - the layouts the images' own cached
// sampled_descriptor_set() is allocated against, so every user must define
// them identically. Push constants are `constant_bytes` for the fragment
// stage.
class EdramTransferLayouts {
 public:
  explicit EdramTransferLayouts(std::uint32_t constant_bytes) noexcept
      : constant_bytes_(constant_bytes) {}
  ~EdramTransferLayouts();
  EdramTransferLayouts(const EdramTransferLayouts&) = delete;
  EdramTransferLayouts& operator=(const EdramTransferLayouts&) = delete;

  // Creates both kinds on first use. False with `error` set on failure.
  [[nodiscard]] bool create(VkDevice device, std::string& error);
  [[nodiscard]] VkDescriptorSetLayout set_layout(bool depth_source) const noexcept {
    return set_layouts_[depth_source ? 1 : 0];
  }
  [[nodiscard]] VkPipelineLayout pipeline_layout(bool depth_source) const noexcept {
    return pipeline_layouts_[depth_source ? 1 : 0];
  }

 private:
  VkDevice device_{VK_NULL_HANDLE};
  std::uint32_t constant_bytes_{};
  std::array<VkDescriptorSetLayout, 2> set_layouts_{};
  std::array<VkPipelineLayout, 2> pipeline_layouts_{};
};

// Moves EDRAM tiles directly from one native color image to another of the
// same format, on the GPU: a sample-frequency draw into the destination that
// loads, for each sample, the source sample holding the same EDRAM word. It is
// recorded into the queue's open batch like a draw, with no CPU round trip.
// Pipelines are built once per format and sample-count pair.
//
// `convert` does the same from an image of another view - a color image of a
// different format, or a depth image - when edram_converting_transfer_supported()
// accepts the pair: each sample's EDRAM word is packed from the source's host
// value and unpacked as the destination's format.
class EdramColorTransfer {
 public:
  EdramColorTransfer() = default;
  ~EdramColorTransfer();
  EdramColorTransfer(const EdramColorTransfer&) = delete;
  EdramColorTransfer& operator=(const EdramColorTransfer&) = delete;

  [[nodiscard]] bool transfer(VkDevice device, CommandQueue& queue, RenderTargetImage& source,
                              RenderTargetImage& destination,
                              const EdramTransferConstants& constants,
                              std::span<const EdramSurfaceRegion> regions);
  [[nodiscard]] bool convert(VkDevice device, CommandQueue& queue, RenderTargetImage& source,
                             RenderTargetImage& destination,
                             const EdramTransferConstants& constants,
                             std::span<const EdramSurfaceRegion> regions);
  [[nodiscard]] bool convert(VkDevice device, CommandQueue& queue, DepthTargetImage& source,
                             RenderTargetImage& destination,
                             const EdramTransferConstants& constants,
                             std::span<const EdramSurfaceRegion> regions);
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  // `converting` selects the converting shader for `source`.
  [[nodiscard]] VkPipeline pipeline(VkDevice device, EdramTransferEndpoint source,
                                    MsaaSamples source_host, const RenderTargetImage& destination,
                                    bool converting);
  template <typename Source>
  [[nodiscard]] bool run(VkDevice device, CommandQueue& queue, Source& source,
                         RenderTargetImage& destination, bool converting,
                         const EdramTransferConstants& constants,
                         std::span<const EdramSurfaceRegion> regions);

  VkDevice device_{VK_NULL_HANDLE};
  EdramTransferLayouts layouts_{sizeof(EdramTransferConstants)};
  std::unordered_map<std::uint64_t, VkPipeline> pipelines_{};
  std::string error_{};
};

// The same direct transfer between two native depth images of one format.
// Depth and stencil move together in one pass where VK_EXT_shader_stencil_
// export is enabled; otherwise depth moves first (stencil replaced with
// zero) and each stencil bit follows in its own pass, still with no CPU
// round trip. `convert` takes a source of another view, as
// EdramColorTransfer's does.
class EdramDepthTransfer {
 public:
  EdramDepthTransfer() = default;
  ~EdramDepthTransfer();
  EdramDepthTransfer(const EdramDepthTransfer&) = delete;
  EdramDepthTransfer& operator=(const EdramDepthTransfer&) = delete;

  [[nodiscard]] bool transfer(VkDevice device, VkPhysicalDevice physical_device,
                              CommandQueue& queue, DepthTargetImage& source,
                              DepthTargetImage& destination,
                              const EdramTransferConstants& constants,
                              std::span<const EdramSurfaceRegion> regions);
  [[nodiscard]] bool convert(VkDevice device, VkPhysicalDevice physical_device,
                             CommandQueue& queue, DepthTargetImage& source,
                             DepthTargetImage& destination,
                             const EdramTransferConstants& constants,
                             std::span<const EdramSurfaceRegion> regions);
  [[nodiscard]] bool convert(VkDevice device, VkPhysicalDevice physical_device,
                             CommandQueue& queue, RenderTargetImage& source,
                             DepthTargetImage& destination,
                             const EdramTransferConstants& constants,
                             std::span<const EdramSurfaceRegion> regions);
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  [[nodiscard]] VkPipeline pipeline(VkDevice device, EdramTransferEndpoint source,
                                    MsaaSamples source_host, const DepthTargetImage& destination,
                                    bool converting, EdramDepthTransferPass pass);
  template <typename Source>
  [[nodiscard]] bool run(VkDevice device, VkPhysicalDevice physical_device, CommandQueue& queue,
                         Source& source, DepthTargetImage& destination, bool converting,
                         const EdramTransferConstants& constants,
                         std::span<const EdramSurfaceRegion> regions);

  VkDevice device_{VK_NULL_HANDLE};
  EdramTransferLayouts layouts_{sizeof(EdramDepthTransferConstants)};
  std::unordered_map<std::uint64_t, VkPipeline> pipelines_{};
  int stencil_export_{-1};
  std::string error_{};
};

}  // namespace xenon::gpu::vulkan

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>

#include <vulkan/vulkan.h>

#include "xenon/gpu/edram_surface.hpp"
#include "xenon/gpu/vulkan/command_queue.hpp"
#include "xenon/gpu/vulkan/render_target.hpp"

namespace xenon::gpu::vulkan {

// Moves EDRAM tiles directly from one native color image to another of the
// same format, on the GPU: a sample-frequency draw into the destination that
// loads, for each sample, the source sample holding the same EDRAM word. It is
// recorded into the queue's open batch like a draw, with no CPU round trip.
// Pipelines are built once per format and sample-count pair.
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
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  [[nodiscard]] bool create_layouts(VkDevice device);
  [[nodiscard]] VkPipeline pipeline(VkDevice device, VkFormat format, MsaaSamples source_host,
                                    MsaaSamples destination_host);

  VkDevice device_{VK_NULL_HANDLE};
  VkDescriptorSetLayout set_layout_{VK_NULL_HANDLE};
  VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
  std::unordered_map<std::uint64_t, VkPipeline> pipelines_{};
  std::string error_{};
};

}  // namespace xenon::gpu::vulkan

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

#include <vulkan/vulkan.h>

#include "xenon/gpu/edram_surface.hpp"
#include "xenon/gpu/primitive_processor.hpp"
#include "xenon/gpu/resource_ir.hpp"
#include "xenon/gpu/shader_translation.hpp"

namespace xenon::gpu::vulkan {

class GraphicsPipeline {
 public:
  // The VkPipelineCache initialize() creates through (the backend's
  // persistent cache); none by default.
  void set_pipeline_cache(VkPipelineCache cache) noexcept { cache_ = cache; }
  GraphicsPipeline() = default;
  ~GraphicsPipeline();
  GraphicsPipeline(const GraphicsPipeline&) = delete;
  GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;

  [[nodiscard]] bool initialize(VkDevice device, VkPipelineLayout layout,
                                const CompiledShader& vertex_shader,
                                const CompiledShader& pixel_shader,
                                const CompiledShader* geometry_shader,
                                std::span<const VkFormat> color_formats,
                                MsaaSamples samples,
                                HostPrimitiveTopology topology,
                                const RasterState& raster,
                                std::span<const std::uint8_t> color_write_masks,
                                std::span<const BlendState> blend_states,
                                VkFormat depth_format = VK_FORMAT_UNDEFINED,
                                const DepthTargetDescriptor* depth_state = nullptr);
  void reset() noexcept;
  [[nodiscard]] VkPipeline pipeline() const noexcept { return pipeline_; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }
  // Whether the driver reported that the pipeline cache supplied this
  // pipeline (pipeline creation feedback, core in Vulkan 1.3); false when it
  // compiled it or reports nothing. The D3D12 pipeline's equivalent.
  [[nodiscard]] bool loaded_from_library() const noexcept { return loaded_from_library_; }

 private:
  VkPipelineCache cache_{VK_NULL_HANDLE};
  VkDevice device_{VK_NULL_HANDLE};
  VkPipeline pipeline_{VK_NULL_HANDLE};
  bool loaded_from_library_{};
  std::string error_{};
};

}  // namespace xenon::gpu::vulkan

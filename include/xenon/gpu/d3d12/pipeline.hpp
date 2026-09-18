#pragma once

#include <cstdint>
#include <span>
#include <string>

#include <d3d12.h>
#include <dxgiformat.h>
#include <wrl/client.h>

#include "xenon/gpu/edram_surface.hpp"
#include "xenon/gpu/primitive_processor.hpp"
#include "xenon/gpu/resource_ir.hpp"
#include "xenon/gpu/shader_translation.hpp"

namespace xenon::gpu::d3d12 {

class GraphicsPipeline {
 public:
  [[nodiscard]] bool initialize(ID3D12Device* device,
                                ID3D12RootSignature* root_signature,
                                const CompiledShader& vertex_shader,
                                const CompiledShader& pixel_shader,
                                const CompiledShader* geometry_shader,
                                std::span<const DXGI_FORMAT> color_formats,
                                MsaaSamples samples,
                                HostPrimitiveTopology topology,
                                const RasterState& raster,
                                std::span<const std::uint8_t> color_write_masks,
                                std::span<const BlendState> blend_states,
                                DXGI_FORMAT depth_format = DXGI_FORMAT_UNKNOWN,
                                const DepthTargetDescriptor* depth_state = nullptr);
  void reset() noexcept;
  [[nodiscard]] ID3D12PipelineState* pipeline() const noexcept { return pipeline_.Get(); }
  [[nodiscard]] D3D12_PRIMITIVE_TOPOLOGY native_topology() const noexcept {
    return topology_;
  }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_{};
  D3D12_PRIMITIVE_TOPOLOGY topology_{D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST};
  std::string error_{};
};

}  // namespace xenon::gpu::d3d12

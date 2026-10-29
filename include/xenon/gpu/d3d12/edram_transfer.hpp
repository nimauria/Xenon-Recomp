#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>

#include <d3d12.h>
#include <wrl/client.h>

#include "xenon/gpu/d3d12/command_queue.hpp"
#include "xenon/gpu/d3d12/depth_target.hpp"
#include "xenon/gpu/d3d12/render_target.hpp"
#include "xenon/gpu/edram_surface.hpp"
#include "xenon/gpu/shader_translation.hpp"

namespace xenon::gpu::d3d12 {

// Moves EDRAM tiles directly from one native color image to another of the
// same format, on the GPU: a sample-frequency draw into the destination that
// loads, for each sample, the source sample holding the same EDRAM word. It is
// recorded into the queue's open batch like a draw, with no CPU round trip.
// Pipelines are built once per format and sample-count pair.
class EdramColorTransfer {
 public:
  [[nodiscard]] bool transfer(ID3D12Device* device, CommandQueue& queue,
                              RenderTargetImage& source, RenderTargetImage& destination,
                              const EdramTransferConstants& constants,
                              std::span<const EdramSurfaceRegion> regions);
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  [[nodiscard]] ID3D12PipelineState* pipeline(ID3D12Device* device, DXGI_FORMAT format,
                                              MsaaSamples source_host,
                                              MsaaSamples destination_host);

  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_{};
  std::unordered_map<std::uint64_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> pipelines_{};
  std::string error_{};
};

// The same direct transfer between two native depth images of one format.
// Depth and stencil move together in one pass where the device takes
// SV_StencilRef; otherwise depth moves first (stencil replaced with zero)
// and each stencil bit follows in its own pass, still with no CPU round trip.
class EdramDepthTransfer {
 public:
  [[nodiscard]] bool transfer(ID3D12Device* device, CommandQueue& queue,
                              DepthTargetImage& source, DepthTargetImage& destination,
                              const EdramTransferConstants& constants,
                              std::span<const EdramSurfaceRegion> regions);
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  // `stencil_bit` selects a StencilBit pass's write mask (ignored otherwise).
  [[nodiscard]] ID3D12PipelineState* pipeline(ID3D12Device* device, DXGI_FORMAT format,
                                              MsaaSamples source_host,
                                              MsaaSamples destination_host,
                                              EdramDepthTransferPass pass,
                                              std::uint32_t stencil_bit);

  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_{};
  std::unordered_map<std::uint64_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> pipelines_{};
  int stencil_ref_supported_{-1};
  std::string error_{};
};

}  // namespace xenon::gpu::d3d12

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include <d3d12.h>
#include <wrl/client.h>

#include "xenon/gpu/d3d12/command_queue.hpp"
#include "xenon/gpu/d3d12/depth_target.hpp"
#include "xenon/gpu/d3d12/edram_transfer.hpp"
#include "xenon/gpu/d3d12/guest_memory_mirror.hpp"
#include "xenon/gpu/d3d12/render_target.hpp"
#include "xenon/gpu/gpu_resolve.hpp"

namespace xenon::gpu::d3d12 {

// Resolves and their clears on the GPU, out of the native image that owns the
// resolved EDRAM tiles (see GpuResolveConstants): color resolves and clears
// from a color owner, depth resolves and clears from a depth owner. All are
// recorded into the queue's open batch; nothing is read back.
class GpuResolver {
 public:
  [[nodiscard]] bool resolve(ID3D12Device* device, CommandQueue& queue,
                             GuestMemoryMirror& mirror, RenderTargetImage& owner,
                             const GpuResolveConstants& constants);
  [[nodiscard]] bool resolve_depth(ID3D12Device* device, CommandQueue& queue,
                                   GuestMemoryMirror& mirror, DepthTargetImage& owner,
                                   const GpuResolveConstants& constants);
  [[nodiscard]] bool clear(ID3D12Device* device, CommandQueue& queue, RenderTargetImage& owner,
                           const GpuClearConstants& constants);
  [[nodiscard]] bool clear_depth(ID3D12Device* device, CommandQueue& queue,
                                 DepthTargetImage& owner, const GpuClearConstants& constants,
                                 std::uint8_t stencil);
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  [[nodiscard]] ID3D12RootSignature* resolve_root(ID3D12Device* device, std::uint32_t srvs);
  [[nodiscard]] ID3D12PipelineState* resolve_pipeline(ID3D12Device* device, MsaaSamples samples,
                                                      const EdramTransferEndpoint& source);
  [[nodiscard]] bool run_resolve(ID3D12Device* device, CommandQueue& queue,
                                 GuestMemoryMirror& mirror, const EdramTransferSourceImage& source,
                                 const GpuResolveConstants& constants);
  [[nodiscard]] ID3D12RootSignature* clear_root(ID3D12Device* device);
  [[nodiscard]] ID3D12PipelineState* clear_pipeline(ID3D12Device* device, DXGI_FORMAT format,
                                                    MsaaSamples samples, bool depth);

  // Indexed by source SRV count - 1: a color owner, or depth and stencil.
  Microsoft::WRL::ComPtr<ID3D12RootSignature> resolve_roots_[2]{};
  Microsoft::WRL::ComPtr<ID3D12RootSignature> clear_root_{};
  std::unordered_map<std::uint32_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> resolve_pipelines_{};
  std::unordered_map<std::uint64_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> clear_pipelines_{};
  std::string error_{};
};

}  // namespace xenon::gpu::d3d12

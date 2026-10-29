#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include <d3d12.h>
#include <wrl/client.h>

#include "xenon/gpu/d3d12/command_queue.hpp"
#include "xenon/gpu/d3d12/guest_memory_mirror.hpp"
#include "xenon/gpu/d3d12/render_target.hpp"
#include "xenon/gpu/gpu_resolve.hpp"

namespace xenon::gpu::d3d12 {

// Color resolves and their clears on the GPU, out of the native image that
// owns the resolved EDRAM tiles (see GpuResolveConstants). Both are recorded
// into the queue's open batch; nothing is read back.
class GpuResolver {
 public:
  [[nodiscard]] bool resolve(ID3D12Device* device, CommandQueue& queue,
                             GuestMemoryMirror& mirror, RenderTargetImage& owner,
                             const GpuResolveConstants& constants);
  [[nodiscard]] bool clear(ID3D12Device* device, CommandQueue& queue, RenderTargetImage& owner,
                           const GpuClearConstants& constants);
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  [[nodiscard]] ID3D12PipelineState* resolve_pipeline(ID3D12Device* device, MsaaSamples samples);
  [[nodiscard]] ID3D12PipelineState* clear_pipeline(ID3D12Device* device, DXGI_FORMAT format,
                                                    MsaaSamples samples);

  Microsoft::WRL::ComPtr<ID3D12RootSignature> resolve_root_{};
  Microsoft::WRL::ComPtr<ID3D12RootSignature> clear_root_{};
  std::unordered_map<std::uint32_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> resolve_pipelines_{};
  std::unordered_map<std::uint64_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> clear_pipelines_{};
  std::string error_{};
};

}  // namespace xenon::gpu::d3d12

#pragma once

#if !defined(_WIN32)
#error "The Direct3D 12 backend is Windows-only"
#endif

#include <cstdint>
#include <string>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace xenon::gpu::d3d12 {

struct ContextConfig {
  bool enable_debug_layer{};
  bool allow_software_adapter{};
};

struct DeviceProperties {
  std::string adapter_name{};
  std::uint64_t dedicated_video_memory{};
  D3D12_RESOURCE_BINDING_TIER resource_binding_tier{
      D3D12_RESOURCE_BINDING_TIER_1};
  D3D12_TILED_RESOURCES_TIER tiled_resources_tier{
      D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED};
  bool unified_memory{};
};

class Context {
 public:
  [[nodiscard]] bool initialize(const ContextConfig& config = {});
  void reset() noexcept;

  [[nodiscard]] ID3D12Device* device() const noexcept { return device_.Get(); }
  [[nodiscard]] IDXGIAdapter1* adapter() const noexcept { return adapter_.Get(); }
  [[nodiscard]] IDXGIFactory6* factory() const noexcept { return factory_.Get(); }
  [[nodiscard]] const DeviceProperties& properties() const noexcept {
    return properties_;
  }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  Microsoft::WRL::ComPtr<IDXGIFactory6> factory_{};
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter_{};
  Microsoft::WRL::ComPtr<ID3D12Device> device_{};
  DeviceProperties properties_{};
  std::string error_{};
};

}  // namespace xenon::gpu::d3d12

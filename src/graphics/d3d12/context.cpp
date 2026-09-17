#include "xenon/gpu/d3d12/context.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace xenon::gpu::d3d12 {
namespace {

std::string utf8_name(const wchar_t* value) {
  char buffer[128]{};
  WideCharToMultiByte(CP_UTF8, 0, value, -1, buffer,
                      static_cast<int>(sizeof(buffer)), nullptr, nullptr);
  return buffer;
}

}  // namespace

bool Context::initialize(const ContextConfig& config) {
  reset();
  UINT factory_flags = 0;
  if (config.enable_debug_layer) {
    Microsoft::WRL::ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
      debug->EnableDebugLayer();
      factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
    }
  }
  if (FAILED(CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory_)))) {
    error_ = "CreateDXGIFactory2 failed";
    return false;
  }
  for (UINT index = 0;; ++index) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
    if (factory_->EnumAdapterByGpuPreference(
            index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            IID_PPV_ARGS(&candidate)) == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    DXGI_ADAPTER_DESC1 desc{};
    candidate->GetDesc1(&desc);
    if (!config.allow_software_adapter &&
        (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
      continue;
    }
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0,
                                 IID_PPV_ARGS(&device)))) {
      continue;
    }
    adapter_ = std::move(candidate);
    device_ = std::move(device);
    properties_.adapter_name = utf8_name(desc.Description);
    properties_.dedicated_video_memory = desc.DedicatedVideoMemory;
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    if (SUCCEEDED(device_->CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)))) {
      properties_.resource_binding_tier = options.ResourceBindingTier;
      properties_.tiled_resources_tier = options.TiledResourcesTier;
    }
    D3D12_FEATURE_DATA_ARCHITECTURE architecture{};
    if (SUCCEEDED(device_->CheckFeatureSupport(
            D3D12_FEATURE_ARCHITECTURE, &architecture,
            sizeof(architecture)))) {
      properties_.unified_memory = architecture.UMA != FALSE;
    }
    return true;
  }
  error_ = "no D3D12 hardware adapter supports feature level 12_0";
  return false;
}

void Context::reset() noexcept {
  device_.Reset();
  adapter_.Reset();
  factory_.Reset();
  properties_ = {};
  error_.clear();
}

}  // namespace xenon::gpu::d3d12

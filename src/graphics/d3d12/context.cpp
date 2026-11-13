#include "xenon/gpu/d3d12/context.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdlib>
#include <string>

#include "xenon/logging/logger.hpp"

namespace xenon::gpu::d3d12 {
namespace {

std::string utf8_name(const wchar_t* value) {
  char buffer[128]{};
  WideCharToMultiByte(CP_UTF8, 0, value, -1, buffer,
                      static_cast<int>(sizeof(buffer)), nullptr, nullptr);
  return buffer;
}

bool debug_layer_requested() {
  const char* value = std::getenv("XENON_D3D12_DEBUG");
  return value != nullptr && *value != 0 && std::string(value) != "0";
}

// XENON_D3D12_ADAPTER=warp renders on WARP, Windows' software rasterizer,
// even where a GPU exists: what a host without one (CI's Windows runners)
// renders on, so its results can be reproduced on any machine.
bool software_adapter_requested() {
  const char* value = std::getenv("XENON_D3D12_ADAPTER");
  return value != nullptr && std::string(value) == "warp";
}

void CALLBACK debug_message(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity,
                            D3D12_MESSAGE_ID id, LPCSTR description, void*) {
  if (severity > D3D12_MESSAGE_SEVERITY_WARNING) return;  // info and messages
  xenon::logging::Logger::instance().record(
      severity == D3D12_MESSAGE_SEVERITY_WARNING ? xenon::logging::Level::Warning
                                                 : xenon::logging::Level::Error,
      "gpu", "D3D12 debug layer (id " + std::to_string(static_cast<int>(id)) + "): " +
                 std::string(description ? description : ""));
}

// Sends the debug layer's warnings and errors to the run's gpu.log. Needs
// ID3D12InfoQueue1 (Windows 11 SDK); without it the layer still runs.
void route_debug_messages(ID3D12Device* device) {
  Microsoft::WRL::ComPtr<ID3D12InfoQueue1> queue;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) return;
  DWORD cookie = 0;
  (void)queue->RegisterMessageCallback(&debug_message, D3D12_MESSAGE_CALLBACK_FLAG_NONE,
                                       nullptr, &cookie);
}

}  // namespace

bool Context::initialize(const ContextConfig& config) {
  reset();
  UINT factory_flags = 0;
  // XENON_D3D12_DEBUG=1 turns the debug layer on for any run, so a draw the
  // driver rejects says why in gpu.log instead of silently drawing nothing.
  const bool debug_layer = config.enable_debug_layer || debug_layer_requested();
  if (debug_layer) {
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
  const bool software_only = software_adapter_requested();
  for (UINT index = 0;; ++index) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
    if (factory_->EnumAdapterByGpuPreference(
            index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            IID_PPV_ARGS(&candidate)) == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    DXGI_ADAPTER_DESC1 desc{};
    candidate->GetDesc1(&desc);
    const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    if (software_only ? !software : (software && !config.allow_software_adapter)) {
      continue;
    }
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0,
                                 IID_PPV_ARGS(&device)))) {
      continue;
    }
    adapter_ = std::move(candidate);
    device_ = std::move(device);
    if (debug_layer) route_debug_messages(device_.Get());
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
  error_ = software_only ? "XENON_D3D12_ADAPTER=warp: WARP does not support feature level 12_0"
                        : "no D3D12 hardware adapter supports feature level 12_0";
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

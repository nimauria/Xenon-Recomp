#include "xenon/gpu/backend_capabilities.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#endif

#include <sstream>
#include <utility>

namespace xenon::gpu {

std::vector<BackendCapability> discover_backend_capabilities() {
  std::vector<BackendCapability> result;
  BackendCapability vulkan{BackendKind::Vulkan};
  BackendCapability d3d12{BackendKind::Direct3D12};
#if defined(_WIN32)
  if (HMODULE loader = LoadLibraryW(L"vulkan-1.dll")) {
    using EnumerateInstanceVersion = long(__stdcall*)(std::uint32_t*);
    const auto enumerate = reinterpret_cast<EnumerateInstanceVersion>(
        GetProcAddress(loader, "vkEnumerateInstanceVersion"));
    std::uint32_t version = (1u << 22);
    if (!enumerate || enumerate(&version) == 0) {
      vulkan.runtime_available = true;
      vulkan.api_version = version;
      std::ostringstream detail;
      detail << "Vulkan loader " << (version >> 22) << '.'
             << ((version >> 12) & 0x3FFu) << '.' << (version & 0xFFFu);
      vulkan.detail = detail.str();
    } else {
      vulkan.detail = "Vulkan loader version query failed";
    }
    FreeLibrary(loader);
  } else {
    vulkan.detail = "vulkan-1.dll is not installed";
  }
#if defined(XENON_HAS_VULKAN_SDK)
  vulkan.development_files_available = true;
#else
  if (vulkan.runtime_available) {
    vulkan.detail += "; Vulkan SDK headers/import library not found";
  }
#endif

  IDXGIFactory6* factory = nullptr;
  if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    for (UINT index = 0;; ++index) {
      IDXGIAdapter1* adapter = nullptr;
      const auto enum_result = factory->EnumAdapterByGpuPreference(
          index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
          IID_PPV_ARGS(&adapter));
      if (enum_result == DXGI_ERROR_NOT_FOUND) break;
      if (FAILED(enum_result) || !adapter) break;
      DXGI_ADAPTER_DESC1 desc{};
      adapter->GetDesc1(&desc);
      if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
          SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0,
                                      __uuidof(ID3D12Device), nullptr))) {
        d3d12.runtime_available = true;
        d3d12.development_files_available = true;
        d3d12.api_version = 120;
        char name[128]{};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name,
                            static_cast<int>(sizeof(name)), nullptr, nullptr);
        d3d12.detail = name;
        adapter->Release();
        break;
      }
      adapter->Release();
      adapter = nullptr;
    }
    factory->Release();
  }
  if (!d3d12.runtime_available) {
    d3d12.detail = "no hardware adapter supports D3D feature level 12_0";
  }
#else
  vulkan.detail = "runtime discovery is implemented for Windows in this phase";
  d3d12.detail = "Direct3D 12 is available only on Windows";
#endif
  result.push_back(std::move(vulkan));
  result.push_back(std::move(d3d12));
  return result;
}

}  // namespace xenon::gpu

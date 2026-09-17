#pragma once

#include <string>

#include <d3d12.h>
#include <wrl/client.h>

#include "xenon/gpu/d3d12/command_queue.hpp"
#include "xenon/gpu/edram_surface.hpp"

namespace xenon::gpu::d3d12 {

[[nodiscard]] DXGI_FORMAT color_render_target_format(
    ColorRenderTargetFormat format) noexcept;

class RenderTargetImage {
 public:
  [[nodiscard]] bool initialize(ID3D12Device* device,
                                const EdramSurfaceLayout& layout,
                                ColorRenderTargetFormat format);
  [[nodiscard]] bool clear(CommandQueue& queue,
                           const float value[4]);
  void reset() noexcept;
  [[nodiscard]] ID3D12Resource* resource() const noexcept { return resource_.Get(); }
  [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE rtv() const noexcept { return rtv_; }
  [[nodiscard]] DXGI_FORMAT format() const noexcept { return format_; }
  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  Microsoft::WRL::ComPtr<ID3D12Resource> resource_{};
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap_{};
  D3D12_CPU_DESCRIPTOR_HANDLE rtv_{};
  DXGI_FORMAT format_{DXGI_FORMAT_UNKNOWN};
  std::uint32_t width_{};
  std::uint32_t height_{};
  std::string error_{};
};

}  // namespace xenon::gpu::d3d12

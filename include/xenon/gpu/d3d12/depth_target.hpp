#pragma once

#include <cstdint>
#include <string>

#include <d3d12.h>
#include <dxgiformat.h>
#include <wrl/client.h>

#include "xenon/gpu/d3d12/command_queue.hpp"
#include "xenon/gpu/edram_surface.hpp"

namespace xenon::gpu::d3d12 {

[[nodiscard]] DXGI_FORMAT depth_render_target_format(
    DepthRenderTargetFormat format) noexcept;

class DepthTargetImage {
 public:
  [[nodiscard]] bool initialize(ID3D12Device* device,
                                const EdramSurfaceLayout& layout,
                                DepthRenderTargetFormat format);
  [[nodiscard]] bool clear(CommandQueue& queue, float depth,
                           std::uint8_t stencil);
  void reset() noexcept;
  [[nodiscard]] ID3D12Resource* resource() const noexcept { return resource_.Get(); }
  [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE dsv() const noexcept { return dsv_; }
  [[nodiscard]] DXGI_FORMAT format() const noexcept { return format_; }
  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
  [[nodiscard]] const EdramSurfaceLayout& surface() const noexcept {
    return surface_;
  }
  [[nodiscard]] bool requires_float24_conversion() const noexcept {
    return float24_;
  }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  Microsoft::WRL::ComPtr<ID3D12Resource> resource_{};
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsv_heap_{};
  D3D12_CPU_DESCRIPTOR_HANDLE dsv_{};
  DXGI_FORMAT format_{DXGI_FORMAT_UNKNOWN};
  bool float24_{};
  std::uint32_t width_{};
  std::uint32_t height_{};
  EdramSurfaceLayout surface_{};
  std::string error_{};
};

}  // namespace xenon::gpu::d3d12

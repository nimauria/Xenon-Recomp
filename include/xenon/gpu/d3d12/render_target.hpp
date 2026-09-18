#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

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
  [[nodiscard]] bool upload(CommandQueue& queue,
                            std::span<const std::byte> source,
                            std::uint32_t row_pitch);
  [[nodiscard]] bool readback(CommandQueue& queue, std::uint32_t left,
                              std::uint32_t top, std::uint32_t right,
                              std::uint32_t bottom,
                              std::vector<std::byte>& destination,
                              std::uint32_t& row_pitch);
  void reset() noexcept;
  [[nodiscard]] ID3D12Resource* resource() const noexcept { return resource_.Get(); }
  [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE rtv() const noexcept { return rtv_; }
  [[nodiscard]] DXGI_FORMAT format() const noexcept { return format_; }
  [[nodiscard]] ColorRenderTargetFormat guest_format() const noexcept {
    return guest_format_;
  }
  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
  [[nodiscard]] const EdramSurfaceLayout& surface() const noexcept {
    return surface_;
  }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  Microsoft::WRL::ComPtr<ID3D12Resource> resource_{};
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap_{};
  D3D12_CPU_DESCRIPTOR_HANDLE rtv_{};
  DXGI_FORMAT format_{DXGI_FORMAT_UNKNOWN};
  ColorRenderTargetFormat guest_format_{ColorRenderTargetFormat::R8G8B8A8};
  std::uint32_t width_{};
  std::uint32_t height_{};
  std::uint32_t bytes_per_pixel_{};
  EdramSurfaceLayout surface_{};
  std::string error_{};
};

}  // namespace xenon::gpu::d3d12

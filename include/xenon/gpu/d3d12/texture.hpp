#pragma once

#include <string>

#include <d3d12.h>
#include <dxgiformat.h>
#include <wrl/client.h>

#include "xenon/gpu/d3d12/command_queue.hpp"
#include "xenon/gpu/texture.hpp"

namespace xenon::gpu::d3d12 {

[[nodiscard]] DXGI_FORMAT host_texture_format(TextureHostFormat format) noexcept;

class TextureImage {
 public:
  [[nodiscard]] bool initialize(ID3D12Device* device, CommandQueue& queue,
                                const DecodedTexture& texture);
  void reset() noexcept;
  [[nodiscard]] ID3D12Resource* resource() const noexcept { return resource_.Get(); }
  [[nodiscard]] DXGI_FORMAT format() const noexcept { return format_; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  Microsoft::WRL::ComPtr<ID3D12Resource> resource_{};
  DXGI_FORMAT format_{DXGI_FORMAT_UNKNOWN};
  std::string error_{};
};

}  // namespace xenon::gpu::d3d12

#pragma once

#if !defined(_WIN32)
#error "The Direct3D 12 backend is Windows-only"
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "xenon/gpu/d3d12/command_queue.hpp"
#include "xenon/gpu/d3d12/context.hpp"
#include "xenon/gpu/d3d12/memory.hpp"
#include "xenon/gpu/presentation.hpp"

namespace xenon::gpu::d3d12 {

class PresentationSwapchain {
 public:
  static constexpr std::uint32_t kMaxBuffers = 4;

  [[nodiscard]] bool initialize(Context& context, CommandQueue& queue,
                                void* native_window,
                                const PresentationConfig& config);
  void reset() noexcept;
  [[nodiscard]] bool resize(std::uint32_t width, std::uint32_t height);
  // Records `draw` into the current back buffer (in RENDER_TARGET state, of
  // format()) and presents it.
  using DrawBackBuffer =
      std::function<void(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE)>;
  [[nodiscard]] PresentStatus present(const DrawBackBuffer& draw);
  [[nodiscard]] static constexpr DXGI_FORMAT format() noexcept {
    return DXGI_FORMAT_R8G8B8A8_UNORM;
  }

  [[nodiscard]] bool ready() const noexcept { return swapchain_ != nullptr; }
  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
  [[nodiscard]] const PresentationConfig& config() const noexcept { return config_; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  struct BackBuffer {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource{};
    std::uint64_t fence_value{};
  };

  [[nodiscard]] bool rebuild_back_buffers();
  void release_back_buffers() noexcept;

  Context* context_{};
  CommandQueue* queue_{};
  void* native_window_{};
  Microsoft::WRL::ComPtr<IDXGISwapChain4> swapchain_{};
  std::array<BackBuffer, kMaxBuffers> buffers_{};
  PresentationConfig config_{};
  std::uint32_t buffer_count_{};
  std::uint32_t width_{};
  std::uint32_t height_{};
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap_{};
  std::uint32_t rtv_stride_{};
  bool tearing_supported_{};
  std::string error_{};
};

}  // namespace xenon::gpu::d3d12

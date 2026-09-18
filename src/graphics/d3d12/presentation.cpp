#include "xenon/gpu/d3d12/presentation.hpp"

#include <algorithm>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace xenon::gpu::d3d12 {
namespace {
constexpr DXGI_FORMAT kSwapchainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr std::uint32_t align_up(std::uint32_t value, std::uint32_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}
}  // namespace

bool PresentationSwapchain::initialize(Context& context, CommandQueue& queue,
                                       void* native_window,
                                       const PresentationConfig& config) {
  reset();
  error_.clear();
  if (!context.device() || !context.factory() || !queue.native_queue() ||
      !native_window) {
    error_ = "D3D12 presentation requires a device, command queue and native window";
    return false;
  }
  if (!config.width || !config.height) {
    error_ = "D3D12 presentation requires a non-zero initial extent";
    return false;
  }
  context_ = &context;
  queue_ = &queue;
  native_window_ = native_window;
  config_ = config;
  width_ = config.width;
  height_ = config.height;
  buffer_count_ = std::clamp(config.image_count, 2u, kMaxBuffers);

  BOOL allow_tearing = FALSE;
  if (SUCCEEDED(context.factory()->CheckFeatureSupport(
          DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing,
          sizeof(allow_tearing)))) {
    tearing_supported_ = allow_tearing != FALSE;
  }

  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = width_;
  desc.Height = height_;
  desc.Format = kSwapchainFormat;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = buffer_count_;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  if (!config.vsync && config.allow_tearing && tearing_supported_) {
    desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
  }

  Microsoft::WRL::ComPtr<IDXGISwapChain1> swapchain1;
  if (FAILED(context.factory()->CreateSwapChainForHwnd(
          queue.native_queue(), static_cast<HWND>(native_window), &desc,
          nullptr, nullptr, &swapchain1)) ||
      FAILED(swapchain1.As(&swapchain_))) {
    error_ = "CreateSwapChainForHwnd failed";
    reset();
    return false;
  }
  context.factory()->MakeWindowAssociation(static_cast<HWND>(native_window),
                                           DXGI_MWA_NO_ALT_ENTER);
  if (!rebuild_back_buffers()) {
    reset();
    return false;
  }
  return true;
}

void PresentationSwapchain::release_back_buffers() noexcept {
  for (auto& buffer : buffers_) {
    buffer.upload_mapping = {};
    buffer.upload.reset();
    buffer.resource.Reset();
    buffer.fence_value = 0;
  }
}

void PresentationSwapchain::reset() noexcept {
  if (queue_ && swapchain_) (void)queue_->wait_idle();
  release_back_buffers();
  swapchain_.Reset();
  context_ = nullptr;
  queue_ = nullptr;
  native_window_ = nullptr;
  buffer_count_ = 0;
  width_ = height_ = upload_row_pitch_ = 0;
  tearing_supported_ = false;
  config_ = {};
}

bool PresentationSwapchain::rebuild_back_buffers() {
  if (!context_ || !queue_ || !swapchain_ || !width_ || !height_) return false;
  upload_row_pitch_ = align_up(width_ * 4u, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
  const auto upload_size = std::uint64_t(upload_row_pitch_) * height_;
  for (std::uint32_t i = 0; i < buffer_count_; ++i) {
    auto& back_buffer = buffers_[i];
    if (FAILED(swapchain_->GetBuffer(i, IID_PPV_ARGS(&back_buffer.resource)))) {
      error_ = "IDXGISwapChain::GetBuffer failed";
      return false;
    }
    if (!back_buffer.upload.initialize(
            context_->device(), upload_size, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ) ||
        !back_buffer.upload.map(back_buffer.upload_mapping)) {
      error_ = back_buffer.upload.error().empty()
                   ? "D3D12 presentation upload allocation failed"
                   : back_buffer.upload.error();
      return false;
    }
  }
  return true;
}

bool PresentationSwapchain::resize(std::uint32_t width, std::uint32_t height) {
  if (!swapchain_ || !queue_) {
    error_ = "D3D12 presentation is not configured";
    return false;
  }
  if (!width || !height) {
    width_ = width;
    height_ = height;
    return true;
  }
  if (width == width_ && height == height_) return true;
  if (!queue_->wait_idle()) {
    error_ = queue_->error();
    return false;
  }
  release_back_buffers();
  UINT flags = 0;
  if (!config_.vsync && config_.allow_tearing && tearing_supported_)
    flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
  if (FAILED(swapchain_->ResizeBuffers(buffer_count_, width, height,
                                       kSwapchainFormat, flags))) {
    error_ = "IDXGISwapChain::ResizeBuffers failed";
    return false;
  }
  width_ = width;
  height_ = height;
  config_.width = width;
  config_.height = height;
  return rebuild_back_buffers();
}

PresentStatus PresentationSwapchain::present(
    const PreparedPresentationFrame& frame) {
  if (!swapchain_ || !queue_) return PresentStatus::NotConfigured;
  if (!width_ || !height_) return PresentStatus::Minimized;
  if (!frame.valid || frame.width != width_ || frame.height != height_ ||
      frame.row_pitch < width_ * 4u) {
    error_ = "D3D12 presentation frame extent does not match the swapchain";
    return PresentStatus::Error;
  }
  const auto index = swapchain_->GetCurrentBackBufferIndex();
  if (index >= buffer_count_) {
    error_ = "DXGI returned an invalid back-buffer index";
    return PresentStatus::Error;
  }
  auto& back_buffer = buffers_[index];
  if (back_buffer.fence_value &&
      queue_->completed_value() < back_buffer.fence_value &&
      !queue_->wait_for_completion(back_buffer.fence_value)) {
    error_ = queue_->error();
    return PresentStatus::Error;
  }
  for (std::uint32_t y = 0; y < height_; ++y) {
    std::memcpy(back_buffer.upload_mapping.data() +
                    std::size_t(y) * upload_row_pitch_,
                frame.rgba8.data() + std::size_t(y) * frame.row_pitch,
                std::size_t(width_) * 4u);
  }

  if (!queue_->execute_async([&](ID3D12GraphicsCommandList* list,
                                 std::uint32_t, std::uint32_t) {
        D3D12_RESOURCE_BARRIER to_copy{};
        to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Transition.pResource = back_buffer.resource.Get();
        to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &to_copy);

        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = back_buffer.upload.resource();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint.Offset = 0;
        source.PlacedFootprint.Footprint.Format = kSwapchainFormat;
        source.PlacedFootprint.Footprint.Width = width_;
        source.PlacedFootprint.Footprint.Height = height_;
        source.PlacedFootprint.Footprint.Depth = 1;
        source.PlacedFootprint.Footprint.RowPitch = upload_row_pitch_;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = back_buffer.resource.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = 0;
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

        std::swap(to_copy.Transition.StateBefore, to_copy.Transition.StateAfter);
        list->ResourceBarrier(1, &to_copy);
      }) ||
      !queue_->flush()) {
    error_ = queue_->error();
    return PresentStatus::Error;
  }
  back_buffer.fence_value = queue_->last_submitted_value();

  const UINT sync_interval = config_.vsync ? 1u : 0u;
  UINT flags = 0;
  if (!config_.vsync && config_.allow_tearing && tearing_supported_)
    flags |= DXGI_PRESENT_ALLOW_TEARING;
  const HRESULT result = swapchain_->Present(sync_interval, flags);
  if (result == DXGI_STATUS_OCCLUDED) return PresentStatus::Suboptimal;
  if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
    error_ = "DXGI device was removed during presentation";
    return PresentStatus::SurfaceLost;
  }
  if (FAILED(result)) {
    error_ = "IDXGISwapChain::Present failed";
    return PresentStatus::Error;
  }
  return PresentStatus::Success;
}

}  // namespace xenon::gpu::d3d12

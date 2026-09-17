#include "xenon/gpu/d3d12/render_target.hpp"

namespace xenon::gpu::d3d12 {

DXGI_FORMAT color_render_target_format(ColorRenderTargetFormat format) noexcept {
  switch (format) {
    case ColorRenderTargetFormat::R8G8B8A8: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case ColorRenderTargetFormat::R8G8B8A8Gamma: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case ColorRenderTargetFormat::R10G10B10A2: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case ColorRenderTargetFormat::R16G16Float: return DXGI_FORMAT_R16G16_FLOAT;
    case ColorRenderTargetFormat::R16G16B16A16Float:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case ColorRenderTargetFormat::R32Float: return DXGI_FORMAT_R32_FLOAT;
    case ColorRenderTargetFormat::R32G32Float: return DXGI_FORMAT_R32G32_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

bool RenderTargetImage::initialize(ID3D12Device* device,
                                   const EdramSurfaceLayout& surface,
                                   ColorRenderTargetFormat color_format) {
  reset();
  error_.clear();
  if (!device || !surface.valid() || surface.depth) {
    error_ = "D3D12 color target requires a valid color EDRAM surface";
    return false;
  }
  format_ = color_render_target_format(color_format);
  if (format_ == DXGI_FORMAT_UNKNOWN) {
    error_ = "Xenos color target format needs a non-native conversion path";
    return false;
  }
  width_ = surface.pitch_pixels;
  height_ = surface.height_pixels;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = surface.pitch_pixels;
  desc.Height = surface.height_pixels;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format_;
  desc.SampleDesc.Count = 1u << static_cast<unsigned>(surface.msaa);
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE clear{};
  clear.Format = format_;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  if (FAILED(device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
          IID_PPV_ARGS(&resource_)))) {
    error_ = "CreateCommittedResource failed for Xenos color target";
    reset(); return false;
  }
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heap_desc.NumDescriptors = 1;
  if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap_)))) {
    error_ = "D3D12 RTV heap creation failed";
    reset(); return false;
  }
  rtv_ = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(resource_.Get(), nullptr, rtv_);
  return true;
}

bool RenderTargetImage::clear(CommandQueue& queue, const float value[4]) {
  if (!resource_ || !rtv_.ptr || !value) return false;
  if (!queue.execute([&](ID3D12GraphicsCommandList* list) {
        list->ClearRenderTargetView(rtv_, value, 0, nullptr);
      })) {
    error_ = queue.error();
    return false;
  }
  return true;
}

void RenderTargetImage::reset() noexcept {
  resource_.Reset();
  rtv_heap_.Reset();
  rtv_ = {};
  format_ = DXGI_FORMAT_UNKNOWN;
  width_ = height_ = 0;
}

}  // namespace xenon::gpu::d3d12

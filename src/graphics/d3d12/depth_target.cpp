#include "xenon/gpu/d3d12/depth_target.hpp"

namespace xenon::gpu::d3d12 {

DXGI_FORMAT depth_render_target_format(DepthRenderTargetFormat format) noexcept {
  return format == DepthRenderTargetFormat::D24S8
             ? DXGI_FORMAT_D24_UNORM_S8_UINT
             : DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}

bool DepthTargetImage::initialize(ID3D12Device* device,
                                  const EdramSurfaceLayout& surface,
                                  DepthRenderTargetFormat depth_format) {
  reset();
  error_.clear();
  if (!device || !surface.valid() || !surface.depth) {
    error_ = "D3D12 depth target requires a valid depth EDRAM surface";
    return false;
  }
  format_ = depth_render_target_format(depth_format);
  width_ = surface.pitch_pixels;
  height_ = surface.height_pixels;
  float24_ = depth_format == DepthRenderTargetFormat::D24FS8;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = surface.pitch_pixels;
  desc.Height = surface.height_pixels;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format_;
  desc.SampleDesc.Count = 1u << static_cast<unsigned>(surface.msaa);
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  D3D12_CLEAR_VALUE clear{};
  clear.Format = format_;
  clear.DepthStencil = {1.0f, 0};
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  if (FAILED(device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
          IID_PPV_ARGS(&resource_)))) {
    error_ = "CreateCommittedResource failed for Xenos depth target";
    reset();
    return false;
  }
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  heap_desc.NumDescriptors = 1;
  if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&dsv_heap_)))) {
    error_ = "D3D12 DSV heap creation failed";
    reset();
    return false;
  }
  dsv_ = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
  device->CreateDepthStencilView(resource_.Get(), nullptr, dsv_);
  return true;
}

bool DepthTargetImage::clear(CommandQueue& queue, float depth,
                             std::uint8_t stencil) {
  if (!resource_ || !dsv_.ptr) return false;
  if (!queue.execute([&](ID3D12GraphicsCommandList* list) {
        list->ClearDepthStencilView(dsv_,
                                    D3D12_CLEAR_FLAG_DEPTH |
                                        D3D12_CLEAR_FLAG_STENCIL,
                                    depth, stencil, 0, nullptr);
      })) {
    error_ = queue.error();
    return false;
  }
  return true;
}

void DepthTargetImage::reset() noexcept {
  resource_.Reset();
  dsv_heap_.Reset();
  dsv_ = {};
  format_ = DXGI_FORMAT_UNKNOWN;
  float24_ = false;
  width_ = height_ = 0;
}

}  // namespace xenon::gpu::d3d12

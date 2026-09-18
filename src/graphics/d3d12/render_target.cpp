#include "xenon/gpu/d3d12/render_target.hpp"

#include <algorithm>
#include <cstring>
#include <span>

#include "xenon/gpu/d3d12/memory.hpp"

namespace xenon::gpu::d3d12 {

DXGI_FORMAT color_render_target_format(ColorRenderTargetFormat format) noexcept {
  switch (color_host_storage(format)) {
    case ColorHostStorage::R8G8B8A8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case ColorHostStorage::R16G16B16A16Unorm: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case ColorHostStorage::R10G10B10A2Unorm: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case ColorHostStorage::R16G16Float: return DXGI_FORMAT_R16G16_FLOAT;
    case ColorHostStorage::R16G16B16A16Float:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case ColorHostStorage::R32Float: return DXGI_FORMAT_R32_FLOAT;
    case ColorHostStorage::R32G32Float: return DXGI_FORMAT_R32G32_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

namespace {

std::uint32_t color_bytes_per_pixel(ColorRenderTargetFormat format) noexcept {
  return color_host_bytes_per_pixel(format);
}

}  // namespace

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
  guest_format_ = color_format;
  bytes_per_pixel_ = color_bytes_per_pixel(color_format);
  if (format_ == DXGI_FORMAT_UNKNOWN) {
    error_ = "Xenos color target format needs a non-native conversion path";
    return false;
  }
  width_ = surface.pitch_pixels;
  height_ = surface.height_pixels;
  surface_ = surface;
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

bool RenderTargetImage::upload(CommandQueue& queue,
                               std::span<const std::byte> source,
                               std::uint32_t row_pitch) {
  if (!resource_ || !bytes_per_pixel_ || surface_.msaa != MsaaSamples::X1 ||
      row_pitch < width_ * bytes_per_pixel_ ||
      std::uint64_t(row_pitch) * height_ > source.size()) {
    error_ = "invalid D3D12 color-target ownership upload";
    return false;
  }
  Microsoft::WRL::ComPtr<ID3D12Device> device;
  if (FAILED(resource_->GetDevice(IID_PPV_ARGS(&device)))) {
    error_ = "failed to obtain D3D12 device for color upload";
    return false;
  }
  const auto desc = resource_->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rows{};
  UINT64 row_size{}, upload_size{};
  device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows,
                                &row_size, &upload_size);
  Buffer upload;
  if (!upload.initialize(device.Get(), upload_size, D3D12_HEAP_TYPE_UPLOAD,
                         D3D12_RESOURCE_STATE_GENERIC_READ)) {
    error_ = upload.error();
    return false;
  }
  std::span<std::byte> mapped;
  if (!upload.map(mapped)) {
    error_ = upload.error();
    return false;
  }
  std::fill(mapped.begin(), mapped.end(), std::byte{});
  for (std::uint32_t y = 0; y < height_; ++y)
    std::memcpy(mapped.data() + footprint.Offset +
                    std::uint64_t(y) * footprint.Footprint.RowPitch,
                source.data() + std::uint64_t(y) * row_pitch,
                width_ * bytes_per_pixel_);
  upload.unmap();
  if (!queue.execute([&](ID3D12GraphicsCommandList* list) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource_.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION source_location{};
        source_location.pResource = upload.resource();
        source_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source_location.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION destination_location{};
        destination_location.pResource = resource_.Get();
        destination_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&destination_location, 0, 0, 0,
                                &source_location, nullptr);
        std::swap(barrier.Transition.StateBefore,
                  barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
      })) {
    error_ = queue.error();
    return false;
  }
  return true;
}

bool RenderTargetImage::readback(CommandQueue& queue, std::uint32_t left,
                                 std::uint32_t top, std::uint32_t right,
                                 std::uint32_t bottom,
                                 std::vector<std::byte>& destination,
                                 std::uint32_t& row_pitch) {
  destination.clear();
  row_pitch = 0;
  if (!resource_ || !bytes_per_pixel_ || left >= right || top >= bottom ||
      right > width_ || bottom > height_) {
    error_ = "invalid D3D12 color-target readback rectangle";
    return false;
  }
  Microsoft::WRL::ComPtr<ID3D12Device> device;
  if (FAILED(resource_->GetDevice(IID_PPV_ARGS(&device)))) {
    error_ = "failed to obtain D3D12 device for color readback";
    return false;
  }
  const auto copy_width = right - left;
  const auto copy_height = bottom - top;
  row_pitch = copy_width * bytes_per_pixel_;
  const auto native_row_pitch = (row_pitch + 255u) & ~255u;
  Buffer readback;
  if (!readback.initialize(device.Get(),
                           std::uint64_t(native_row_pitch) * copy_height,
                           D3D12_HEAP_TYPE_READBACK,
                           D3D12_RESOURCE_STATE_COPY_DEST)) {
    error_ = readback.error();
    return false;
  }

  const auto resource_desc = resource_->GetDesc();
  Microsoft::WRL::ComPtr<ID3D12Resource> resolved;
  if (resource_desc.SampleDesc.Count > 1) {
    auto resolved_desc = resource_desc;
    resolved_desc.SampleDesc = {1, 0};
    resolved_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &resolved_desc,
            D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr,
            IID_PPV_ARGS(&resolved)))) {
      error_ = "failed to allocate D3D12 resolve readback image";
      return false;
    }
  }

  if (!queue.execute([&](ID3D12GraphicsCommandList* list) {
        ID3D12Resource* copy_source = resource_.Get();
        D3D12_RESOURCE_BARRIER source_barrier{};
        source_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        source_barrier.Transition.pResource = resource_.Get();
        source_barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        source_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        source_barrier.Transition.StateAfter = resolved
            ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE
            : D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &source_barrier);
        if (resolved) {
          list->ResolveSubresource(resolved.Get(), 0, resource_.Get(), 0, format_);
          D3D12_RESOURCE_BARRIER resolved_barrier{};
          resolved_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          resolved_barrier.Transition.pResource = resolved.Get();
          resolved_barrier.Transition.Subresource =
              D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          resolved_barrier.Transition.StateBefore =
              D3D12_RESOURCE_STATE_RESOLVE_DEST;
          resolved_barrier.Transition.StateAfter =
              D3D12_RESOURCE_STATE_COPY_SOURCE;
          list->ResourceBarrier(1, &resolved_barrier);
          copy_source = resolved.Get();
        }
        D3D12_TEXTURE_COPY_LOCATION destination_location{};
        destination_location.pResource = readback.resource();
        destination_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination_location.PlacedFootprint.Footprint = {
            format_, copy_width, copy_height, 1, native_row_pitch};
        D3D12_TEXTURE_COPY_LOCATION source_location{};
        source_location.pResource = copy_source;
        source_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{left, top, 0, right, bottom, 1};
        list->CopyTextureRegion(&destination_location, 0, 0, 0,
                                &source_location, &box);
        std::swap(source_barrier.Transition.StateBefore,
                  source_barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &source_barrier);
      })) {
    error_ = queue.error();
    return false;
  }
  std::span<std::byte> mapped;
  if (!readback.map(mapped)) {
    error_ = readback.error();
    return false;
  }
  destination.resize(std::size_t(row_pitch) * copy_height);
  for (std::uint32_t y = 0; y < copy_height; ++y) {
    std::memcpy(destination.data() + std::size_t(y) * row_pitch,
                mapped.data() + std::size_t(y) * native_row_pitch, row_pitch);
  }
  readback.unmap();
  return true;
}

void RenderTargetImage::reset() noexcept {
  resource_.Reset();
  rtv_heap_.Reset();
  rtv_ = {};
  format_ = DXGI_FORMAT_UNKNOWN;
  guest_format_ = ColorRenderTargetFormat::R8G8B8A8;
  width_ = height_ = bytes_per_pixel_ = 0;
  surface_ = {};
}

}  // namespace xenon::gpu::d3d12

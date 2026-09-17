#include "xenon/gpu/d3d12/texture.hpp"

#include <algorithm>
#include <cstring>
#include <span>
#include <vector>

#include "xenon/gpu/d3d12/memory.hpp"

namespace xenon::gpu::d3d12 {

DXGI_FORMAT host_texture_format(TextureHostFormat format) noexcept {
  switch (format) {
    case TextureHostFormat::R8Unorm: return DXGI_FORMAT_R8_UNORM;
    case TextureHostFormat::R8G8Unorm: return DXGI_FORMAT_R8G8_UNORM;
    case TextureHostFormat::R8G8B8A8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case TextureHostFormat::R8G8B8A8Srgb: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case TextureHostFormat::B5G5R5A1Unorm: return DXGI_FORMAT_B5G5R5A1_UNORM;
    case TextureHostFormat::B5G6R5Unorm: return DXGI_FORMAT_B5G6R5_UNORM;
    case TextureHostFormat::B4G4R4A4Unorm: return DXGI_FORMAT_B4G4R4A4_UNORM;
    case TextureHostFormat::R10G10B10A2Unorm: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case TextureHostFormat::R11G11B10Float: return DXGI_FORMAT_R11G11B10_FLOAT;
    case TextureHostFormat::R16Unorm: return DXGI_FORMAT_R16_UNORM;
    case TextureHostFormat::R16G16Unorm: return DXGI_FORMAT_R16G16_UNORM;
    case TextureHostFormat::R16G16B16A16Unorm: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case TextureHostFormat::R16Float: return DXGI_FORMAT_R16_FLOAT;
    case TextureHostFormat::R16G16Float: return DXGI_FORMAT_R16G16_FLOAT;
    case TextureHostFormat::R16G16B16A16Float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case TextureHostFormat::R32Uint: return DXGI_FORMAT_R32_UINT;
    case TextureHostFormat::R32G32Uint: return DXGI_FORMAT_R32G32_UINT;
    case TextureHostFormat::R32G32B32A32Uint: return DXGI_FORMAT_R32G32B32A32_UINT;
    case TextureHostFormat::R32Float: return DXGI_FORMAT_R32_FLOAT;
    case TextureHostFormat::R32G32Float: return DXGI_FORMAT_R32G32_FLOAT;
    case TextureHostFormat::R32G32B32Float: return DXGI_FORMAT_R32G32B32_FLOAT;
    case TextureHostFormat::R32G32B32A32Float: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case TextureHostFormat::D24UnormS8Uint: return DXGI_FORMAT_R24G8_TYPELESS;
    case TextureHostFormat::BC1Unorm: return DXGI_FORMAT_BC1_UNORM;
    case TextureHostFormat::BC2Unorm: return DXGI_FORMAT_BC2_UNORM;
    case TextureHostFormat::BC3Unorm: return DXGI_FORMAT_BC3_UNORM;
    case TextureHostFormat::BC4Unorm: return DXGI_FORMAT_BC4_UNORM;
    case TextureHostFormat::BC5Unorm: return DXGI_FORMAT_BC5_UNORM;
    case TextureHostFormat::Unsupported: break;
  }
  return DXGI_FORMAT_UNKNOWN;
}

bool TextureImage::initialize(ID3D12Device* device, CommandQueue& queue,
                              const DecodedTexture& texture) {
  reset();
  error_.clear();
  if (!device || !texture.valid) {
    error_ = "D3D12 texture image requires decoded texture data and a device";
    return false;
  }
  const auto& descriptor = texture.layout.descriptor;
  format_ = host_texture_format(texture.layout.format.host_format);
  if (format_ == DXGI_FORMAT_UNKNOWN) {
    error_ = "Xenos texture format has no D3D12 image mapping";
    return false;
  }
  const auto dimension_support = descriptor.dimension == TextureDimension::OneD
                                     ? D3D12_FORMAT_SUPPORT1_TEXTURE1D
                                 : descriptor.dimension == TextureDimension::ThreeD
                                     ? D3D12_FORMAT_SUPPORT1_TEXTURE3D
                                 : descriptor.dimension == TextureDimension::Cube
                                     ? D3D12_FORMAT_SUPPORT1_TEXTURECUBE
                                     : D3D12_FORMAT_SUPPORT1_TEXTURE2D;
  D3D12_FEATURE_DATA_FORMAT_SUPPORT format_support{format_};
  if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                                         &format_support,
                                         sizeof(format_support))) ||
      !(format_support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) ||
      !(format_support.Support1 & dimension_support)) {
    error_ = "D3D12 device cannot sample the mapped Xenos texture format";
    reset();
    return false;
  }
  const auto mip_levels = std::uint16_t(descriptor.mip_max_level + 1u);
  const auto layers = descriptor.dimension == TextureDimension::Cube ? 6u :
                      descriptor.dimension == TextureDimension::TwoDOrStacked && descriptor.stacked
                          ? descriptor.depth : 1u;
  D3D12_RESOURCE_DESC image{};
  image.Dimension = descriptor.dimension == TextureDimension::OneD
                        ? D3D12_RESOURCE_DIMENSION_TEXTURE1D
                        : descriptor.dimension == TextureDimension::ThreeD
                              ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                              : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  image.Width = descriptor.width;
  image.Height = descriptor.dimension == TextureDimension::OneD ? 1u : descriptor.height;
  image.DepthOrArraySize = static_cast<UINT16>(
      descriptor.dimension == TextureDimension::ThreeD ? descriptor.depth : layers);
  image.MipLevels = mip_levels;
  image.Format = format_;
  image.SampleDesc.Count = 1;
  image.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  if (FAILED(device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &image, D3D12_RESOURCE_STATE_COPY_DEST,
          nullptr, IID_PPV_ARGS(&resource_)))) {
    error_ = "CreateCommittedResource failed for decoded Xenos texture";
    reset(); return false;
  }
  const auto subresource_count = descriptor.dimension == TextureDimension::ThreeD
                                     ? std::uint32_t(mip_levels)
                                     : std::uint32_t(mip_levels) * layers;
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresource_count);
  std::vector<UINT> rows(subresource_count);
  std::vector<UINT64> row_sizes(subresource_count);
  UINT64 upload_size{};
  device->GetCopyableFootprints(&image, 0, subresource_count, 0,
                                footprints.data(), rows.data(), row_sizes.data(),
                                &upload_size);
  Buffer upload;
  if (!upload.initialize(device, upload_size, D3D12_HEAP_TYPE_UPLOAD,
                         D3D12_RESOURCE_STATE_GENERIC_READ)) {
    error_ = upload.error(); reset(); return false;
  }
  std::span<std::byte> mapped;
  if (!upload.map(mapped)) { error_ = upload.error(); reset(); return false; }
  std::fill(mapped.begin(), mapped.end(), std::byte{});
  for (const auto& sub : texture.layout.subresources) {
    const auto index = descriptor.dimension == TextureDimension::ThreeD
                           ? sub.mip_level
                           : sub.mip_level + sub.array_layer * mip_levels;
    const auto& footprint = footprints[index];
    const auto source_slice = std::uint64_t(sub.linear_row_pitch_bytes) *
                              sub.height_blocks;
    const auto destination_slice = std::uint64_t(footprint.Footprint.RowPitch) *
                                   rows[index];
    for (std::uint32_t z = 0; z < sub.depth_texels; ++z)
      for (std::uint32_t y = 0; y < sub.height_blocks; ++y)
        std::memcpy(mapped.data() + footprint.Offset + z * destination_slice +
                        std::uint64_t(y) * footprint.Footprint.RowPitch,
                    texture.linear_data.data() + sub.linear_offset_bytes +
                        z * source_slice + std::uint64_t(y) * sub.linear_row_pitch_bytes,
                    sub.linear_row_pitch_bytes);
  }
  upload.unmap();
  if (!queue.execute([&](ID3D12GraphicsCommandList* list) {
        for (const auto& sub : texture.layout.subresources) {
          const auto index = descriptor.dimension == TextureDimension::ThreeD
                                 ? sub.mip_level
                                 : sub.mip_level + sub.array_layer * mip_levels;
          D3D12_TEXTURE_COPY_LOCATION source{};
          source.pResource = upload.resource();
          source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
          source.PlacedFootprint = footprints[index];
          D3D12_TEXTURE_COPY_LOCATION destination{};
          destination.pResource = resource_.Get();
          destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
          destination.SubresourceIndex = index;
          list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource_.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
      })) {
    error_ = queue.error(); reset(); return false;
  }
  return true;
}

void TextureImage::reset() noexcept {
  resource_.Reset();
  format_ = DXGI_FORMAT_UNKNOWN;
}

}  // namespace xenon::gpu::d3d12

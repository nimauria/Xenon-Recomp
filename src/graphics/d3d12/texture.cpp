#include "xenon/gpu/d3d12/texture.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <vector>

#include "xenon/gpu/d3d12/memory.hpp"
#include "xenon/gpu/gpu_texture_load.hpp"
#include "xenon/memory/types.hpp"

#ifdef XENON_HAS_DXC
#include "xenon/gpu/dxc_shader_compiler.hpp"
#endif

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

namespace {

std::uint32_t array_layers(const TextureDescriptor& descriptor) noexcept {
  return descriptor.dimension == TextureDimension::Cube ? 6u
         : descriptor.dimension == TextureDimension::TwoDOrStacked && descriptor.stacked
             ? descriptor.depth
             : 1u;
}

}  // namespace

bool TextureImage::create(ID3D12Device* device, const TextureLayout& layout) {
  reset();
  error_.clear();
  if (!device || !layout.valid) {
    error_ = "D3D12 texture image requires a valid texture layout and a device";
    return false;
  }
  const auto& descriptor = layout.descriptor;
  format_ = host_texture_format(layout.format.host_format);
  if (format_ == DXGI_FORMAT_UNKNOWN) {
    error_ = "Xenos texture format has no D3D12 image mapping";
    unsupported_format_ = true;
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
  if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &format_support,
                                         sizeof(format_support))) ||
      !(format_support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) ||
      !(format_support.Support1 & dimension_support)) {
    error_ = "D3D12 device cannot sample the mapped Xenos texture format";
    reset();
    return false;
  }
  mip_levels_ = std::uint32_t(descriptor.mip_max_level) + 1u;
  const auto layers = array_layers(descriptor);
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
  image.MipLevels = static_cast<UINT16>(mip_levels_);
  image.Format = format_;
  image.SampleDesc.Count = 1;
  image.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &image,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&resource_)))) {
    error_ = "CreateCommittedResource failed for Xenos texture";
    reset();
    return false;
  }
  state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  const auto subresource_count = descriptor.dimension == TextureDimension::ThreeD
                                     ? mip_levels_
                                     : mip_levels_ * layers;
  footprints_.resize(subresource_count);
  rows_.resize(subresource_count);
  std::vector<UINT64> row_sizes(subresource_count);
  device->GetCopyableFootprints(&image, 0, subresource_count, 0, footprints_.data(),
                                rows_.data(), row_sizes.data(), &upload_size_);
  descriptor_ = descriptor;
  return true;
}

bool TextureImage::matches(const TextureLayout& layout) const noexcept {
  const auto& d = layout.descriptor;
  return resource_ && format_ == host_texture_format(layout.format.host_format) &&
         d.width == descriptor_.width && d.height == descriptor_.height &&
         d.depth == descriptor_.depth && d.dimension == descriptor_.dimension &&
         d.stacked == descriptor_.stacked && d.mip_max_level == descriptor_.mip_max_level;
}

std::uint32_t TextureImage::subresource_index(const TextureSubresourceLayout& sub) const noexcept {
  return descriptor_.dimension == TextureDimension::ThreeD
             ? sub.mip_level
             : sub.mip_level + sub.array_layer * mip_levels_;
}

bool TextureImage::initialize(ID3D12Device* device, CommandQueue& queue,
                              const DecodedTexture& texture) {
  if (!texture.valid) {
    reset();
    error_ = "D3D12 texture image requires decoded texture data and a device";
    return false;
  }
  if (!create(device, texture.layout)) return false;
  Buffer upload;
  if (!upload.initialize(device, upload_size_, D3D12_HEAP_TYPE_UPLOAD,
                         D3D12_RESOURCE_STATE_GENERIC_READ)) {
    error_ = upload.error();
    reset();
    return false;
  }
  std::span<std::byte> mapped;
  if (!upload.map(mapped)) {
    error_ = upload.error();
    reset();
    return false;
  }
  std::fill(mapped.begin(), mapped.end(), std::byte{});
  for (const auto& sub : texture.layout.subresources) {
    const auto& footprint = footprints_[subresource_index(sub)];
    const auto source_slice = std::uint64_t(sub.linear_row_pitch_bytes) * sub.height_blocks;
    const auto destination_slice =
        std::uint64_t(footprint.Footprint.RowPitch) * rows_[subresource_index(sub)];
    for (std::uint32_t z = 0; z < sub.depth_texels; ++z)
      for (std::uint32_t y = 0; y < sub.height_blocks; ++y)
        std::memcpy(mapped.data() + footprint.Offset + z * destination_slice +
                        std::uint64_t(y) * footprint.Footprint.RowPitch,
                    texture.linear_data.data() + sub.linear_offset_bytes + z * source_slice +
                        std::uint64_t(y) * sub.linear_row_pitch_bytes,
                    sub.linear_row_pitch_bytes);
  }
  upload.unmap();
  if (!queue.execute([&](ID3D12GraphicsCommandList* list) {
        for (const auto& sub : texture.layout.subresources) {
          const auto index = subresource_index(sub);
          D3D12_TEXTURE_COPY_LOCATION source{};
          source.pResource = upload.resource();
          source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
          source.PlacedFootprint = footprints_[index];
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
    error_ = queue.error();
    reset();
    return false;
  }
  state_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  return true;
}

void TextureImage::reset() noexcept {
  resource_.Reset();
  format_ = DXGI_FORMAT_UNKNOWN;
  footprints_.clear();
  rows_.clear();
  upload_size_ = 0;
  descriptor_ = {};
  mip_levels_ = 0;
  state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  unsupported_format_ = false;
}

bool GpuTextureLoader::initialize(ID3D12Device* device) {
  if (pipeline_ && device == device_) return true;
#ifndef XENON_HAS_DXC
  (void)device;
  error_ = "D3D12 GPU texture loading requires DXC";
  return false;
#else
  reset();
  device_ = device;
  std::array<D3D12_ROOT_PARAMETER, 3> parameters{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants.Num32BitValues = sizeof(GpuTextureLoadConstants) / 4u;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // guest bytes
  parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;  // staging
  D3D12_ROOT_SIGNATURE_DESC desc{};
  desc.NumParameters = static_cast<UINT>(parameters.size());
  desc.pParameters = parameters.data();
  Microsoft::WRL::ComPtr<ID3DBlob> serialized;
  Microsoft::WRL::ComPtr<ID3DBlob> diagnostics;
  if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized,
                                         &diagnostics)) ||
      FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                         serialized->GetBufferSize(), IID_PPV_ARGS(&root_)))) {
    error_ = "D3D12 texture load root signature creation failed";
    return false;
  }
  UtilityShaderCompiler compiler;
  const auto cs = compiler.compile(make_gpu_texture_load_shader());
  if (!cs.succeeded) {
    error_ = cs.diagnostics.empty() ? "D3D12 texture load shader compilation failed"
                                    : cs.diagnostics.front();
    return false;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
  pipeline.pRootSignature = root_.Get();
  pipeline.CS = {cs.binary.data(), cs.binary.size()};
  if (FAILED(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pipeline_)))) {
    error_ = "D3D12 texture load pipeline creation failed";
    return false;
  }
  if (!staging_.initialize(device, kStagingSize, D3D12_HEAP_TYPE_DEFAULT,
                           D3D12_RESOURCE_STATE_COPY_SOURCE,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
      !upload_.initialize(device, kUploadSize, D3D12_HEAP_TYPE_UPLOAD,
                          D3D12_RESOURCE_STATE_GENERIC_READ)) {
    error_ = "D3D12 texture load staging allocation failed";
    pipeline_.Reset();
    return false;
  }
  staging_state_ = D3D12_RESOURCE_STATE_COPY_SOURCE;
  staging_ring_.reset(kStagingSize);
  upload_ring_.reset(kUploadSize);
  return true;
#endif
}

void GpuTextureLoader::reset() noexcept {
  transients_.clear();
  staging_.reset();
  upload_.reset();
  staging_ring_.reset(0);
  upload_ring_.reset(0);
  pipeline_.Reset();
  root_.Reset();
  device_ = nullptr;
}

void GpuTextureLoader::retire(std::uint64_t completed) {
  std::erase_if(transients_, [&](const Transient& t) { return t.value <= completed; });
}

bool GpuTextureLoader::load_from_mirror(ID3D12Device* device, CommandQueue& queue,
                                        GuestMemoryMirror& mirror, TextureImage& image,
                                        const TextureLayout& layout) {
  if (!initialize(device)) return false;
  if (!mirror.resource()) {
    error_ = "D3D12 texture load has no guest-memory mirror";
    return false;
  }
  return run(queue, image, layout, mirror.resource()->GetGPUVirtualAddress(), 0u,
             memory::kPhysicalMemorySize,
             [&](ID3D12GraphicsCommandList* list) { mirror.prepare_shader_access(list, false); });
}

bool GpuTextureLoader::load_from_bytes(ID3D12Device* device, CommandQueue& queue,
                                       TextureImage& image, const TextureLayout& layout,
                                       std::span<const std::byte> bytes, std::uint32_t base) {
  if (!initialize(device)) return false;
  if (bytes.empty() || bytes.size() > memory::kPhysicalMemorySize) {
    error_ = "D3D12 texture load has no guest bytes to read";
    return false;
  }
  retire(queue.completed_value());
  const auto size = (bytes.size() + 3u) & ~std::size_t{3u};
  ID3D12Resource* buffer = nullptr;
  std::uint64_t offset = 0;
  if (size <= kUploadSize) {
    const auto reserved = upload_ring_.reserve(size, queue.completed_value(), [&](std::uint64_t v) {
      return queue.flush() && queue.wait_for_completion(v);
    });
    if (!reserved) {
      error_ = queue.error().empty() ? "D3D12 texture upload ring reservation failed"
                                     : queue.error();
      return false;
    }
    offset = *reserved;
    buffer = upload_.resource();
    std::span<std::byte> mapped;
    if (!upload_.map(mapped)) {
      error_ = upload_.error();
      return false;
    }
    std::memcpy(mapped.data() + offset, bytes.data(), bytes.size());
    upload_ring_.commit(offset, size, queue.retirement_value());
  } else {
    auto transient = std::make_unique<Buffer>();
    std::span<std::byte> mapped;
    if (!transient->initialize(device, size, D3D12_HEAP_TYPE_UPLOAD,
                               D3D12_RESOURCE_STATE_GENERIC_READ) ||
        !transient->map(mapped)) {
      error_ = transient->error();
      return false;
    }
    std::memcpy(mapped.data(), bytes.data(), bytes.size());
    buffer = transient->resource();
    transients_.push_back({std::move(transient), queue.retirement_value()});
  }
  return run(queue, image, layout, buffer->GetGPUVirtualAddress() + offset, base,
             static_cast<std::uint32_t>(bytes.size()), {});
}

bool GpuTextureLoader::run(CommandQueue& queue, TextureImage& image, const TextureLayout& layout,
                           D3D12_GPU_VIRTUAL_ADDRESS source, std::uint32_t source_base,
                           std::uint32_t source_limit,
                           const std::function<void(ID3D12GraphicsCommandList*)>& prepare_source) {
  constexpr std::uint64_t kFootprintAlignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
  const auto block_height = std::uint32_t(layout.format.block_height);
  const auto image_state = image.state();
  const auto wait = [&](std::uint64_t value) {
    return queue.flush() && queue.wait_for_completion(value);
  };
  bool first = true;
  for (const auto& sub : layout.subresources) {
    if (sub.guest_address < source_base) {
      error_ = "Xenos texture lies outside the guest bytes it is loaded from";
      return false;
    }
    const auto index = image.subresource_index(sub);
    const auto& footprint = image.footprint(index);
    const auto row_pitch = footprint.Footprint.RowPitch;
    if (row_pitch < sub.width_blocks * layout.format.host_bytes_per_block()) {
      error_ = "D3D12 texture footprint is narrower than the Xenos subresource";
      return false;
    }
    for (const auto& chunk : plan_gpu_texture_load_chunks(sub, row_pitch, kStagingSize / 2u)) {
      auto constants = make_gpu_texture_load_constants(layout, sub, chunk, row_pitch);
      const std::uint64_t bytes = std::uint64_t(row_pitch) * chunk.rows * chunk.slices;
      const auto reserved = staging_ring_.reserve(bytes + kFootprintAlignment,
                                                  queue.completed_value(), wait);
      if (!reserved) {
        error_ = queue.error().empty() ? "D3D12 texture staging reservation failed"
                                       : queue.error();
        return false;
      }
      const auto offset = (*reserved + kFootprintAlignment - 1u) / kFootprintAlignment *
                          kFootprintAlignment;
      constants.source_offset = sub.guest_address - source_base;
      constants.source_limit = source_limit;
      constants.destination_offset = static_cast<std::uint32_t>(offset);
      // The chunk's rows as a placed footprint, copied to its place in the
      // subresource.
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed = footprint;
      placed.Offset = offset;
      const auto top = chunk.row_start * block_height;
      placed.Footprint.Height = (std::min)(chunk.rows * block_height,
                                           footprint.Footprint.Height - top);
      placed.Footprint.Depth = chunk.slices;
      const bool to_copy_dest = first && image_state != D3D12_RESOURCE_STATE_COPY_DEST;
      first = false;
      if (!queue.execute_async([&](ID3D12GraphicsCommandList* list, std::uint32_t,
                                   std::uint32_t) {
            if (prepare_source) prepare_source(list);
            std::array<D3D12_RESOURCE_BARRIER, 2> barriers{};
            UINT count = 0;
            if (staging_state_ != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
              auto& b = barriers[count++];
              b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
              b.Transition.pResource = staging_.resource();
              b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
              b.Transition.StateBefore = staging_state_;
              b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }
            if (to_copy_dest) {
              auto& b = barriers[count++];
              b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
              b.Transition.pResource = image.resource();
              b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
              b.Transition.StateBefore = image_state;
              b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            }
            if (count) list->ResourceBarrier(count, barriers.data());
            list->SetPipelineState(pipeline_.Get());
            list->SetComputeRootSignature(root_.Get());
            list->SetComputeRoot32BitConstants(0, sizeof(constants) / 4u, &constants, 0);
            list->SetComputeRootShaderResourceView(1, source);
            list->SetComputeRootUnorderedAccessView(2, staging_.resource()->GetGPUVirtualAddress());
            list->Dispatch((constants.row_dwords + 7u) / 8u, (constants.rows + 7u) / 8u,
                           constants.slices);
            D3D12_RESOURCE_BARRIER to_copy{};
            to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            to_copy.Transition.pResource = staging_.resource();
            to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &to_copy);
            D3D12_TEXTURE_COPY_LOCATION from{};
            from.pResource = staging_.resource();
            from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            from.PlacedFootprint = placed;
            D3D12_TEXTURE_COPY_LOCATION to{};
            to.pResource = image.resource();
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.SubresourceIndex = index;
            list->CopyTextureRegion(&to, 0, top, chunk.z_start, &from, nullptr);
          })) {
        error_ = queue.error();
        return false;
      }
      staging_state_ = D3D12_RESOURCE_STATE_COPY_SOURCE;
      staging_ring_.commit(*reserved, bytes + kFootprintAlignment, queue.retirement_value());
    }
  }
  const auto sampled =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  if (!queue.execute_async([&](ID3D12GraphicsCommandList* list, std::uint32_t, std::uint32_t) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = image.resource();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = sampled;
        list->ResourceBarrier(1, &barrier);
      })) {
    error_ = queue.error();
    return false;
  }
  image.set_state(sampled);
  return true;
}

}  // namespace xenon::gpu::d3d12

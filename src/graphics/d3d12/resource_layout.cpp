#include "xenon/gpu/d3d12/resource_layout.hpp"

#include <algorithm>
#include <array>
#include <span>

namespace xenon::gpu::d3d12 {
namespace {

D3D12_SHADER_RESOURCE_VIEW_DESC null_texture_view(D3D12_SRV_DIMENSION dimension) {
  D3D12_SHADER_RESOURCE_VIEW_DESC view{};
  view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  view.ViewDimension = dimension;
  view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  switch (dimension) {
    case D3D12_SRV_DIMENSION_TEXTURE1D:
      view.Texture1D.MipLevels = 1;
      break;
    case D3D12_SRV_DIMENSION_TEXTURE2D:
      view.Texture2D.MipLevels = 1;
      break;
    case D3D12_SRV_DIMENSION_TEXTURE3D:
      view.Texture3D.MipLevels = 1;
      break;
    case D3D12_SRV_DIMENSION_TEXTURECUBE:
      view.TextureCube.MipLevels = 1;
      break;
    default:
      break;
  }
  return view;
}

UINT component_mapping(std::uint16_t swizzle, std::uint32_t component) {
  switch ((swizzle >> (component * 3u)) & 7u) {
    case 0: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0;
    case 1: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1;
    case 2: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2;
    case 3: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3;
    case 4: return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
    case 5: return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1;
    default: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0 + component;
  }
}

D3D12_FILTER basic_filter(const TextureDescriptor& state) {
  const bool min = state.min_filter != 0;
  const bool mag = state.mag_filter != 0;
  const bool mip = state.mip_filter != 0;
  if (min && mag && mip) return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  if (min && mag) return D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  if (min && mip) return D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
  if (mag && mip) return D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR;
  if (min) return D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT;
  if (mag) return D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
  if (mip) return D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
  return D3D12_FILTER_MIN_MAG_MIP_POINT;
}

UINT max_anisotropy(std::uint8_t filter) {
  if (!filter) return 1;
  return (std::min)(16u, 1u << (std::min)(4u, std::uint32_t(filter - 1u)));
}

}  // namespace

bool ResourceLayout::initialize(ID3D12Device* device) {
  reset();
  error_.clear();
  if (!device) {
    error_ = "D3D12 resource layout requires a device";
    return false;
  }
  device_ = device;

  const D3D12_DESCRIPTOR_RANGE srv_range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                                         kSrvCount, 0, 0, 0};
  const D3D12_DESCRIPTOR_RANGE sampler_range{
      D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, kSamplerCount, 0, 0, 0};
  const D3D12_DESCRIPTOR_RANGE uav_range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
                                         kUavCount, 0, 0, 0};
  std::array<D3D12_ROOT_PARAMETER, 4> parameters{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  parameters[0].Descriptor.ShaderRegister = 0;
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[1].DescriptorTable = {1, &srv_range};
  parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[2].DescriptorTable = {1, &sampler_range};
  parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[3].DescriptorTable = {1, &uav_range};
  parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC root_desc{};
  root_desc.NumParameters = static_cast<UINT>(parameters.size());
  root_desc.pParameters = parameters.data();
  root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  Microsoft::WRL::ComPtr<ID3DBlob> serialized;
  Microsoft::WRL::ComPtr<ID3DBlob> messages;
  if (FAILED(D3D12SerializeRootSignature(&root_desc,
                                         D3D_ROOT_SIGNATURE_VERSION_1,
                                         &serialized, &messages)) ||
      FAILED(device_->CreateRootSignature(0, serialized->GetBufferPointer(),
                                          serialized->GetBufferSize(),
                                          IID_PPV_ARGS(&root_signature_)))) {
    error_ = messages ? static_cast<const char*>(messages->GetBufferPointer())
                      : "D3D12 root signature creation failed";
    reset();
    return false;
  }

  resource_increment_ = device_->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  sampler_increment_ = device_->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

  auto create_heap = [&](D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count,
                         D3D12_DESCRIPTOR_HEAP_FLAGS flags,
                         Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>& heap) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type;
    desc.NumDescriptors = count;
    desc.Flags = flags;
    return SUCCEEDED(device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)));
  };
  if (!create_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kResourceCount,
                   D3D12_DESCRIPTOR_HEAP_FLAG_NONE, staging_resource_heap_) ||
      !create_heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerCount,
                   D3D12_DESCRIPTOR_HEAP_FLAG_NONE, staging_sampler_heap_)) {
    error_ = "D3D12 staging descriptor heap creation failed";
    reset();
    return false;
  }
  for (auto& frame : frames_) {
    for (auto& draw : frame.draws) {
      if (!create_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kResourceCount,
                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
                       draw.resource_heap) ||
          !create_heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerCount,
                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
                       draw.sampler_heap) ||
          !draw.constants.initialize(device_, kConstantBufferBytes,
                                     D3D12_HEAP_TYPE_UPLOAD,
                                     D3D12_RESOURCE_STATE_GENERIC_READ) ||
          !draw.constants.map(draw.constants_mapping)) {
        error_ = draw.constants.error().empty()
                     ? "D3D12 draw resource snapshot creation failed"
                     : draw.constants.error();
        reset();
        return false;
      }
    }
  }

  auto resource_handle = staging_resource_heap_->GetCPUDescriptorHandleForHeapStart();
  D3D12_SHADER_RESOURCE_VIEW_DESC null_buffer{};
  null_buffer.Format = DXGI_FORMAT_R32_TYPELESS;
  null_buffer.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  null_buffer.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  null_buffer.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
  device_->CreateShaderResourceView(nullptr, &null_buffer, resource_handle);
  constexpr std::array dimensions{D3D12_SRV_DIMENSION_TEXTURE1D,
                                   D3D12_SRV_DIMENSION_TEXTURE2D,
                                   D3D12_SRV_DIMENSION_TEXTURE3D,
                                   D3D12_SRV_DIMENSION_TEXTURECUBE};
  for (const auto dimension : dimensions) {
    const auto view = null_texture_view(dimension);
    for (std::uint32_t i = 0; i < kTextureCount; ++i) {
      resource_handle.ptr += resource_increment_;
      device_->CreateShaderResourceView(nullptr, &view, resource_handle);
    }
  }
  resource_handle = staging_resource_heap_->GetCPUDescriptorHandleForHeapStart();
  resource_handle.ptr += std::uint64_t(kMemoryExportDescriptor) * resource_increment_;
  D3D12_UNORDERED_ACCESS_VIEW_DESC null_uav{};
  null_uav.Format = DXGI_FORMAT_R32_TYPELESS;
  null_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  null_uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  device_->CreateUnorderedAccessView(nullptr, nullptr, &null_uav, resource_handle);

  auto sampler_handle = staging_sampler_heap_->GetCPUDescriptorHandleForHeapStart();
  D3D12_SAMPLER_DESC sampler{};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  for (std::uint32_t i = 0; i < kSamplerCount; ++i) {
    device_->CreateSampler(&sampler, sampler_handle);
    sampler_handle.ptr += sampler_increment_;
  }

  std::fill(constants_staging_.begin(), constants_staging_.end(), std::byte{});
  for (std::uint32_t frame = 0; frame < kFrameCount; ++frame)
    for (std::uint32_t draw = 0; draw < CommandQueue::kBatchCommandCount; ++draw)
      prepare_draw(frame, draw);
  return true;
}

bool ResourceLayout::bind_texture(std::uint32_t slot, TextureDimension dimension,
                                  ID3D12Resource* resource, DXGI_FORMAT format,
                                  std::uint32_t mip_levels,
                                  const TextureDescriptor& state) {
  if (!device_ || !staging_resource_heap_ || !staging_sampler_heap_ || slot >= kTextureCount ||
      !resource || format == DXGI_FORMAT_UNKNOWN || !mip_levels) {
    error_ = "D3D12 texture binding requires a valid slot and native image";
    return false;
  }
  std::uint32_t descriptor_index{};
  D3D12_SHADER_RESOURCE_VIEW_DESC view{};
  view.Format = format == DXGI_FORMAT_R24G8_TYPELESS
                    ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : format;
  view.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
      component_mapping(state.swizzle, 0), component_mapping(state.swizzle, 1),
      component_mapping(state.swizzle, 2), component_mapping(state.swizzle, 3));
  switch (dimension) {
    case TextureDimension::OneD:
      descriptor_index = 1u + slot;
      view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
      view.Texture1D.MipLevels = mip_levels;
      break;
    case TextureDimension::TwoDOrStacked:
      descriptor_index = 33u + slot;
      view.ViewDimension = state.stacked ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY
                                         : D3D12_SRV_DIMENSION_TEXTURE2D;
      if (state.stacked) {
        view.Texture2DArray.MipLevels = mip_levels;
        view.Texture2DArray.ArraySize = state.depth;
      } else view.Texture2D.MipLevels = mip_levels;
      break;
    case TextureDimension::ThreeD:
      descriptor_index = 65u + slot;
      view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
      view.Texture3D.MipLevels = mip_levels;
      break;
    case TextureDimension::Cube:
      descriptor_index = 97u + slot;
      view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
      view.TextureCube.MipLevels = mip_levels;
      break;
  }
  auto resource_handle = staging_resource_heap_->GetCPUDescriptorHandleForHeapStart();
  resource_handle.ptr += std::uint64_t(descriptor_index) * resource_increment_;
  device_->CreateShaderResourceView(resource, &view, resource_handle);

  const auto address_mode = [](std::uint8_t clamp) {
    switch (clamp) {
      case 0: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
      case 1: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
      case 2: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
      case 3: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
      default: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    }
  };
  D3D12_SAMPLER_DESC sampler{};
  sampler.Filter = state.aniso_filter ? D3D12_FILTER_ANISOTROPIC
                                      : basic_filter(state);
  sampler.AddressU = address_mode(state.clamps[0]);
  sampler.AddressV = address_mode(state.clamps[1]);
  sampler.AddressW = address_mode(state.clamps[2]);
  sampler.MipLODBias = float(state.lod_bias) / 32.0f;
  sampler.MaxAnisotropy = max_anisotropy(state.aniso_filter);
  sampler.MinLOD = float(state.mip_min_level);
  sampler.MaxLOD = float(state.mip_max_level);
  auto sampler_handle = staging_sampler_heap_->GetCPUDescriptorHandleForHeapStart();
  sampler_handle.ptr += std::uint64_t(slot) * sampler_increment_;
  device_->CreateSampler(&sampler, sampler_handle);
  return true;
}

bool ResourceLayout::bind_guest_memory(ID3D12Resource* resource,
                                       std::uint64_t size) {
  if (!device_ || !staging_resource_heap_ || !resource || !size || (size & 3u)) {
    error_ = "D3D12 guest-memory SRV requires an initialized layout and aligned buffer";
    return false;
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC view{};
  view.Format = DXGI_FORMAT_R32_TYPELESS;
  view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  view.Buffer.NumElements = static_cast<UINT>(size / 4u);
  view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
  device_->CreateShaderResourceView(
      resource, &view, staging_resource_heap_->GetCPUDescriptorHandleForHeapStart());

  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.Format = DXGI_FORMAT_R32_TYPELESS;
  uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  uav.Buffer.NumElements = static_cast<UINT>(size / 4u);
  uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  auto uav_handle = staging_resource_heap_->GetCPUDescriptorHandleForHeapStart();
  uav_handle.ptr +=
      std::uint64_t(kMemoryExportDescriptor) * resource_increment_;
  device_->CreateUnorderedAccessView(resource, nullptr, &uav, uav_handle);
  return true;
}

void ResourceLayout::prepare_draw(std::uint32_t frame_index,
                                  std::uint32_t draw_slot) {
  if (!device_ || frame_index >= frames_.size() ||
      draw_slot >= CommandQueue::kBatchCommandCount)
    return;
  auto& draw = frames_[frame_index].draws[draw_slot];
  device_->CopyDescriptorsSimple(
      kResourceCount, draw.resource_heap->GetCPUDescriptorHandleForHeapStart(),
      staging_resource_heap_->GetCPUDescriptorHandleForHeapStart(),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  device_->CopyDescriptorsSimple(
      kSamplerCount, draw.sampler_heap->GetCPUDescriptorHandleForHeapStart(),
      staging_sampler_heap_->GetCPUDescriptorHandleForHeapStart(),
      D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  std::copy(constants_staging_.begin(), constants_staging_.end(),
            draw.constants_mapping.begin());
}

D3D12_GPU_DESCRIPTOR_HANDLE ResourceLayout::memory_export_handle(
    std::uint32_t frame_index, std::uint32_t draw_slot) const noexcept {
  if (frame_index >= frames_.size() ||
      draw_slot >= CommandQueue::kBatchCommandCount ||
      !frames_[frame_index].draws[draw_slot].resource_heap)
    return {};
  auto handle = frames_[frame_index]
                    .draws[draw_slot]
                    .resource_heap->GetGPUDescriptorHandleForHeapStart();
  handle.ptr += std::uint64_t(kMemoryExportDescriptor) * resource_increment_;
  return handle;
}

void ResourceLayout::reset() noexcept {
  for (auto& frame : frames_) {
    for (auto& draw : frame.draws) {
      draw.constants_mapping = {};
      draw.constants.reset();
      draw.sampler_heap.Reset();
      draw.resource_heap.Reset();
    }
  }
  staging_sampler_heap_.Reset();
  staging_resource_heap_.Reset();
  root_signature_.Reset();
  constants_staging_.fill(std::byte{});
  resource_increment_ = 0;
  sampler_increment_ = 0;
  device_ = nullptr;
}

}  // namespace xenon::gpu::d3d12

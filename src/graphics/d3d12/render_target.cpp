#include "xenon/gpu/d3d12/render_target.hpp"

#include <algorithm>
#include <cstring>
#include <span>

#include "xenon/gpu/d3d12/memory.hpp"
#ifdef XENON_HAS_DXC
#include "xenon/gpu/dxc_shader_compiler.hpp"
#include "xenon/gpu/shader_translation.hpp"
#endif

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

#ifdef XENON_HAS_DXC
bool create_sample_transfer_pipeline(
    ID3D12Device* device, DXGI_FORMAT format, MsaaSamples source_samples,
    bool write, Microsoft::WRL::ComPtr<ID3D12RootSignature>& root_signature,
    Microsoft::WRL::ComPtr<ID3D12PipelineState>& pipeline,
    std::string& error) {
  DxcShaderCompiler compiler;
  const auto vs = compiler.compile(make_transfer_fullscreen_vertex_shader());
  const auto ps = compiler.compile(write ? make_color_sample_write_shader()
                                         : make_color_sample_read_shader(source_samples));
  if (!vs.succeeded || !ps.succeeded) {
    error = !vs.succeeded && !vs.diagnostics.empty() ? vs.diagnostics.front()
            : !ps.diagnostics.empty() ? ps.diagnostics.front()
                                      : "D3D12 sample transfer shader compilation failed";
    return false;
  }

  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 1;
  range.BaseShaderRegister = 0;
  range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
  std::array<D3D12_ROOT_PARAMETER, 2> parameters{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  parameters[0].Descriptor.ShaderRegister = 0;
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[1].DescriptorTable.NumDescriptorRanges = 1;
  parameters[1].DescriptorTable.pDescriptorRanges = &range;
  parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC root_desc{};
  root_desc.NumParameters = static_cast<UINT>(parameters.size());
  root_desc.pParameters = parameters.data();
  root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  Microsoft::WRL::ComPtr<ID3DBlob> serialized;
  Microsoft::WRL::ComPtr<ID3DBlob> diagnostics;
  if (FAILED(D3D12SerializeRootSignature(
          &root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
          &serialized, &diagnostics)) ||
      FAILED(device->CreateRootSignature(
          0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
          IID_PPV_ARGS(&root_signature)))) {
    error = diagnostics ? std::string(
        static_cast<const char*>(diagnostics->GetBufferPointer()),
        diagnostics->GetBufferSize()) : "D3D12 sample transfer root signature failed";
    return false;
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root_signature.Get();
  desc.VS = {vs.binary.data(), vs.binary.size()};
  desc.PS = {ps.binary.data(), ps.binary.size()};
  desc.BlendState.AlphaToCoverageEnable = FALSE;
  desc.BlendState.IndependentBlendEnable = FALSE;
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.DepthStencilState.DepthEnable = FALSE;
  desc.DepthStencilState.StencilEnable = FALSE;
  desc.InputLayout = {nullptr, 0};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = format;
  desc.SampleDesc.Count = write
      ? 1u << static_cast<unsigned>(source_samples) : 1u;
  if (FAILED(device->CreateGraphicsPipelineState(&desc,
                                                  IID_PPV_ARGS(&pipeline)))) {
    error = "D3D12 sample transfer pipeline creation failed";
    return false;
  }
  return true;
}

bool create_shader_visible_srv_heap(
    ID3D12Device* device, ID3D12Resource* resource, DXGI_FORMAT format,
    bool multisampled,
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>& heap) {
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap_desc.NumDescriptors = 1;
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap))))
    return false;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = format;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.ViewDimension = multisampled ? D3D12_SRV_DIMENSION_TEXTURE2DMS
                                    : D3D12_SRV_DIMENSION_TEXTURE2D;
  if (!multisampled) srv.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(resource, &srv,
                                   heap->GetCPUDescriptorHandleForHeapStart());
  return true;
}
#endif

}  // namespace

bool RenderTargetImage::initialize(ID3D12Device* device,
                                   const EdramSurfaceLayout& surface,
                                   ColorRenderTargetFormat color_format,
                                   bool native_2x_supported) {
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
  host_msaa_ = surface.msaa;
  if (surface.msaa == MsaaSamples::X2) {
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{};
    levels.Format = format_;
    levels.SampleCount = 2;
    levels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
    const bool supports_native_2x = SUCCEEDED(device->CheckFeatureSupport(
        D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels,
        sizeof(levels))) && levels.NumQualityLevels;
    if (!native_2x_supported || !supports_native_2x)
      host_msaa_ = MsaaSamples::X4;
  }
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = surface.pitch_pixels;
  desc.Height = surface.height_pixels;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format_;
  desc.SampleDesc.Count = 1u << static_cast<unsigned>(host_msaa_);
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

bool RenderTargetImage::readback_sample(
    CommandQueue& queue, std::uint32_t guest_sample, std::uint32_t left,
    std::uint32_t top, std::uint32_t right, std::uint32_t bottom,
    std::vector<std::byte>& destination, std::uint32_t& row_pitch) {
  destination.clear();
  row_pitch = 0;
  const auto mapping = map_guest_sample_to_host(
      surface_.msaa, guest_sample, host_msaa_ == MsaaSamples::X2);
  if (!mapping || !resource_ || !bytes_per_pixel_ || left >= right ||
      top >= bottom || right > width_ || bottom > height_) {
    error_ = "invalid D3D12 selected-sample readback";
    return false;
  }
  if (surface_.msaa == MsaaSamples::X1)
    return readback(queue, left, top, right, bottom, destination, row_pitch);
#ifndef XENON_HAS_DXC
  error_ = "D3D12 selected-sample readback requires DXC";
  return false;
#else
  Microsoft::WRL::ComPtr<ID3D12Device> device;
  if (FAILED(resource_->GetDevice(IID_PPV_ARGS(&device)))) {
    error_ = "failed to obtain D3D12 device for selected-sample readback";
    return false;
  }
  const auto copy_width = right - left;
  const auto copy_height = bottom - top;
  row_pitch = copy_width * bytes_per_pixel_;
  const auto native_row_pitch = (row_pitch + 255u) & ~255u;

  D3D12_RESOURCE_DESC target_desc{};
  target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  target_desc.Width = copy_width;
  target_desc.Height = copy_height;
  target_desc.DepthOrArraySize = 1;
  target_desc.MipLevels = 1;
  target_desc.Format = format_;
  target_desc.SampleDesc.Count = 1;
  target_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_HEAP_PROPERTIES default_heap{};
  default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  default_heap.CreationNodeMask = default_heap.VisibleNodeMask = 1;
  Microsoft::WRL::ComPtr<ID3D12Resource> target;
  if (FAILED(device->CreateCommittedResource(
          &default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
          D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
          IID_PPV_ARGS(&target)))) {
    error_ = "D3D12 selected-sample readback target creation failed";
    return false;
  }
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
  rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_desc.NumDescriptors = 1;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap;
  if (FAILED(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)))) {
    error_ = "D3D12 selected-sample readback RTV creation failed";
    return false;
  }
  const auto target_rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, target_rtv);

  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_heap;
  if (!create_shader_visible_srv_heap(device.Get(), resource_.Get(), format_,
                                      true, srv_heap)) {
    error_ = "D3D12 selected-sample readback SRV creation failed";
    return false;
  }
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
  if (!create_sample_transfer_pipeline(device.Get(), format_, host_msaa_,
                                       false, root_signature, pipeline, error_))
    return false;
  struct TransferConstants { std::uint32_t origin[2], sample, pad; };
  Buffer constants;
  if (!constants.initialize(device.Get(), 256, D3D12_HEAP_TYPE_UPLOAD,
                            D3D12_RESOURCE_STATE_GENERIC_READ)) {
    error_ = constants.error(); return false;
  }
  std::span<std::byte> mapped;
  if (!constants.map(mapped)) { error_ = constants.error(); return false; }
  const TransferConstants values{{left, top}, mapping->sample, 0};
  std::memcpy(mapped.data(), &values, sizeof(values)); constants.unmap();
  Buffer readback_buffer;
  if (!readback_buffer.initialize(device.Get(),
          std::uint64_t(native_row_pitch) * copy_height,
          D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST)) {
    error_ = readback_buffer.error(); return false;
  }

  if (!queue.execute([&](ID3D12GraphicsCommandList* list) {
        D3D12_RESOURCE_BARRIER source_barrier{};
        source_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        source_barrier.Transition.pResource = resource_.Get();
        source_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        source_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        source_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &source_barrier);
        list->SetPipelineState(pipeline.Get());
        list->SetGraphicsRootSignature(root_signature.Get());
        ID3D12DescriptorHeap* heaps[]{srv_heap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootConstantBufferView(
            0, constants.resource()->GetGPUVirtualAddress());
        list->SetGraphicsRootDescriptorTable(
            1, srv_heap->GetGPUDescriptorHandleForHeapStart());
        const D3D12_VIEWPORT viewport{0, 0, float(copy_width),
                                      float(copy_height), 0, 1};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(copy_width),
                                 static_cast<LONG>(copy_height)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &target_rtv, FALSE, nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        D3D12_RESOURCE_BARRIER target_barrier{};
        target_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        target_barrier.Transition.pResource = target.Get();
        target_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        target_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        target_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &target_barrier);
        D3D12_TEXTURE_COPY_LOCATION destination_location{};
        destination_location.pResource = readback_buffer.resource();
        destination_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination_location.PlacedFootprint.Footprint = {
            format_, copy_width, copy_height, 1, native_row_pitch};
        D3D12_TEXTURE_COPY_LOCATION source_location{};
        source_location.pResource = target.Get();
        source_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&destination_location, 0, 0, 0,
                                &source_location, nullptr);
        std::swap(source_barrier.Transition.StateBefore,
                  source_barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &source_barrier);
      })) {
    error_ = queue.error(); return false;
  }
  if (!readback_buffer.map(mapped)) {
    error_ = readback_buffer.error(); return false;
  }
  destination.resize(std::size_t(row_pitch) * copy_height);
  for (std::uint32_t y = 0; y < copy_height; ++y)
    std::memcpy(destination.data() + std::size_t(y) * row_pitch,
                mapped.data() + std::size_t(y) * native_row_pitch, row_pitch);
  readback_buffer.unmap();
  return true;
#endif
}

bool RenderTargetImage::upload_sample(
    CommandQueue& queue, std::uint32_t guest_sample, std::uint32_t left,
    std::uint32_t top, std::uint32_t right, std::uint32_t bottom,
    std::span<const std::byte> source, std::uint32_t row_pitch) {
  const auto mapping = map_guest_sample_to_host(
      surface_.msaa, guest_sample, host_msaa_ == MsaaSamples::X2);
  const auto copy_width = right > left ? right - left : 0;
  const auto copy_height = bottom > top ? bottom - top : 0;
  if (!mapping || !resource_ || !bytes_per_pixel_ || !copy_width ||
      !copy_height || right > width_ || bottom > height_ ||
      row_pitch < copy_width * bytes_per_pixel_ ||
      std::uint64_t(row_pitch) * copy_height > source.size()) {
    error_ = "invalid D3D12 selected-sample upload";
    return false;
  }
  if (surface_.msaa == MsaaSamples::X1 && left == 0 && top == 0 &&
      right == width_ && bottom == height_)
    return upload(queue, source, row_pitch);
#ifndef XENON_HAS_DXC
  error_ = "D3D12 selected-sample upload requires DXC";
  return false;
#else
  Microsoft::WRL::ComPtr<ID3D12Device> device;
  if (FAILED(resource_->GetDevice(IID_PPV_ARGS(&device)))) {
    error_ = "failed to obtain D3D12 device for selected-sample upload";
    return false;
  }
  D3D12_RESOURCE_DESC upload_desc{};
  upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  upload_desc.Width = copy_width;
  upload_desc.Height = copy_height;
  upload_desc.DepthOrArraySize = 1;
  upload_desc.MipLevels = 1;
  upload_desc.Format = format_;
  upload_desc.SampleDesc.Count = 1;
  upload_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  D3D12_HEAP_PROPERTIES default_heap{};
  default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  default_heap.CreationNodeMask = default_heap.VisibleNodeMask = 1;
  Microsoft::WRL::ComPtr<ID3D12Resource> upload_image;
  if (FAILED(device->CreateCommittedResource(
          &default_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&upload_image)))) {
    error_ = "D3D12 selected-sample upload image creation failed";
    return false;
  }
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rows{};
  UINT64 row_size{}, upload_size{};
  device->GetCopyableFootprints(&upload_desc, 0, 1, 0, &footprint, &rows,
                                &row_size, &upload_size);
  Buffer staging;
  if (!staging.initialize(device.Get(), upload_size, D3D12_HEAP_TYPE_UPLOAD,
                          D3D12_RESOURCE_STATE_GENERIC_READ)) {
    error_ = staging.error(); return false;
  }
  std::span<std::byte> mapped;
  if (!staging.map(mapped)) { error_ = staging.error(); return false; }
  std::fill(mapped.begin(), mapped.end(), std::byte{});
  for (std::uint32_t y = 0; y < copy_height; ++y)
    std::memcpy(mapped.data() + footprint.Offset +
                    std::uint64_t(y) * footprint.Footprint.RowPitch,
                source.data() + std::uint64_t(y) * row_pitch,
                copy_width * bytes_per_pixel_);
  staging.unmap();
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_heap;
  if (!create_shader_visible_srv_heap(device.Get(), upload_image.Get(), format_,
                                      false, srv_heap)) {
    error_ = "D3D12 selected-sample upload SRV creation failed";
    return false;
  }
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
  if (!create_sample_transfer_pipeline(device.Get(), format_, host_msaa_,
                                       true, root_signature, pipeline, error_))
    return false;
  struct TransferConstants { std::uint32_t origin[2], sample, pad; };
  Buffer constants;
  if (!constants.initialize(device.Get(), 256, D3D12_HEAP_TYPE_UPLOAD,
                            D3D12_RESOURCE_STATE_GENERIC_READ)) {
    error_ = constants.error(); return false;
  }
  if (!constants.map(mapped)) { error_ = constants.error(); return false; }
  const TransferConstants values{{left, top}, mapping->sample, 0};
  std::memcpy(mapped.data(), &values, sizeof(values)); constants.unmap();
  if (!queue.execute([&](ID3D12GraphicsCommandList* list) {
        D3D12_TEXTURE_COPY_LOCATION source_location{};
        source_location.pResource = staging.resource();
        source_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source_location.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION destination_location{};
        destination_location.pResource = upload_image.Get();
        destination_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&destination_location, 0, 0, 0,
                                &source_location, nullptr);
        D3D12_RESOURCE_BARRIER upload_barrier{};
        upload_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        upload_barrier.Transition.pResource = upload_image.Get();
        upload_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        upload_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        upload_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &upload_barrier);
        list->SetPipelineState(pipeline.Get());
        list->SetGraphicsRootSignature(root_signature.Get());
        ID3D12DescriptorHeap* heaps[]{srv_heap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootConstantBufferView(
            0, constants.resource()->GetGPUVirtualAddress());
        list->SetGraphicsRootDescriptorTable(
            1, srv_heap->GetGPUDescriptorHandleForHeapStart());
        const D3D12_VIEWPORT viewport{float(left), float(top), float(copy_width),
                                      float(copy_height), 0, 1};
        const D3D12_RECT scissor{static_cast<LONG>(left), static_cast<LONG>(top),
                                 static_cast<LONG>(right), static_cast<LONG>(bottom)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &rtv_, FALSE, nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
      })) {
    error_ = queue.error(); return false;
  }
  return true;
#endif
}

void RenderTargetImage::reset() noexcept {
  resource_.Reset();
  rtv_heap_.Reset();
  rtv_ = {};
  format_ = DXGI_FORMAT_UNKNOWN;
  guest_format_ = ColorRenderTargetFormat::R8G8B8A8;
  width_ = height_ = bytes_per_pixel_ = 0;
  surface_ = {};
  host_msaa_ = MsaaSamples::X1;
}

}  // namespace xenon::gpu::d3d12

#include "xenon/gpu/d3d12/depth_target.hpp"

#include <array>
#include <cstring>
#include <span>

#include "xenon/gpu/depth_format.hpp"
#include "xenon/gpu/d3d12/memory.hpp"
#ifdef XENON_HAS_DXC
#include "xenon/gpu/dxc_shader_compiler.hpp"
#include "xenon/gpu/shader_translation.hpp"
#endif

namespace xenon::gpu::d3d12 {

DXGI_FORMAT depth_render_target_format(DepthRenderTargetFormat format) noexcept {
  return format == DepthRenderTargetFormat::D24S8
             ? DXGI_FORMAT_D24_UNORM_S8_UINT
             : DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}

namespace {
DXGI_FORMAT depth_resource_format(DepthRenderTargetFormat format) noexcept {
  return format == DepthRenderTargetFormat::D24S8
             ? DXGI_FORMAT_R24G8_TYPELESS
             : DXGI_FORMAT_R32G8X24_TYPELESS;
}
DXGI_FORMAT depth_srv_format(DepthRenderTargetFormat format) noexcept {
  return format == DepthRenderTargetFormat::D24S8
             ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS
             : DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
}
DXGI_FORMAT stencil_srv_format(DepthRenderTargetFormat format) noexcept {
  return format == DepthRenderTargetFormat::D24S8
             ? DXGI_FORMAT_X24_TYPELESS_G8_UINT
             : DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
}

#ifdef XENON_HAS_DXC
enum class DepthTransferMode { Read, CombinedWrite, DepthWrite, StencilWrite };

bool create_depth_transfer_pipeline(
    ID3D12Device* device, DepthTransferMode mode, MsaaSamples samples,
    DXGI_FORMAT depth_format,
    Microsoft::WRL::ComPtr<ID3D12RootSignature>& root_signature,
    Microsoft::WRL::ComPtr<ID3D12PipelineState>& pipeline,
    std::string& error) {
  DxcShaderCompiler compiler;
  const auto vs = compiler.compile(make_transfer_fullscreen_vertex_shader());
  LoweredShader pixel;
  switch (mode) {
    case DepthTransferMode::Read: pixel = make_depth_sample_read_shader(samples); break;
    case DepthTransferMode::CombinedWrite: pixel = make_depth_sample_write_shader(); break;
    case DepthTransferMode::DepthWrite: pixel = make_depth_only_sample_write_shader(); break;
    case DepthTransferMode::StencilWrite: pixel = make_stencil_mask_write_shader(); break;
  }
  const auto ps = compiler.compile(pixel);
  if (!vs.succeeded || !ps.succeeded) {
    error = !vs.succeeded && !vs.diagnostics.empty() ? vs.diagnostics.front()
            : !ps.diagnostics.empty() ? ps.diagnostics.front()
                                      : "D3D12 depth transfer shader compilation failed";
    return false;
  }
  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 2;
  range.BaseShaderRegister = 0;
  range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
  std::array<D3D12_ROOT_PARAMETER, 2> parameters{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  parameters[0].Descriptor.ShaderRegister = 0;
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[1].DescriptorTable = {1, &range};
  parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC root_desc{};
  root_desc.NumParameters = static_cast<UINT>(parameters.size());
  root_desc.pParameters = parameters.data();
  root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  Microsoft::WRL::ComPtr<ID3DBlob> serialized;
  Microsoft::WRL::ComPtr<ID3DBlob> diagnostics;
  if (FAILED(D3D12SerializeRootSignature(&root_desc,
          D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &diagnostics)) ||
      FAILED(device->CreateRootSignature(
          0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
          IID_PPV_ARGS(&root_signature)))) {
    error = diagnostics ? std::string(
        static_cast<const char*>(diagnostics->GetBufferPointer()),
        diagnostics->GetBufferSize()) : "D3D12 depth transfer root signature failed";
    return false;
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root_signature.Get();
  desc.VS = {vs.binary.data(), vs.binary.size()};
  desc.PS = {ps.binary.data(), ps.binary.size()};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  const bool write = mode != DepthTransferMode::Read;
  desc.SampleDesc.Count = write ? 1u << static_cast<unsigned>(samples) : 1u;
  if (write) {
    desc.DSVFormat = depth_format;
    // Keep depth/stencil testing active for stencil-only replacement. Depth
    // writes are disabled below and ALWAYS prevents the mask pass from
    // disturbing the already uploaded depth plane.
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = mode == DepthTransferMode::StencilWrite
        ? D3D12_DEPTH_WRITE_MASK_ZERO : D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.StencilEnable =
        mode != DepthTransferMode::DepthWrite;
    desc.DepthStencilState.StencilReadMask = 0xFF;
    desc.DepthStencilState.StencilWriteMask = 0xFF;
    desc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    desc.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    desc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
    desc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.BackFace = desc.DepthStencilState.FrontFace;
  } else {
    desc.NumRenderTargets = 2;
    desc.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
    desc.RTVFormats[1] = DXGI_FORMAT_R8_UINT;
  }
  if (FAILED(device->CreateGraphicsPipelineState(&desc,
                                                  IID_PPV_ARGS(&pipeline)))) {
    error = "D3D12 depth transfer pipeline creation failed";
    return false;
  }
  return true;
}

bool create_srv_heap(ID3D12Device* device, ID3D12Resource* depth,
                     ID3D12Resource* stencil, DXGI_FORMAT depth_format,
                     DXGI_FORMAT stencil_format, bool multisampled,
                     Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>& heap) {
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap_desc.NumDescriptors = 2;
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap))))
    return false;
  const auto increment = device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto handle = heap->GetCPUDescriptorHandleForHeapStart();
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.ViewDimension = multisampled ? D3D12_SRV_DIMENSION_TEXTURE2DMS
                                    : D3D12_SRV_DIMENSION_TEXTURE2D;
  if (!multisampled) srv.Texture2D.MipLevels = 1;
  srv.Format = depth_format;
  device->CreateShaderResourceView(depth, &srv, handle);
  handle.ptr += increment;
  srv.Format = stencil_format;
  device->CreateShaderResourceView(stencil, &srv, handle);
  return true;
}

bool create_color_target(ID3D12Device* device, DXGI_FORMAT format,
                         std::uint32_t width, std::uint32_t height,
                         Microsoft::WRL::ComPtr<ID3D12Resource>& resource) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width; desc.Height = height;
  desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.Format = format;
  desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  return SUCCEEDED(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
      nullptr, IID_PPV_ARGS(&resource)));
}

bool create_sampled_texture(ID3D12Device* device, DXGI_FORMAT format,
                            std::uint32_t width, std::uint32_t height,
                            Microsoft::WRL::ComPtr<ID3D12Resource>& resource) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width; desc.Height = height;
  desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.Format = format;
  desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  return SUCCEEDED(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
      nullptr, IID_PPV_ARGS(&resource)));
}
#endif
}

bool DepthTargetImage::initialize(ID3D12Device* device,
                                  const EdramSurfaceLayout& surface,
                                  DepthRenderTargetFormat depth_format,
                                  bool native_2x_supported) {
  reset();
  error_.clear();
  if (!device || !surface.valid() || !surface.depth) {
    error_ = "D3D12 depth target requires a valid depth EDRAM surface";
    return false;
  }
  format_ = depth_render_target_format(depth_format);
  guest_format_ = depth_format;
  width_ = surface.pitch_pixels;
  height_ = surface.height_pixels;
  surface_ = surface;
  host_msaa_ = surface.msaa;
  if (surface.msaa == MsaaSamples::X2) {
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{};
    levels.Format = format_;
    levels.SampleCount = 2;
    const bool native_2x = SUCCEEDED(device->CheckFeatureSupport(
        D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels, sizeof(levels))) &&
        levels.NumQualityLevels;
    if (!native_2x_supported || !native_2x) host_msaa_ = MsaaSamples::X4;
  }
  float24_ = depth_format == DepthRenderTargetFormat::D24FS8;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = surface.pitch_pixels;
  desc.Height = surface.height_pixels;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = depth_resource_format(depth_format);
  desc.SampleDesc.Count = 1u << static_cast<unsigned>(host_msaa_);
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
  D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
  dsv_desc.Format = format_;
  dsv_desc.ViewDimension = desc.SampleDesc.Count > 1
                               ? D3D12_DSV_DIMENSION_TEXTURE2DMS
                               : D3D12_DSV_DIMENSION_TEXTURE2D;
  device->CreateDepthStencilView(resource_.Get(), &dsv_desc, dsv_);
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

bool DepthTargetImage::readback_sample(
    CommandQueue& queue, std::uint32_t guest_sample, std::uint32_t left,
    std::uint32_t top, std::uint32_t right, std::uint32_t bottom,
    std::vector<std::uint32_t>& destination, std::uint32_t& row_pitch) {
  const auto mapping = map_guest_sample_to_host(
      surface_.msaa, guest_sample, host_msaa_ == MsaaSamples::X2);
  if (!mapping) {
    destination.clear();
    row_pitch = 0;
    error_ = "invalid D3D12 selected depth-sample readback";
    return false;
  }
  return readback_native_sample(queue, mapping->sample, left, top, right,
                                bottom, destination, row_pitch);
}

bool DepthTargetImage::readback_native_sample(
    CommandQueue& queue, std::uint32_t host_sample, std::uint32_t left,
    std::uint32_t top, std::uint32_t right, std::uint32_t bottom,
    std::vector<std::uint32_t>& destination, std::uint32_t& row_pitch) {
  destination.clear(); row_pitch = 0;
  const auto host_sample_count = 1u << static_cast<unsigned>(host_msaa_);
  if (host_sample >= host_sample_count || !resource_ || left >= right ||
      top >= bottom || right > width_ || bottom > height_) {
    error_ = "invalid D3D12 native depth-sample readback";
    return false;
  }
#ifndef XENON_HAS_DXC
  error_ = "D3D12 native depth-sample readback requires DXC";
  return false;
#else
  Microsoft::WRL::ComPtr<ID3D12Device> device;
  if (FAILED(resource_->GetDevice(IID_PPV_ARGS(&device)))) {
    error_ = "failed to obtain D3D12 device for depth readback";
    return false;
  }
  const auto copy_width = right - left;
  const auto copy_height = bottom - top;
  const auto depth_pitch = (copy_width * 4u + 255u) & ~255u;
  const auto stencil_pitch = (copy_width + 255u) & ~255u;
  row_pitch = copy_width * 4u;
  Microsoft::WRL::ComPtr<ID3D12Resource> depth_target, stencil_target;
  if (!create_color_target(device.Get(), DXGI_FORMAT_R32_FLOAT,
                           copy_width, copy_height, depth_target) ||
      !create_color_target(device.Get(), DXGI_FORMAT_R8_UINT,
                           copy_width, copy_height, stencil_target)) {
    error_ = "D3D12 depth readback targets could not be created";
    return false;
  }
  D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
  rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_heap_desc.NumDescriptors = 2;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap;
  if (FAILED(device->CreateDescriptorHeap(&rtv_heap_desc,
                                           IID_PPV_ARGS(&rtv_heap)))) {
    error_ = "D3D12 depth readback RTV heap creation failed";
    return false;
  }
  const auto rtv_increment = device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> rtvs{};
  rtvs[0] = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  rtvs[1] = rtvs[0]; rtvs[1].ptr += rtv_increment;
  device->CreateRenderTargetView(depth_target.Get(), nullptr, rtvs[0]);
  device->CreateRenderTargetView(stencil_target.Get(), nullptr, rtvs[1]);
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_heap;
  if (!create_srv_heap(device.Get(), resource_.Get(), resource_.Get(),
                       depth_srv_format(guest_format_),
                       stencil_srv_format(guest_format_), true, srv_heap)) {
    error_ = "D3D12 depth readback SRVs could not be created";
    return false;
  }
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
  if (!create_depth_transfer_pipeline(device.Get(), DepthTransferMode::Read,
                                      host_msaa_, format_,
                                      root_signature, pipeline, error_))
    return false;
  struct Constants { std::uint32_t origin[2], sample, pad; };
  Buffer constants;
  if (!constants.initialize(device.Get(), 256, D3D12_HEAP_TYPE_UPLOAD,
                            D3D12_RESOURCE_STATE_GENERIC_READ)) {
    error_ = constants.error(); return false;
  }
  std::span<std::byte> mapped;
  if (!constants.map(mapped)) { error_ = constants.error(); return false; }
  const Constants values{{left, top}, host_sample, 0};
  std::memcpy(mapped.data(), &values, sizeof(values)); constants.unmap();
  Buffer depth_readback, stencil_readback;
  if (!depth_readback.initialize(device.Get(),
          std::uint64_t(depth_pitch) * copy_height, D3D12_HEAP_TYPE_READBACK,
          D3D12_RESOURCE_STATE_COPY_DEST) ||
      !stencil_readback.initialize(device.Get(),
          std::uint64_t(stencil_pitch) * copy_height, D3D12_HEAP_TYPE_READBACK,
          D3D12_RESOURCE_STATE_COPY_DEST)) {
    error_ = "D3D12 depth readback buffers could not be created";
    return false;
  }
  if (!queue.execute([&](ID3D12GraphicsCommandList* list) {
        D3D12_RESOURCE_BARRIER source{};
        source.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        source.Transition.pResource = resource_.Get();
        source.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        source.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        source.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &source);
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
        list->OMSetRenderTargets(2, rtvs.data(), FALSE, nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        std::array<D3D12_RESOURCE_BARRIER, 2> barriers{};
        const std::array<ID3D12Resource*, 2> targets{
            depth_target.Get(), stencil_target.Get()};
        for (std::size_t i = 0; i < 2; ++i) {
          barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          barriers[i].Transition.pResource = targets[i];
          barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
          barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        }
        list->ResourceBarrier(2, barriers.data());
        D3D12_TEXTURE_COPY_LOCATION depth_dst{};
        depth_dst.pResource = depth_readback.resource();
        depth_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        depth_dst.PlacedFootprint.Footprint = {
            DXGI_FORMAT_R32_FLOAT, copy_width, copy_height, 1, depth_pitch};
        D3D12_TEXTURE_COPY_LOCATION stencil_dst{};
        stencil_dst.pResource = stencil_readback.resource();
        stencil_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        stencil_dst.PlacedFootprint.Footprint = {
            DXGI_FORMAT_R8_UINT, copy_width, copy_height, 1, stencil_pitch};
        D3D12_TEXTURE_COPY_LOCATION depth_src{};
        depth_src.pResource = depth_target.Get();
        depth_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION stencil_src{};
        stencil_src.pResource = stencil_target.Get();
        stencil_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&depth_dst, 0, 0, 0, &depth_src, nullptr);
        list->CopyTextureRegion(&stencil_dst, 0, 0, 0, &stencil_src, nullptr);
        std::swap(source.Transition.StateBefore, source.Transition.StateAfter);
        list->ResourceBarrier(1, &source);
      })) {
    error_ = queue.error(); return false;
  }
  std::span<std::byte> depth_bytes, stencil_bytes;
  if (!depth_readback.map(depth_bytes) || !stencil_readback.map(stencil_bytes)) {
    error_ = "D3D12 depth readback mapping failed"; return false;
  }
  destination.resize(std::size_t(copy_width) * copy_height);
  for (std::uint32_t y = 0; y < copy_height; ++y) {
    for (std::uint32_t x = 0; x < copy_width; ++x) {
      float depth{};
      std::memcpy(&depth, depth_bytes.data() + std::size_t(y) * depth_pitch + x * 4, 4);
      const auto stencil = std::to_integer<std::uint8_t>(
          stencil_bytes[std::size_t(y) * stencil_pitch + x]);
      destination[std::size_t(y) * copy_width + x] =
          host_to_depth_stencil(guest_format_, depth, stencil);
    }
  }
  depth_readback.unmap(); stencil_readback.unmap();
  return true;
#endif
}

bool DepthTargetImage::upload_sample(
    CommandQueue& queue, std::uint32_t guest_sample, std::uint32_t left,
    std::uint32_t top, std::uint32_t right, std::uint32_t bottom,
    std::span<const std::uint32_t> source, std::uint32_t row_pitch) {
  const auto mapping = map_guest_sample_to_host(
      surface_.msaa, guest_sample, host_msaa_ == MsaaSamples::X2);
  const auto copy_width = right > left ? right - left : 0;
  const auto copy_height = bottom > top ? bottom - top : 0;
  if (!mapping || !resource_ || !copy_width || !copy_height ||
      right > width_ || bottom > height_ || row_pitch < copy_width * 4u ||
      row_pitch % 4u || std::uint64_t(row_pitch) * copy_height >
          source.size_bytes()) {
    error_ = "invalid D3D12 selected depth-sample upload";
    return false;
  }
#ifndef XENON_HAS_DXC
  error_ = "D3D12 selected depth-sample upload requires DXC";
  return false;
#else
  Microsoft::WRL::ComPtr<ID3D12Device> device;
  if (FAILED(resource_->GetDevice(IID_PPV_ARGS(&device)))) {
    error_ = "failed to obtain D3D12 device for depth upload";
    return false;
  }
  Microsoft::WRL::ComPtr<ID3D12Resource> depth_source, stencil_source;
  if (!create_sampled_texture(device.Get(), DXGI_FORMAT_R32_FLOAT,
                              copy_width, copy_height, depth_source) ||
      !create_sampled_texture(device.Get(), DXGI_FORMAT_R8_UINT,
                              copy_width, copy_height, stencil_source)) {
    error_ = "D3D12 depth upload textures could not be created";
    return false;
  }
  const auto make_upload = [&](ID3D12Resource* texture, std::uint32_t element_size,
                               Buffer& upload,
                               D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
                               const auto& write_element) -> bool {
    const auto desc = texture->GetDesc();
    UINT rows{}; UINT64 row_bytes{}, total{};
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows,
                                  &row_bytes, &total);
    if (!upload.initialize(device.Get(), total, D3D12_HEAP_TYPE_UPLOAD,
                           D3D12_RESOURCE_STATE_GENERIC_READ)) return false;
    std::span<std::byte> mapped;
    if (!upload.map(mapped)) return false;
    std::fill(mapped.begin(), mapped.end(), std::byte{});
    for (std::uint32_t y = 0; y < copy_height; ++y)
      for (std::uint32_t x = 0; x < copy_width; ++x)
        write_element(mapped.data() + footprint.Offset +
                          std::uint64_t(y) * footprint.Footprint.RowPitch +
                          std::uint64_t(x) * element_size,
                      source[(std::uint64_t(y) * row_pitch) / 4u + x]);
    upload.unmap();
    return true;
  };
  Buffer depth_upload, stencil_upload;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT depth_footprint{}, stencil_footprint{};
  if (!make_upload(depth_source.Get(), 4, depth_upload, depth_footprint,
          [&](std::byte* destination, std::uint32_t packed) {
            const auto value = depth_stencil_to_host(guest_format_, packed);
            std::memcpy(destination, &value.depth, 4);
          }) ||
      !make_upload(stencil_source.Get(), 1, stencil_upload, stencil_footprint,
          [&](std::byte* destination, std::uint32_t packed) {
            *destination = std::byte(depth_stencil_to_host(
                guest_format_, packed).stencil);
          })) {
    error_ = "D3D12 depth upload staging allocation failed";
    return false;
  }
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_heap;
  if (!create_srv_heap(device.Get(), depth_source.Get(), stencil_source.Get(),
                       DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8_UINT, false,
                       srv_heap)) {
    error_ = "D3D12 depth upload SRVs could not be created";
    return false;
  }
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
  if (!create_depth_transfer_pipeline(device.Get(), DepthTransferMode::DepthWrite,
                                      host_msaa_, format_, root_signature,
                                      pipeline, error_))
    return false;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> stencil_root_signature;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> stencil_pipeline;
  if (!create_depth_transfer_pipeline(
          device.Get(), DepthTransferMode::StencilWrite, host_msaa_, format_,
          stencil_root_signature, stencil_pipeline, error_))
    return false;
  struct Constants { std::uint32_t origin[2], sample, pad; };
  Buffer constants;
  constexpr std::uint64_t kConstantStride = 256;
  if (!constants.initialize(device.Get(), 257 * kConstantStride,
                            D3D12_HEAP_TYPE_UPLOAD,
                            D3D12_RESOURCE_STATE_GENERIC_READ)) {
    error_ = constants.error(); return false;
  }
  std::span<std::byte> mapped;
  if (!constants.map(mapped)) { error_ = constants.error(); return false; }
  const Constants values{{left, top}, mapping->sample, 0};
  std::memcpy(mapped.data(), &values, sizeof(values));
  for (std::uint32_t value = 0; value < 256; ++value) {
    const Constants stencil_values{{left, top}, mapping->sample, value};
    std::memcpy(mapped.data() + (value + 1u) * kConstantStride,
                &stencil_values, sizeof(stencil_values));
  }
  constants.unmap();
  if (!queue.execute([&](ID3D12GraphicsCommandList* list) {
        D3D12_TEXTURE_COPY_LOCATION depth_src{};
        depth_src.pResource = depth_upload.resource();
        depth_src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        depth_src.PlacedFootprint = depth_footprint;
        D3D12_TEXTURE_COPY_LOCATION depth_dst{};
        depth_dst.pResource = depth_source.Get();
        depth_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&depth_dst, 0, 0, 0, &depth_src, nullptr);
        D3D12_TEXTURE_COPY_LOCATION stencil_src{};
        stencil_src.pResource = stencil_upload.resource();
        stencil_src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        stencil_src.PlacedFootprint = stencil_footprint;
        D3D12_TEXTURE_COPY_LOCATION stencil_dst{};
        stencil_dst.pResource = stencil_source.Get();
        stencil_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&stencil_dst, 0, 0, 0, &stencil_src, nullptr);
        std::array<D3D12_RESOURCE_BARRIER, 2> barriers{};
        const std::array<ID3D12Resource*, 2> textures{
            depth_source.Get(), stencil_source.Get()};
        for (std::size_t i = 0; i < 2; ++i) {
          barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          barriers[i].Transition.pResource = textures[i];
          barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
          barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
        list->ResourceBarrier(2, barriers.data());
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
        list->OMSetRenderTargets(0, nullptr, FALSE, &dsv_);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        list->SetPipelineState(stencil_pipeline.Get());
        list->SetGraphicsRootSignature(stencil_root_signature.Get());
        list->SetGraphicsRootDescriptorTable(
            1, srv_heap->GetGPUDescriptorHandleForHeapStart());
        for (std::uint32_t value = 0; value < 256; ++value) {
          list->SetGraphicsRootConstantBufferView(
              0, constants.resource()->GetGPUVirtualAddress() +
                     (value + 1u) * kConstantStride);
          list->OMSetStencilRef(value);
          list->DrawInstanced(3, 1, 0, 0);
        }
      })) {
    error_ = queue.error(); return false;
  }
  return true;
#endif
}

void DepthTargetImage::reset() noexcept {
  resource_.Reset();
  dsv_heap_.Reset();
  dsv_ = {};
  format_ = DXGI_FORMAT_UNKNOWN;
  guest_format_ = DepthRenderTargetFormat::D24S8;
  host_msaa_ = MsaaSamples::X1;
  float24_ = false;
  width_ = height_ = 0;
  surface_ = {};
}

}  // namespace xenon::gpu::d3d12

#include "xenon/gpu/d3d12/pipeline.hpp"

#include <cmath>

namespace xenon::gpu::d3d12 {
namespace {

#ifdef _MSC_VER
#pragma warning(push)
// Pipeline-state stream subobjects intentionally add pointer-alignment padding.
#pragma warning(disable : 4324)
#endif

template <typename T, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type>
struct alignas(void*) PipelineSubobject {
  D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{Type};
  T value{};
};

struct GraphicsPipelineStream {
  PipelineSubobject<ID3D12RootSignature*,
                    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE> root;
  PipelineSubobject<D3D12_SHADER_BYTECODE,
                    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS> vs;
  PipelineSubobject<D3D12_SHADER_BYTECODE,
                    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS> gs;
  PipelineSubobject<D3D12_SHADER_BYTECODE,
                    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS> ps;
  PipelineSubobject<D3D12_BLEND_DESC,
                    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND> blend;
  PipelineSubobject<UINT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK>
      sample_mask;
  PipelineSubobject<D3D12_RASTERIZER_DESC,
                    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER> rasterizer;
  PipelineSubobject<D3D12_DEPTH_STENCIL_DESC2,
                    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2>
      depth_stencil;
  PipelineSubobject<D3D12_INPUT_LAYOUT_DESC,
                    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT>
      input_layout;
  PipelineSubobject<D3D12_PRIMITIVE_TOPOLOGY_TYPE,
                    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY>
      topology;
  PipelineSubobject<D3D12_RT_FORMAT_ARRAY,
                    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS>
      render_targets;
  PipelineSubobject<DXGI_FORMAT,
                    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT>
      depth_format;
  PipelineSubobject<DXGI_SAMPLE_DESC,
                    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC> sample_desc;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type(HostPrimitiveTopology topology) {
  switch (topology) {
    case HostPrimitiveTopology::PointList: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    case HostPrimitiveTopology::LineList: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    case HostPrimitiveTopology::TriangleList: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  }
  return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
}

D3D12_PRIMITIVE_TOPOLOGY command_topology(HostPrimitiveTopology topology) {
  switch (topology) {
    case HostPrimitiveTopology::PointList: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    case HostPrimitiveTopology::LineList: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case HostPrimitiveTopology::TriangleList: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  }
  return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

UINT sample_count(MsaaSamples samples) {
  switch (samples) {
    case MsaaSamples::X1: return 1;
    case MsaaSamples::X2: return 2;
    case MsaaSamples::X4: return 4;
  }
  return 1;
}

D3D12_BLEND native_blend_factor(BlendFactor factor) {
  switch (factor) {
    case BlendFactor::Zero: return D3D12_BLEND_ZERO;
    case BlendFactor::One: return D3D12_BLEND_ONE;
    case BlendFactor::SrcColor: return D3D12_BLEND_SRC_COLOR;
    case BlendFactor::InvSrcColor: return D3D12_BLEND_INV_SRC_COLOR;
    case BlendFactor::SrcAlpha: return D3D12_BLEND_SRC_ALPHA;
    case BlendFactor::InvSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
    case BlendFactor::DestColor: return D3D12_BLEND_DEST_COLOR;
    case BlendFactor::InvDestColor: return D3D12_BLEND_INV_DEST_COLOR;
    case BlendFactor::DestAlpha: return D3D12_BLEND_DEST_ALPHA;
    case BlendFactor::InvDestAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
    case BlendFactor::ConstantColor:
    case BlendFactor::ConstantAlpha: return D3D12_BLEND_BLEND_FACTOR;
    case BlendFactor::InvConstantColor:
    case BlendFactor::InvConstantAlpha: return D3D12_BLEND_INV_BLEND_FACTOR;
    case BlendFactor::SrcAlphaSaturate: return D3D12_BLEND_SRC_ALPHA_SAT;
  }
  return D3D12_BLEND_ONE;
}

D3D12_BLEND native_alpha_blend_factor(BlendFactor factor) {
  switch (factor) {
    case BlendFactor::SrcColor: return D3D12_BLEND_SRC_ALPHA;
    case BlendFactor::InvSrcColor: return D3D12_BLEND_INV_SRC_ALPHA;
    case BlendFactor::DestColor: return D3D12_BLEND_DEST_ALPHA;
    case BlendFactor::InvDestColor: return D3D12_BLEND_INV_DEST_ALPHA;
    default: return native_blend_factor(factor);
  }
}

D3D12_BLEND_OP native_blend_operation(BlendOperation operation) {
  switch (operation) {
    case BlendOperation::Add: return D3D12_BLEND_OP_ADD;
    case BlendOperation::Subtract: return D3D12_BLEND_OP_SUBTRACT;
    case BlendOperation::Minimum: return D3D12_BLEND_OP_MIN;
    case BlendOperation::Maximum: return D3D12_BLEND_OP_MAX;
    case BlendOperation::ReverseSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
  }
  return D3D12_BLEND_OP_ADD;
}

D3D12_COMPARISON_FUNC native_compare(CompareFunction function) {
  return static_cast<D3D12_COMPARISON_FUNC>(D3D12_COMPARISON_FUNC_NEVER +
                                            static_cast<unsigned>(function));
}

D3D12_STENCIL_OP native_stencil(StencilOperation operation) {
  return static_cast<D3D12_STENCIL_OP>(D3D12_STENCIL_OP_KEEP +
                                       static_cast<unsigned>(operation));
}

D3D12_DEPTH_STENCILOP_DESC native_stencil_face(const StencilFaceState& face) {
  return {native_stencil(face.fail), native_stencil(face.depth_fail),
          native_stencil(face.depth_pass), native_compare(face.function)};
}

D3D12_DEPTH_STENCILOP_DESC1 native_stencil_face(
    const StencilFaceState& face, std::uint8_t read_mask,
    std::uint8_t write_mask) {
  return {native_stencil(face.fail), native_stencil(face.depth_fail),
          native_stencil(face.depth_pass), native_compare(face.function),
          read_mask, write_mask};
}

}  // namespace

bool GraphicsPipeline::initialize(ID3D12Device* device,
                                  ID3D12RootSignature* root_signature,
                                  const CompiledShader& vertex_shader,
                                  const CompiledShader& pixel_shader,
                                  const CompiledShader* geometry_shader,
                                  std::span<const DXGI_FORMAT> color_formats,
                                  MsaaSamples samples,
                                  HostPrimitiveTopology topology,
                                  const RasterState& raster,
                                  std::span<const std::uint8_t> color_write_masks,
                                  std::span<const BlendState> blend_states,
                                  DXGI_FORMAT depth_format,
                                  const DepthTargetDescriptor* depth_state) {
  reset();
  error_.clear();
  if (!device || !root_signature || color_formats.size() > 4 ||
      (color_formats.empty() && depth_format == DXGI_FORMAT_UNKNOWN) ||
      color_write_masks.size() != color_formats.size() ||
      blend_states.size() != color_formats.size() ||
      !vertex_shader.succeeded || !pixel_shader.succeeded ||
      vertex_shader.format != ShaderBinaryFormat::Dxil ||
      pixel_shader.format != ShaderBinaryFormat::Dxil ||
      vertex_shader.binary.empty() || pixel_shader.binary.empty() ||
      (geometry_shader &&
       (!geometry_shader->succeeded ||
        geometry_shader->format != ShaderBinaryFormat::Dxil ||
        geometry_shader->binary.empty()))) {
    error_ = "D3D12 graphics pipeline requires valid DXIL shaders and render state";
    return false;
  }
  for (const auto format : color_formats) {
    if (format == DXGI_FORMAT_UNKNOWN) {
      error_ = "D3D12 graphics pipeline received an unsupported color format";
      return false;
    }
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root_signature;
  desc.VS = {vertex_shader.binary.data(), vertex_shader.binary.size()};
  desc.PS = {pixel_shader.binary.data(), pixel_shader.binary.size()};
  if (geometry_shader)
    desc.GS = {geometry_shader->binary.data(), geometry_shader->binary.size()};
  desc.BlendState.IndependentBlendEnable = TRUE;
  for (std::size_t i = 0; i < color_formats.size(); ++i) {
    auto& target = desc.BlendState.RenderTarget[i];
    target.BlendEnable = blend_states[i].enabled;
    target.SrcBlend = native_blend_factor(blend_states[i].color_source);
    target.DestBlend = native_blend_factor(blend_states[i].color_destination);
    target.BlendOp = native_blend_operation(blend_states[i].color_operation);
    target.SrcBlendAlpha = native_alpha_blend_factor(blend_states[i].alpha_source);
    target.DestBlendAlpha = native_alpha_blend_factor(blend_states[i].alpha_destination);
    target.BlendOpAlpha = native_blend_operation(blend_states[i].alpha_operation);
    target.LogicOp = D3D12_LOGIC_OP_NOOP;
    target.RenderTargetWriteMask = color_write_masks[i] & 0xFu;
    desc.RTVFormats[i] = color_formats[i];
  }
  desc.SampleMask = UINT_MAX;
  // D3D12 has no point polygon fill mode. As in other native Xenos renderers,
  // point and line dual-polygon requests use wireframe here; triangle fill is
  // represented exactly. Front+back culling is filtered before production
  // submission because D3D12 also has no CULL_FRONT_AND_BACK state.
  desc.RasterizerState.FillMode =
      host_polygon_mode(raster) == HostPolygonMode::Fill
          ? D3D12_FILL_MODE_SOLID
          : D3D12_FILL_MODE_WIREFRAME;
  desc.RasterizerState.CullMode = raster.cull_front && raster.cull_back
                                      ? D3D12_CULL_MODE_NONE
                                      : raster.cull_front ? D3D12_CULL_MODE_FRONT
                                      : raster.cull_back ? D3D12_CULL_MODE_BACK
                                                         : D3D12_CULL_MODE_NONE;
  desc.RasterizerState.FrontCounterClockwise = raster.front_face_clockwise ? FALSE : TRUE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  const auto polygon_offset = preferred_polygon_offset(raster, topology);
  if (polygon_offset.enabled) {
    const auto depth_target_format =
        depth_state && depth_state->format ==
                           static_cast<std::uint8_t>(DepthRenderTargetFormat::D24FS8)
            ? DepthRenderTargetFormat::D24FS8
            : DepthRenderTargetFormat::D24S8;
    desc.RasterizerState.DepthBias = integer_polygon_offset(
        polygon_offset.offset, depth_target_format);
    desc.RasterizerState.SlopeScaledDepthBias = polygon_offset.scale / 16.0f;
  }
  if (depth_state) {
    desc.DepthStencilState.DepthEnable = depth_state->test_enabled;
    desc.DepthStencilState.DepthWriteMask = depth_state->write_enabled
                                                ? D3D12_DEPTH_WRITE_MASK_ALL
                                                : D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = native_compare(depth_state->function);
    desc.DepthStencilState.StencilEnable = depth_state->stencil_enabled;
    desc.DepthStencilState.StencilReadMask = depth_state->stencil_read_mask;
    desc.DepthStencilState.StencilWriteMask = depth_state->stencil_write_mask;
    desc.DepthStencilState.FrontFace = native_stencil_face(depth_state->stencil_front);
    desc.DepthStencilState.BackFace = depth_state->backface_stencil_enabled
                                          ? native_stencil_face(depth_state->stencil_back)
                                          : desc.DepthStencilState.FrontFace;
  }
  desc.InputLayout = {nullptr, 0};
  desc.PrimitiveTopologyType = topology_type(topology);
  desc.NumRenderTargets = static_cast<UINT>(color_formats.size());
  desc.DSVFormat = depth_format;
  desc.SampleDesc.Count = sample_count(samples);
  Microsoft::WRL::ComPtr<ID3D12Device2> device2;
  if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device2)))) {
    GraphicsPipelineStream stream{};
    stream.root.value = root_signature;
    stream.vs.value = desc.VS;
    stream.gs.value = desc.GS;
    stream.ps.value = desc.PS;
    stream.blend.value = desc.BlendState;
    stream.sample_mask.value = desc.SampleMask;
    stream.rasterizer.value = desc.RasterizerState;
    auto& depth = stream.depth_stencil.value;
    depth.DepthEnable = desc.DepthStencilState.DepthEnable;
    depth.DepthWriteMask = desc.DepthStencilState.DepthWriteMask;
    depth.DepthFunc = desc.DepthStencilState.DepthFunc;
    depth.StencilEnable = desc.DepthStencilState.StencilEnable;
    if (depth_state) {
      depth.FrontFace = native_stencil_face(
          depth_state->stencil_front, depth_state->stencil_read_mask,
          depth_state->stencil_write_mask);
      depth.BackFace = depth_state->backface_stencil_enabled
                           ? native_stencil_face(
                                 depth_state->stencil_back,
                                 depth_state->stencil_back_read_mask,
                                 depth_state->stencil_back_write_mask)
                           : depth.FrontFace;
    }
    stream.input_layout.value = desc.InputLayout;
    stream.topology.value = desc.PrimitiveTopologyType;
    stream.render_targets.value.NumRenderTargets = desc.NumRenderTargets;
    for (UINT i = 0; i < desc.NumRenderTargets; ++i)
      stream.render_targets.value.RTFormats[i] = desc.RTVFormats[i];
    stream.depth_format.value = desc.DSVFormat;
    stream.sample_desc.value = desc.SampleDesc;
    const D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{
        sizeof(stream), &stream};
    if (FAILED(device2->CreatePipelineState(&stream_desc,
                                            IID_PPV_ARGS(&pipeline_)))) {
      error_ = "CreatePipelineState failed for translated Xenos shaders";
      return false;
    }
  } else if (depth_state && depth_state->stencil_enabled &&
             depth_state->backface_stencil_enabled &&
             (depth_state->stencil_read_mask !=
                  depth_state->stencil_back_read_mask ||
              depth_state->stencil_write_mask !=
                  depth_state->stencil_back_write_mask)) {
    error_ = "D3D12 device lacks independent front/back stencil-mask support";
    return false;
  } else if (FAILED(device->CreateGraphicsPipelineState(
                 &desc, IID_PPV_ARGS(&pipeline_)))) {
    error_ = "CreateGraphicsPipelineState failed for translated Xenos shaders";
    return false;
  }
  topology_ = command_topology(topology);
  return true;
}

void GraphicsPipeline::reset() noexcept {
  pipeline_.Reset();
  topology_ = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

}  // namespace xenon::gpu::d3d12

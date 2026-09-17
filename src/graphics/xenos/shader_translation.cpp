#include "xenon/gpu/shader_translation.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>
#include <unordered_map>

namespace xenon::gpu {
namespace {

std::uint64_t hash_text(std::string_view text) noexcept {
  std::uint64_t hash = 14695981039346656037ull;
  for (unsigned char c : text) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string swizzle(std::uint8_t value) {
  static constexpr char component[] = "xyzw";
  std::string result{"."};
  for (unsigned i = 0; i < 4; ++i) result += component[(value >> (i * 2u)) & 3u];
  return result;
}

std::string source(const AluSource& value, ShaderStage stage) {
  std::ostringstream out;
  if (value.negate) out << "-";
  if (value.absolute_value) out << "abs(";
  if (value.temporary) {
    out << "r[(" << value.index;
    if (value.relative) out << " + ar";
    out << ") & 63]";
  } else {
    out << "XenonFloatConstants[((" << value.index;
    if (value.relative) out << " + ar";
    out << ") & 255)";
    if (stage == ShaderStage::Pixel) out << " + 256";
    out << "]";
  }
  out << swizzle(value.swizzle);
  if (value.absolute_value) out << ")";
  return out.str();
}

std::string predicate(const Predicate& value) {
  if (!value.enabled) return "true";
  return value.condition ? "p0" : "!p0";
}

void write_masked(std::ostringstream& out, std::string_view destination,
                  std::string_view value, std::uint8_t mask, unsigned indent) {
  static constexpr char component[] = "xyzw";
  const std::string spaces(indent, ' ');
  for (unsigned i = 0; i < 4; ++i) {
    if (mask & (1u << i)) {
      out << spaces << destination << "." << component[i] << " = " << value
          << "." << component[i] << ";\n";
    }
  }
}

std::string destination(const AluInstruction& alu, bool scalar) {
  const auto index = scalar ? alu.scalar_destination : alu.vector_destination;
  const bool relative = scalar ? alu.scalar_destination_relative
                               : alu.vector_destination_relative;
  std::ostringstream out;
  if (alu.export_data) {
    out << "e[" << unsigned(index) << "]";
  } else {
    out << "r[(" << unsigned(index);
    if (relative) out << " + ar";
    out << ") & 63]";
  }
  return out.str();
}

void emit_alu(std::ostringstream& out, const AluInstruction& alu,
              ShaderStage stage) {
  out << "        if (" << predicate(alu.predicate) << ") {\n"
      << "          float4 s0 = " << source(alu.sources[0], stage) << ";\n"
      << "          float4 s1 = " << source(alu.sources[1], stage) << ";\n"
      << "          float4 s2 = " << source(alu.sources[2], stage) << ";\n"
      << "          bool killed = false;\n"
      << "          float4 pv = xenon_vector_op(" << unsigned(alu.vector_opcode)
      << ", s0, s1, s2, p0, a0, killed);\n"
      << "          float4 ps_new = xenon_scalar_op(" << unsigned(alu.scalar_opcode)
      << ", s0, s1, ps, p0, a0, killed);\n";
  if (alu.vector_clamp) out << "          pv = saturate(pv);\n";
  if (alu.scalar_clamp) out << "          ps_new = saturate(ps_new);\n";
  write_masked(out, destination(alu, false), "pv", alu.vector_write_mask, 10);
  write_masked(out, destination(alu, true), "ps_new", alu.scalar_write_mask, 10);
  out << "          ps = ps_new;\n";
  if (stage == ShaderStage::Pixel) out << "          if (killed) discard;\n";
  out << "        }\n";
}

void emit_fetch(std::ostringstream& out, const DecodedInstruction& instruction) {
  if (instruction.kind == ShaderInstructionKind::VertexFetch) {
    const auto& fetch = instruction.vertex_fetch;
    out << "        if (" << predicate(fetch.predicate) << ") {\n"
        << "          uint source_index = asuint(r[(" << unsigned(fetch.source_register);
    if (fetch.source_relative) out << " + ar";
    out << ") & 63][" << unsigned(fetch.source_swizzle & 3u) << "]);\n"
        << "          float4 fetched = xenon_vertex_fetch(" << unsigned(fetch.fetch_constant)
        << ", source_index, " << unsigned(fetch.stride_dwords) << ", "
        << fetch.offset_dwords << ", " << unsigned(fetch.data_format) << ", "
        << (fetch.signed_data ? "true" : "false") << ", "
        << (fetch.normalized ? "true" : "false") << ");\n"
        << "          xenon_write_fetch(r[(" << unsigned(fetch.destination_register);
    if (fetch.destination_relative) out << " + ar";
    out << ") & 63], fetched, " << fetch.destination_swizzle << ");\n"
        << "        }\n";
  } else {
    const auto& fetch = instruction.texture_fetch;
    out << "        if (" << predicate(fetch.predicate) << ") {\n"
        << "          float4 coord = r[(" << unsigned(fetch.source_register);
    if (fetch.source_relative) out << " + ar";
    out << ") & 63]" << swizzle(fetch.source_swizzle) << ";\n"
        << "          float4 grad_h = r[(" << unsigned(fetch.source_register + 1u)
        << ") & 63];\n"
        << "          float4 grad_v = r[(" << unsigned(fetch.source_register + 2u)
        << ") & 63];\n"
        << "          float4 fetched = xenon_texture_fetch(" << unsigned(fetch.fetch_constant)
        << ", " << unsigned(fetch.opcode) << ", " << unsigned(fetch.dimension)
        << ", coord, " << (float(fetch.lod_bias_sixteenths) / 16.0f)
        << ", " << (fetch.use_computed_lod ? "true" : "false")
        << ", " << (fetch.use_register_lod ? "true" : "false")
        << ", " << (fetch.use_register_gradients ? "true" : "false")
        << ", grad_h, grad_v, "
        << (fetch.unnormalized_coordinates ? "true" : "false") << ", float3("
        << (float(fetch.offsets_half_texels[0]) / 2.0f) << ","
        << (float(fetch.offsets_half_texels[1]) / 2.0f) << ","
        << (float(fetch.offsets_half_texels[2]) / 2.0f) << "));\n"
        << "          xenon_write_fetch(r[(" << unsigned(fetch.destination_register);
    if (fetch.destination_relative) out << " + ar";
    out << ") & 63], fetched, " << fetch.destination_swizzle << ");\n"
        << "        }\n";
  }
}

void emit_prelude(std::ostringstream& out) {
  out << R"hlsl(
cbuffer XenonShaderConstants : register(b0) {
  // Xenos has independent 256-vector banks for VS and PS. Pixel shader
  // lowering adds 256 to its architectural constant index.
  float4 XenonFloatConstants[512];
  uint4 XenonBoolConstants[8];
  uint4 XenonLoopConstants[32];
  uint4 XenonVertexFetchConstants[32];
};
ByteAddressBuffer XenonGuestMemory : register(t0);
Texture1D<float4> XenonTextures1D[32] : register(t1);
Texture2D<float4> XenonTextures2D[32] : register(t33);
Texture3D<float4> XenonTextures3D[32] : register(t65);
TextureCube<float4> XenonTexturesCube[32] : register(t97);
SamplerState XenonSamplers[32] : register(s0);

bool xenon_bool(uint index) {
  uint word = XenonBoolConstants[(index >> 7) & 7][(index >> 5) & 3];
  return ((word >> (index & 31)) & 1) != 0;
}

float xenon_legacy_mul(float a, float b) {
  return (a == 0.0 || b == 0.0) ? 0.0 : a * b;
}
float4 xenon_legacy_mul4(float4 a, float4 b) {
  return float4(xenon_legacy_mul(a.x,b.x), xenon_legacy_mul(a.y,b.y),
                xenon_legacy_mul(a.z,b.z), xenon_legacy_mul(a.w,b.w));
}

float4 xenon_vector_op(uint op, float4 a, float4 b, float4 c,
                       inout bool p0, inout int a0, out bool killed) {
  killed = false;
  if (op == 0) return a + b;
  if (op == 1) return xenon_legacy_mul4(a, b);
  if (op == 2) return max(a, b);
  if (op == 3) return min(a, b);
  if (op == 4) return float4(a == b);
  if (op == 5) return float4(a > b);
  if (op == 6) return float4(a >= b);
  if (op == 7) return float4(a != b);
  if (op == 8) return frac(a);
  if (op == 9) return trunc(a);
  if (op == 10) return floor(a);
  if (op == 11) return xenon_legacy_mul4(a, b) + c;
  if (op == 12) return select(a == 0.0, b, c);
  if (op == 13) return select(a >= 0.0, b, c);
  if (op == 14) return select(a > 0.0, b, c);
  if (op == 15) return dot(a, b).xxxx;
  if (op == 16) return dot(a.xyz, b.xyz).xxxx;
  if (op == 17) return (dot(a.xy, b.xy) + c.x).xxxx;
  if (op == 18) {
    float3 q = abs(a.xyz); float m = max(q.x, max(q.y, q.z));
    return float4(m, 1.0, 1.0, 1.0);
  }
  if (op == 19) return max(max(a.x, a.y), max(a.z, a.w)).xxxx;
  if (op >= 20 && op <= 23) {
    p0 = op == 20 ? (a.w == 0.0 && b.w == 0.0) :
         op == 21 ? (a.w == 0.0 && b.w != 0.0) :
         op == 22 ? (a.w == 0.0 && b.w > 0.0) : (a.w == 0.0 && b.w >= 0.0);
    return ((op == 20 ? b.x == 0.0 : op == 21 ? b.x != 0.0 :
             op == 22 ? b.x > 0.0 : b.x >= 0.0) && a.x == 0.0) ? 0.0.xxxx : (a.x + 1.0).xxxx;
  }
  if (op >= 24 && op <= 27) {
    killed = op == 24 ? any(a == b) : op == 25 ? any(a > b) :
             op == 26 ? any(a >= b) : any(a != b);
    return (killed ? 1.0 : 0.0).xxxx;
  }
  if (op == 28) return float4(1.0, a.y * b.y, a.z, b.w);
  if (op == 29) { a0 = (int)floor(a.w + 0.5); return max(a, b); }
  return 0.0.xxxx;
}

float4 xenon_scalar_op(uint op, float4 a, float4 b, float4 previous,
                       inout bool p0, inout int a0, out bool killed) {
  killed = false; float x = a.w; float y = a.x; float v = 0.0;
  if (op == 0) v=x+y; else if (op == 1) v=x+previous.x;
  else if (op == 2) v=xenon_legacy_mul(x,y); else if (op==3 || op==4) v=xenon_legacy_mul(x,previous.x);
  else if (op==5) v=max(x,y); else if (op==6) v=min(x,y);
  else if (op==7) v=x==0.0; else if (op==8) v=x>0.0; else if (op==9) v=x>=0.0; else if (op==10) v=x!=0.0;
  else if (op==11) v=frac(x); else if (op==12) v=trunc(x); else if (op==13) v=floor(x);
  else if (op==14) v=exp2(x); else if (op==15 || op==16) v=log2(x);
  else if (op>=17 && op<=19) v=rcp(x); else if (op>=20 && op<=22) v=rsqrt(abs(x));
  else if (op==23 || op==24) { v=max(x,y); a0=(int)floor(v + (op==24 ? 0.0 : 0.5)); }
  else if (op==25) v=x-y; else if (op==26) v=x-previous.x;
  else if (op>=27 && op<=30) { p0=op==27?x==0.0:op==28?x!=0.0:op==29?x>0.0:x>=0.0; v=p0; }
  else if (op==31) { p0=!p0; v=p0; } else if (op==32) { p0=false; v=0.0; }
  else if (op==33) { p0=false; v=0.0; } else if (op==34) v=p0;
  else if (op>=35 && op<=39) { killed=op==35?x==0.0:op==36?x>0.0:op==37?x>=0.0:op==38?x!=0.0:x==1.0; v=killed; }
  else if (op==40) v=sqrt(abs(x)); else if (op==42 || op==43) v=xenon_legacy_mul(x, b.x);
  else if (op==44 || op==45) v=x+b.x; else if (op==46 || op==47) v=x-b.x;
  else if (op==48) v=sin(x); else if (op==49) v=cos(x); else if (op==50) return previous;
  return v.xxxx;
}

float4 xenon_vertex_fetch(uint fetch_constant, uint index, uint stride, int offset,
                          uint format, bool is_signed, bool normalized) {
  uint base = XenonVertexFetchConstants[fetch_constant & 31].x;
  uint address = base + 4 * (index * stride + offset);
  uint4 raw = XenonGuestMemory.Load4(address);
  if (format == 36) return asfloat(raw.x).xxxx;
  if (format == 37) return float4(asfloat(raw.xy), 0.0, 1.0);
  if (format == 57) return float4(asfloat(raw.xyz), 1.0);
  if (format == 38) return asfloat(raw);
  if (format == 33) return (is_signed ? (float)(int)raw.x : (float)raw.x).xxxx;
  if (format == 34) return float4(is_signed ? float2((int2)raw.xy) : float2(raw.xy), 0.0, 1.0);
  if (format == 35) return is_signed ? float4((int4)raw) : float4(raw);
  if (format == 31) return float4(f16tof32(raw.x & 65535), f16tof32(raw.x >> 16), 0.0, 1.0);
  if (format == 32) return float4(f16tof32(raw.x & 65535), f16tof32(raw.x >> 16),
                                  f16tof32(raw.y & 65535), f16tof32(raw.y >> 16));
  uint4 u = 0; int4 s = 0; uint4 maximum = 1;
  if (format == 6) {
    u=uint4(raw.x&255,(raw.x>>8)&255,(raw.x>>16)&255,(raw.x>>24)&255); maximum=255;
    s=(int4)(u<<24)>>24;
  } else if (format == 7) {
    u=uint4(raw.x&1023,(raw.x>>10)&1023,(raw.x>>20)&1023,raw.x>>30); maximum=uint4(1023,1023,1023,3);
    s=int4((int)(u.x<<22)>>22,(int)(u.y<<22)>>22,(int)(u.z<<22)>>22,(int)(u.w<<30)>>30);
  } else if (format == 25 || format == 26) {
    u=uint4(raw.x&65535,raw.x>>16,raw.y&65535,raw.y>>16); maximum=65535;
    s=(int4)(u<<16)>>16;
    if (format == 25) { u.zw=uint2(0,1); s.zw=int2(0,1); maximum.zw=1; }
  }
  if (normalized) return is_signed ? max(float4(s) / float4(max(maximum >> 1, 1)), -1.0) : float4(u) / float4(maximum);
  return is_signed ? float4(s) : float4(u);
}
float4 xenon_texture_fetch(uint fetch_constant, uint opcode, uint dimension,
                           float4 coord, float lod_bias, bool computed_lod,
                           bool register_lod, bool register_gradients,
                           float4 grad_h, float4 grad_v, bool unnormalized,
                           float3 offset) {
  uint slot=fetch_constant & 31;
  float gradient_scale=exp2(lod_bias);
  if (dimension==0) {
    uint width, levels; XenonTextures1D[slot].GetDimensions(0,width,levels);
    float c=coord.x+offset.x; float gh=grad_h.x; float gv=grad_v.x;
    if (unnormalized) { c/=max(width,1); gh/=max(width,1); gv/=max(width,1); }
    if (register_gradients) return XenonTextures1D[slot].SampleGrad(XenonSamplers[slot],c,gh*gradient_scale,gv*gradient_scale);
    if (register_lod || !computed_lod) return XenonTextures1D[slot].SampleLevel(XenonSamplers[slot],c,coord.w+lod_bias);
    return XenonTextures1D[slot].SampleBias(XenonSamplers[slot],c,lod_bias);
  }
  if (dimension==1) {
    uint width,height,levels; XenonTextures2D[slot].GetDimensions(0,width,height,levels);
    float2 c=coord.xy+offset.xy; float2 gh=grad_h.xy; float2 gv=grad_v.xy;
    if (unnormalized) { float2 size=max(float2(width,height),1.0); c/=size; gh/=size; gv/=size; }
    if (register_gradients) return XenonTextures2D[slot].SampleGrad(XenonSamplers[slot],c,gh*gradient_scale,gv*gradient_scale);
    if (register_lod || !computed_lod) return XenonTextures2D[slot].SampleLevel(XenonSamplers[slot],c,coord.w+lod_bias);
    return XenonTextures2D[slot].SampleBias(XenonSamplers[slot],c,lod_bias);
  }
  if (dimension==2) {
    uint width,height,depth,levels; XenonTextures3D[slot].GetDimensions(0,width,height,depth,levels);
    float3 c=coord.xyz+offset; float3 gh=grad_h.xyz; float3 gv=grad_v.xyz;
    if (unnormalized) { float3 size=max(float3(width,height,depth),1.0); c/=size; gh/=size; gv/=size; }
    if (register_gradients) return XenonTextures3D[slot].SampleGrad(XenonSamplers[slot],c,gh*gradient_scale,gv*gradient_scale);
    if (register_lod || !computed_lod) return XenonTextures3D[slot].SampleLevel(XenonSamplers[slot],c,coord.w+lod_bias);
    return XenonTextures3D[slot].SampleBias(XenonSamplers[slot],c,lod_bias);
  }
  float3 cube_coord=coord.xyz;
  if (register_gradients) return XenonTexturesCube[slot].SampleGrad(XenonSamplers[slot],cube_coord,grad_h.xyz*gradient_scale,grad_v.xyz*gradient_scale);
  if (register_lod || !computed_lod) return XenonTexturesCube[slot].SampleLevel(XenonSamplers[slot],cube_coord,coord.w+lod_bias);
  return XenonTexturesCube[slot].SampleBias(XenonSamplers[slot],cube_coord,lod_bias);
}
void xenon_write_fetch(inout float4 dest, float4 value, uint swizzle) {
  [unroll] for (uint i=0;i<4;++i) {
    uint s=(swizzle>>(i*3))&7;
    if (s<4) dest[i]=value[s]; else if (s==4) dest[i]=0.0; else if (s==5) dest[i]=1.0;
  }
}
)hlsl";
}

}  // namespace

LoweredShader HlslShaderLowerer::lower(const DecodedShader& shader,
                                       const ShaderLoweringOptions& options) {
  LoweredShader result{};
  result.stage = shader.stage;
  result.source_hash = shader.source_hash;
  result.entry_point = options.entry_point;
  result.profile = shader.stage == ShaderStage::Vertex ? "vs_6_0" : "ps_6_0";
  result.reflection = shader.reflection;
  if (!shader.complete) {
    result.diagnostics.emplace_back("cannot lower an incomplete decoded shader");
    result.diagnostics.insert(result.diagnostics.end(), shader.diagnostics.begin(),
                              shader.diagnostics.end());
    return result;
  }
  if (shader.reflection.temporary_register_count > options.temporary_register_limit ||
      options.temporary_register_limit > 64) {
    result.diagnostics.emplace_back("shader temporary register usage exceeds the Xenos limit");
    return result;
  }

  static constexpr std::array<std::uint8_t, 11> supported_vertex_formats{
      6, 7, 25, 26, 31, 32, 33, 34, 35, 36, 37};
  for (const auto& instruction : shader.instructions) {
    if (instruction.kind == ShaderInstructionKind::Alu) {
      const auto& alu = instruction.alu;
      if (alu.vector_opcode > 29) {
        result.diagnostics.emplace_back("reserved Xenos vector ALU opcode");
      }
      if (alu.scalar_opcode == 41 || alu.scalar_opcode > 50) {
        result.diagnostics.emplace_back("reserved Xenos scalar ALU opcode");
      }
    } else if (instruction.kind == ShaderInstructionKind::VertexFetch) {
      const auto format = instruction.vertex_fetch.data_format;
      const bool supported = std::find(supported_vertex_formats.begin(),
                                       supported_vertex_formats.end(), format) !=
                             supported_vertex_formats.end() || format == 38 ||
                             format == 57;
      if (!supported) {
        result.diagnostics.emplace_back(
            "vertex format requires a lowering implementation");
      }
    } else {
      const auto& fetch = instruction.texture_fetch;
      if (fetch.opcode != 1) {
        result.diagnostics.emplace_back(
            "texture state/query opcode requires a lowering implementation");
      }
    }
  }
  if (shader.reflection.memory_exports) {
    result.diagnostics.emplace_back("Xenos memory-export lowering is not implemented");
  }
  if (!result.diagnostics.empty()) return result;

  std::unordered_map<std::uint32_t, const DecodedInstruction*> instructions;
  for (const auto& instruction : shader.instructions) {
    const auto address = instruction.kind == ShaderInstructionKind::Alu
                             ? instruction.alu.address
                             : instruction.kind == ShaderInstructionKind::VertexFetch
                                   ? instruction.vertex_fetch.address
                                   : instruction.texture_fetch.address;
    instructions.emplace(address, &instruction);
  }

  std::ostringstream out;
  out << "// Project Xenon generated HLSL; source hash 0x" << std::hex
      << shader.source_hash << std::dec << "\n";
  emit_prelude(out);
  if (shader.stage == ShaderStage::Vertex) {
    out << "struct XenonOutput { float4 position : SV_Position;";
    for (unsigned i=0;i<16;++i) out << " float4 i" << i << " : TEXCOORD" << i << ";";
    out << " };\nXenonOutput " << options.entry_point << "(uint vertex_id : SV_VertexID) {\n";
  } else {
    out << "struct XenonInput { float4 position : SV_Position;";
    for (unsigned i=0;i<16;++i) out << " float4 i" << i << " : TEXCOORD" << i << ";";
    out << " };\nstruct XenonOutput {";
    for (unsigned i=0;i<4;++i) out << " float4 c" << i << " : SV_Target" << i << ";";
    out << " };\nXenonOutput " << options.entry_point << "(XenonInput input) {\n";
  }
  out << "  float4 r[64]; float4 e[64];\n"
      << "  [unroll] for (uint i=0;i<64;++i) { r[i]=0.0.xxxx; e[i]=0.0.xxxx; }\n"
      << "  bool p0=false; int a0=0; int aL=0; float4 ps=0.0.xxxx;\n"
      << "  uint pc=0, guard=0, call_depth=0, loop_depth=0; bool running=true;\n"
      << "  uint call_stack[4], loop_remaining[4]; int loop_value[4], loop_step[4];\n";
  if (shader.stage == ShaderStage::Vertex) out << "  r[0].x = (float)vertex_id;\n";
  else for (unsigned i=0;i<16;++i) out << "  r[" << i << "] = input.i" << i << ";\n";

  out << "  [loop] while (running && guard++ < 65536) {\n"
      << "    switch (pc) {\n";
  for (const auto& cf : shader.control_flow) {
    const auto next = cf.index + 1u;
    out << "      case " << cf.index << ": {\n";
    const auto opcode = cf.opcode;
    const bool is_exec = (static_cast<unsigned>(opcode) >= 1 &&
                          static_cast<unsigned>(opcode) <= 6) ||
                         opcode == ControlFlowOpcode::CondExecPredClean ||
                         opcode == ControlFlowOpcode::CondExecPredCleanEnd;
    if (is_exec) {
      std::string condition = "true";
      if (opcode == ControlFlowOpcode::CondExec ||
          opcode == ControlFlowOpcode::CondExecEnd ||
          opcode == ControlFlowOpcode::CondExecPredClean ||
          opcode == ControlFlowOpcode::CondExecPredCleanEnd) {
        condition = "xenon_bool(" + std::to_string(cf.bool_constant) + ") == " +
                    (cf.condition ? "true" : "false");
      } else if (opcode == ControlFlowOpcode::CondExecPred ||
                 opcode == ControlFlowOpcode::CondExecPredEnd) {
        condition = cf.predicate.condition ? "p0" : "!p0";
      }
      out << "        bool execute = " << condition << ";\n"
          << "        int ar = "
          << (cf.addressing == AddressingMode::Absolute ? "a0" : "aL") << ";\n"
          << "        if (execute) {\n";
      for (std::uint32_t address = cf.target; address < cf.target + cf.count; ++address) {
        auto found = instructions.find(address);
        if (found == instructions.end()) {
          result.diagnostics.emplace_back(
              "control flow references an instruction absent from the decoded IR");
          continue;
        }
        if (found->second->kind == ShaderInstructionKind::Alu) {
          emit_alu(out, found->second->alu, shader.stage);
        } else {
          emit_fetch(out, *found->second);
        }
      }
      const bool ends = opcode == ControlFlowOpcode::ExecEnd ||
                        opcode == ControlFlowOpcode::CondExecEnd ||
                        opcode == ControlFlowOpcode::CondExecPredEnd ||
                        opcode == ControlFlowOpcode::CondExecPredCleanEnd;
      if (ends) out << "          running=false;\n";
      out << "        }\n";
      if (!ends || opcode != ControlFlowOpcode::ExecEnd) {
        out << "        if (running) pc=" << next << ";\n";
      }
    } else if (opcode == ControlFlowOpcode::LoopStart) {
      out << "        uint loop_data=XenonLoopConstants[" << cf.loop_constant
          << " & 31].x;\n"
          << "        uint count=loop_data & 255;\n"
          << "        if (count==0 || loop_depth>=4) { pc=" << cf.target
          << "; break; }\n"
          << "        uint slot=loop_depth++; loop_remaining[slot]=count;\n"
          << "        loop_value[slot]=(int)((loop_data>>8)&255);\n"
          << "        loop_step[slot]=(int)(loop_data<<8)>>24;\n"
          << "        aL=loop_value[slot]; pc=" << next << ";\n";
    } else if (opcode == ControlFlowOpcode::LoopEnd) {
      const auto break_condition = cf.predicate.enabled
                                       ? (cf.predicate.condition ? "p0" : "!p0")
                                       : "false";
      out << "        bool break_loop=" << break_condition << ";\n"
          << "        if (loop_depth==0) { pc=" << next << "; break; }\n"
          << "        uint slot=loop_depth-1;\n"
          << "        if (!break_loop && --loop_remaining[slot]!=0) {\n"
          << "          loop_value[slot]+=loop_step[slot]; aL=loop_value[slot]; pc="
          << cf.target << ";\n"
          << "        } else { --loop_depth; aL=loop_depth ? loop_value[loop_depth-1] : 0; pc="
          << next << "; }\n";
    } else if (opcode == ControlFlowOpcode::CondCall ||
               opcode == ControlFlowOpcode::CondJump) {
      std::string condition = "true";
      if (!cf.unconditional) {
        condition = cf.predicate.enabled
                        ? (cf.predicate.condition ? "p0" : "!p0")
                        : "xenon_bool(" + std::to_string(cf.bool_constant) + ") == " +
                              (cf.condition ? "true" : "false");
      }
      out << "        bool take=" << condition << ";\n";
      if (opcode == ControlFlowOpcode::CondCall) {
        out << "        if (take && call_depth<4) { call_stack[call_depth++]=" << next
            << "; pc=" << cf.target << "; } else pc=" << next << ";\n";
      } else {
        out << "        pc=take ? " << cf.target << " : " << next << ";\n";
      }
    } else if (opcode == ControlFlowOpcode::Return) {
      out << "        if (call_depth) pc=call_stack[--call_depth]; else running=false;\n";
    } else {
      out << "        pc=" << next << ";\n";
    }
    out << "        break; }\n";
  }
  out << "      default: running=false; break;\n"
      << "    }\n"
      << "  }\n";
  out << "  XenonOutput output;\n";
  if (shader.stage == ShaderStage::Vertex) {
    out << "  output.position=e[62];\n";
    for (unsigned i=0;i<16;++i) out << "  output.i" << i << "=e[" << i << "];\n";
  } else {
    for (unsigned i=0;i<4;++i) out << "  output.c" << i << "=e[" << i << "];\n";
  }
  out << "  return output;\n}\n";
  result.hlsl = out.str();
  result.translation_hash = hash_text(result.hlsl);
  result.complete = result.diagnostics.empty();
  return result;
}

}  // namespace xenon::gpu

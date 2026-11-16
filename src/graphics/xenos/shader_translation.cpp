#include "xenon/gpu/shader_translation.hpp"

#include "xenon/memory/types.hpp"

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

// Temporary registers. A shader that addresses any temporary relative to the
// loop index aL keeps all 64 in an array it can index; every other shader
// names each one as a variable of its own (r_N), which the compiler takes
// straight to SSA values. Indexing the array, even by constants, made DXC's
// SPIR-V legalization most of a shader's compile time: AC6's mission shaders
// compiled in 46 ms on average instead of 174 ms, the slowest in 229 ms
// instead of 2.1 s. Set by lower() for the shader it is emitting (lowering
// runs on several compile workers at once).
thread_local bool t_register_array = true;

std::string temporary(unsigned index, bool relative) {
  if (t_register_array || relative)
    return "r[(" + std::to_string(index) + (relative ? " + aL" : "") + ") & 63]";
  return "r_" + std::to_string(index & 63u);
}

// Whether any instruction of `shader` addresses a temporary relative to aL.
bool uses_relative_temporaries(const DecodedShader& shader) {
  for (const auto& instruction : shader.instructions) {
    if (instruction.kind == ShaderInstructionKind::Alu) {
      const auto& alu = instruction.alu;
      for (const auto& source : alu.sources)
        if (source.temporary && source.relative) return true;
      if (!alu.export_data &&
          (alu.vector_destination_relative || alu.scalar_destination_relative))
        return true;
    } else if (instruction.kind == ShaderInstructionKind::VertexFetch) {
      if (instruction.vertex_fetch.source_relative ||
          instruction.vertex_fetch.destination_relative)
        return true;
    } else if (instruction.texture_fetch.source_relative ||
               instruction.texture_fetch.destination_relative) {
      return true;
    }
  }
  return false;
}

// Relative temporaries add the loop index aL. Relative constants add a0 when
// the instruction's const_address_register_relative bit is set, otherwise aL.
std::string source(const AluSource& value, ShaderStage stage,
                   bool constant_address_register_relative) {
  std::ostringstream out;
  if (value.negate) out << "-";
  if (value.absolute_value) out << "abs(";
  if (value.temporary) {
    out << temporary(value.index, value.relative);
  } else {
    out << "XenonFloatConstants[((" << value.index;
    if (value.relative) out << (constant_address_register_relative ? " + a0" : " + aL");
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
  // Xenos exports both the vector and scalar ALU results to vector_dest. The
  // scalar destination field only selects a temporary register when EXPORT_DATA
  // is clear.
  const auto index = alu.export_data
                         ? alu.vector_destination
                         : (scalar ? alu.scalar_destination : alu.vector_destination);
  const bool relative = !alu.export_data &&
                        (scalar ? alu.scalar_destination_relative
                                : alu.vector_destination_relative);
  std::ostringstream out;
  if (alu.export_data) {
    out << "e[" << unsigned(index) << "]";
  } else {
    out << temporary(index, relative);
  }
  return out.str();
}

// The vector unit's operation, as HLSL for this opcode alone: sources s0, s1
// and s2, result pv, and the p0, a0 and vector_killed side effects. Emitting
// only the instruction's own operation - instead of a call to one function
// holding every opcode, which DXC had to inline and fold again for each
// instruction - is most of what a shader's compile time was. The semantics
// are the Xenos ones the CPU twin (vertex_position_evaluator.cpp) evaluates.
std::string vector_operation(unsigned op) {
  switch (op) {
    case 0: return "pv = s0 + s1;";
    case 1: return "pv = xenon_legacy_mul4(s0, s1);";
    case 2: return "pv = max(s0, s1);";
    case 3: return "pv = min(s0, s1);";
    case 4: return "pv = float4(s0 == s1);";
    case 5: return "pv = float4(s0 > s1);";
    case 6: return "pv = float4(s0 >= s1);";
    case 7: return "pv = float4(s0 != s1);";
    case 8: return "pv = frac(s0);";
    case 9: return "pv = trunc(s0);";
    case 10: return "pv = floor(s0);";
    case 11: return "pv = xenon_legacy_mul4(s0, s1) + s2;";
    case 12: return "pv = select(s0 == 0.0, s1, s2);";
    case 13: return "pv = select(s0 >= 0.0, s1, s2);";
    case 14: return "pv = select(s0 > 0.0, s1, s2);";
    // Dot products are sums of Xenos multiplications, each with the Shader
    // Model 3 rule (0 * anything = 0, infinity and NaN included). The host's
    // dot() gives NaN for 0 * inf: AC6's luminance and bloom reductions
    // weight channels by 0, and the NaN those products left spread through
    // the downsample chain (8-pixel blocks on screen, the exposure collapsing).
    case 15: return "{ float4 p = xenon_legacy_mul4(s0, s1); pv = (((p.x + p.y) + p.z) + p.w).xxxx; }";
    case 16: return "{ float4 p = xenon_legacy_mul4(s0, s1); pv = ((p.x + p.y) + p.z).xxxx; }";
    case 17: return "{ float4 p = xenon_legacy_mul4(s0, s1); pv = ((p.x + p.y) + s2.x).xxxx; }";
    // cube: source 1 is the direction as .z_xy. Returns T, S, 2 * major axis
    // and the D3DCUBEMAP_FACES face.
    case 18:
      return "{ float x = s0.z, y = s0.w, z = s0.x; float3 q = abs(float3(x, y, z));\n"
             "            if (q.z >= q.x && q.z >= q.y) pv = float4(-y, z < 0.0 ? -x : x, 2.0 * z, z < 0.0 ? 5.0 : 4.0);\n"
             "            else if (q.y >= q.x) pv = float4(y < 0.0 ? -z : z, x, 2.0 * y, y < 0.0 ? 3.0 : 2.0);\n"
             "            else pv = float4(-y, x < 0.0 ? z : -z, 2.0 * x, x < 0.0 ? 1.0 : 0.0); }";
    case 19: return "pv = max(max(s0.x, s0.y), max(s0.z, s0.w)).xxxx;";
    case 20:
    case 21:
    case 22:
    case 23: {
      static constexpr const char* compare[] = {"== 0.0", "!= 0.0", "> 0.0", ">= 0.0"};
      const std::string c = compare[op - 20u];
      return "p0 = s0.w == 0.0 && s1.w " + c + "; pv = (s1.x " + c +
             " && s0.x == 0.0) ? 0.0.xxxx : (s0.x + 1.0).xxxx;";
    }
    case 24:
    case 25:
    case 26:
    case 27: {
      static constexpr const char* compare[] = {"==", ">", ">=", "!="};
      return std::string("vector_killed = any(s0 ") + compare[op - 24u] +
             " s1); pv = (vector_killed ? 1.0 : 0.0).xxxx;";
    }
    case 28: return "pv = float4(1.0, xenon_legacy_mul(s0.y, s1.y), s0.z, s1.w);";
    case 29: return "a0 = (int)clamp(floor(s0.w + 0.5), -256.0, 255.0); pv = max(s0, s1);";
    default: return "pv = 0.0.xxxx;";
  }
}

// The scalar unit's operation for this opcode: operand x is source 3's w
// (s2.w), y its x (s2.x), b the mulsc/addsc/subsc temporary and ps the
// previous scalar result; it sets v (written to ps_new as v.xxxx) and the
// p0, a0 and scalar_killed side effects. retain_prev (50) sets ps_new itself.
std::string scalar_operation(unsigned op) {
  switch (op) {
    case 0: return "v = x + y;";
    case 1: return "v = x + pp;";
    case 2: return "v = xenon_legacy_mul(x, y);";
    case 3: return "v = xenon_legacy_mul(x, pp);";
    case 4:
      return "v = (pp == -XENON_FLT_MAX || !isfinite(pp) || !isfinite(y) || y <= 0.0) "
             "? -XENON_FLT_MAX : xenon_legacy_mul(x, pp);";
    case 5: return "v = x >= y ? x : y;";
    case 6: return "v = x < y ? x : y;";
    case 7: return "v = x == 0.0;";
    case 8: return "v = x > 0.0;";
    case 9: return "v = x >= 0.0;";
    case 10: return "v = x != 0.0;";
    case 11: return "v = x - floor(x);";
    case 12: return "v = trunc(x);";
    case 13: return "v = floor(x);";
    case 14: return "v = exp2(x);";
    case 15: return "v = x == 1.0 ? 0.0 : log2(x); if (isinf(v) && v < 0.0) v = -XENON_FLT_MAX;";
    case 16: return "v = x == 1.0 ? 0.0 : log2(x);";
    case 17: return "v = xenon_clamp_infinity(x == 1.0 ? 1.0 : 1.0 / x, false);";
    case 18: return "v = xenon_clamp_infinity(x == 1.0 ? 1.0 : 1.0 / x, true);";
    case 19: return "v = x == 1.0 ? 1.0 : 1.0 / x;";
    case 20: return "v = xenon_clamp_infinity(x == 1.0 ? 1.0 : 1.0 / sqrt(x), false);";
    case 21: return "v = xenon_clamp_infinity(x == 1.0 ? 1.0 : 1.0 / sqrt(x), true);";
    case 22: return "v = x == 1.0 ? 1.0 : 1.0 / sqrt(x);";
    case 23: return "a0 = (int)clamp(floor(x + 0.5), -256.0, 255.0); v = x >= y ? x : y;";
    case 24: return "a0 = (int)clamp(floor(x), -256.0, 255.0); v = x >= y ? x : y;";
    case 25: return "v = x - y;";
    case 26: return "v = x - pp;";
    case 27: return "p0 = x == 0.0; v = p0 ? 0.0 : 1.0;";
    case 28: return "p0 = x != 0.0; v = p0 ? 0.0 : 1.0;";
    case 29: return "p0 = x > 0.0; v = p0 ? 0.0 : 1.0;";
    case 30: return "p0 = x >= 0.0; v = p0 ? 0.0 : 1.0;";
    case 31: return "p0 = x == 1.0; v = p0 ? 0.0 : (x == 0.0 ? 1.0 : x);";
    case 32: return "p0 = x - 1.0 <= 0.0; v = p0 ? 0.0 : x - 1.0;";
    case 33: return "p0 = false; v = XENON_FLT_MAX;";
    case 34: return "p0 = x == 0.0; v = p0 ? 0.0 : x;";
    case 35: return "scalar_killed = x == 0.0; v = scalar_killed;";
    case 36: return "scalar_killed = x > 0.0; v = scalar_killed;";
    case 37: return "scalar_killed = x >= 0.0; v = scalar_killed;";
    case 38: return "scalar_killed = x != 0.0; v = scalar_killed;";
    case 39: return "scalar_killed = x == 1.0; v = scalar_killed;";
    case 40: return "v = sqrt(x);";
    case 42:
    case 43: return "v = xenon_legacy_mul(x, b.x);";
    case 44:
    case 45: return "v = x + b.x;";
    case 46:
    case 47: return "v = x - b.x;";
    case 48: return "v = sin(x);";
    case 49: return "v = cos(x);";
    // retain_prev: the whole previous result, not its X replicated.
    case 50: return "ps_new = ps;";
    default: return "v = 0.0;";
  }
}

void emit_alu(std::ostringstream& out, const AluInstruction& alu,
              ShaderStage stage, bool memexport_enabled) {
  const bool a0_constants = alu.constant_address_register_relative;
  out << "        if (" << predicate(alu.predicate) << ") {\n"
      << "          float4 s0 = " << source(alu.sources[0], stage, a0_constants) << ";\n"
      << "          float4 s1 = " << source(alu.sources[1], stage, a0_constants) << ";\n"
      << "          float4 s2 = " << source(alu.sources[2], stage, a0_constants) << ";\n";
  // The scalar unit reads source 3. mulsc/addsc/subsc pair its constant with
  // a temporary, which takes source 3's negation and the constant abs flag.
  std::string scalar_b = "s2";
  if (alu.scalar_opcode >= 42u && alu.scalar_opcode <= 47u) {
    AluSource temporary = alu.sources[2];
    temporary.index = alu.scalar_constant_temporary;
    temporary.temporary = true;
    temporary.relative = false;
    out << "          float4 s3t = " << source(temporary, stage, a0_constants) << ";\n";
    scalar_b = "s3t";
  }
  out << "          bool vector_killed = false, scalar_killed = false;\n"
      << "          // vector op " << unsigned(alu.vector_opcode) << ", scalar op "
      << unsigned(alu.scalar_opcode) << "\n"
      << "          float4 pv = 0.0.xxxx;\n"
      << "          " << vector_operation(alu.vector_opcode) << "\n"
      << "          float4 ps_new;\n";
  if (alu.scalar_opcode == 50u) {
    out << "          " << scalar_operation(alu.scalar_opcode) << "\n";
  } else {
    out << "          { float x = s2.w; float y = s2.x; float4 b = " << scalar_b
        << "; float pp = ps.x; float v = 0.0;\n"
        << "            " << scalar_operation(alu.scalar_opcode) << "\n"
        << "            ps_new = v.xxxx; }\n";
  }
  if (alu.vector_clamp) out << "          pv = saturate(pv);\n";
  if (alu.scalar_clamp) out << "          ps_new = saturate(ps_new);\n";
  // Both results of an export go to vector_dest. A component selected by both
  // write masks receives 1.0; with scalar_dest_rel set, one selected by
  // neither receives 0.0.
  std::uint8_t vector_mask = alu.vector_write_mask;
  std::uint8_t scalar_mask = alu.scalar_write_mask;
  std::uint8_t one_mask = 0, zero_mask = 0;
  if (alu.export_data) {
    one_mask = vector_mask & scalar_mask;
    if (alu.scalar_destination_relative) zero_mask = 0xFu & ~(vector_mask | scalar_mask);
    vector_mask &= static_cast<std::uint8_t>(~one_mask);
    scalar_mask &= static_cast<std::uint8_t>(~one_mask);
  }
  write_masked(out, destination(alu, false), "pv", vector_mask, 10);
  write_masked(out, destination(alu, true), "ps_new", scalar_mask, 10);
  write_masked(out, destination(alu, false), "(1.0).xxxx", one_mask, 10);
  write_masked(out, destination(alu, false), "(0.0).xxxx", zero_mask, 10);
  if (memexport_enabled && alu.export_data && alu.vector_destination >= 33u &&
      alu.vector_destination <= 37u &&
      (vector_mask | scalar_mask | one_mask | zero_mask) != 0) {
    out << "          memexport_written_mask |= "
        << (1u << (alu.vector_destination - 33u)) << "u;\n";
  }
  out << "          ps = ps_new;\n";
  if (stage == ShaderStage::Pixel) {
    if (memexport_enabled) {
      out << "          if (vector_killed || scalar_killed) { xenon_memexport_flush(e[32], "
             "e[33], e[34], e[35], e[36], e[37], memexport_written_mask); discard; }\n";
    } else {
      out << "          if (vector_killed || scalar_killed) discard;\n";
    }
  }
  out << "        }\n";
}

void emit_fetch(std::ostringstream& out, const DecodedInstruction& instruction) {
  if (instruction.kind == ShaderInstructionKind::VertexFetch) {
    const auto& fetch = instruction.vertex_fetch;
    out << "        if (" << predicate(fetch.predicate) << ") {\n";
    // A full vfetch computes and latches the element's address (the fetch
    // constant's base plus index * stride); a vfetch_mini reads at that
    // latched address plus its own offset and never reads an index register.
    // Titles overwrite the index register with the full fetch's own result
    // (r0 = vfetch r0.x), so re-reading it for the mini fetches that follow
    // fetched the rest of the vertex from wherever that data pointed:
    // AC6's characters came out contorted and its aircraft torn apart.
    if (!fetch.mini_fetch) {
      out << "          vfetch_base = xenon_vertex_fetch_base(" << unsigned(fetch.fetch_constant)
          << ", " << unsigned(fetch.fetch_constant_select) << ", xenon_fetch_index("
          << temporary(fetch.source_register, fetch.source_relative) << "["
          << unsigned(fetch.source_swizzle & 3u) << "], "
          << (fetch.index_rounded ? "true" : "false") << "), " << unsigned(fetch.stride_dwords)
          << ");\n";
    }
    out << "          float4 fetched = xenon_vertex_decode(vfetch_base, " << fetch.offset_dwords
        << ", " << unsigned(fetch.data_format) << ", " << (fetch.signed_data ? "true" : "false")
        << ", " << (fetch.normalized ? "true" : "false") << ");\n"
        << "          xenon_write_fetch("
        << temporary(fetch.destination_register, fetch.destination_relative) << ", fetched, "
        << fetch.destination_swizzle << ");\n"
        << "        }\n";
  } else {
    const auto& fetch = instruction.texture_fetch;
    const auto offsets = [&] {
      std::ostringstream o;
      o << "float3(" << (float(fetch.offsets_half_texels[0]) / 2.0f) << ","
        << (float(fetch.offsets_half_texels[1]) / 2.0f) << ","
        << (float(fetch.offsets_half_texels[2]) / 2.0f) << ")";
      return o.str();
    };
    out << "        if (" << predicate(fetch.predicate) << ") {\n"
        << "          float4 coord = "
        << temporary(fetch.source_register, fetch.source_relative)
        << swizzle(fetch.source_swizzle) << ";\n";
    // Xenos setTextureLod / setTextureGradientsHorz / setTextureGradientsVert
    // write per-shader-invocation state that later tfetches read when their
    // use_register_lod / use_register_gradients flags are set.
    switch (fetch.opcode) {
      case 24:
        out << "          xenon_register_lod = coord.x;\n        }\n";
        return;
      case 25:
        out << "          xenon_grad_h = float4(coord.xyz, 0.0);\n        }\n";
        return;
      case 26:
        out << "          xenon_grad_v = float4(coord.xyz, 0.0);\n        }\n";
        return;
      default:
        break;
    }
    const float lod_bias = float(fetch.lod_bias_sixteenths) / 16.0f;
    if (fetch.opcode == 1) {
      out << "          float4 fetched = xenon_texture_fetch(" << unsigned(fetch.fetch_constant)
          << ", " << unsigned(fetch.dimension) << ", coord, " << lod_bias << ", "
          << (fetch.use_computed_lod ? "true" : "false") << ", "
          << (fetch.use_register_lod ? "true" : "false") << ", "
          << (fetch.use_register_gradients ? "true" : "false")
          << ", xenon_grad_h, xenon_grad_v, xenon_register_lod, "
          << (fetch.unnormalized_coordinates ? "true" : "false") << ", " << offsets()
          << ");\n"
          << "          fetched = xenon_apply_texture_exp_adjust("
          << unsigned(fetch.fetch_constant) << ", fetched);\n";
    } else if (fetch.opcode == 17) {
      out << "          float4 fetched = xenon_texture_computed_lod("
          << unsigned(fetch.fetch_constant) << ", " << unsigned(fetch.dimension)
          << ", coord, " << lod_bias << ", "
          << (fetch.unnormalized_coordinates ? "true" : "false") << ");\n";
    } else if (fetch.opcode == 18) {
      // getTextureGradients: (ddx(s), ddy(s), ddx(t), ddy(t)).
      out << "          float4 fetched = float4(ddx_coarse(coord.x), ddy_coarse(coord.x), "
             "ddx_coarse(coord.y), ddy_coarse(coord.y));\n";
    } else {
      out << "          float4 fetched = xenon_texture_weights("
          << unsigned(fetch.fetch_constant) << ", " << unsigned(fetch.dimension)
          << ", coord, " << (fetch.unnormalized_coordinates ? "true" : "false") << ", "
          << offsets() << ");\n";
    }
    out << "          xenon_write_fetch("
        << temporary(fetch.destination_register, fetch.destination_relative) << ", fetched, "
        << fetch.destination_swizzle << ");\n"
        << "        }\n";
  }
}

// The single guest-memory declaration emit_prelude() writes, which the
// segmented binding replaces.
constexpr std::string_view kGuestMemoryDeclaration =
    "#ifdef XENON_MEMORY_RW\n"
    "RWByteAddressBuffer XenonGuestMemory : register(u0);\n"
    "#else\n"
    "ByteAddressBuffer XenonGuestMemory : register(t0);\n"
    "#endif\n";

void replace_all(std::string& text, std::string_view from, std::string_view to) {
  for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
    text.replace(at, from.size(), to);
}

// Rewrites a lowered guest shader for segmented guest memory: one buffer per
// segment (read-only segment 0 at t0 and segment i at t(128 + i), past the
// texture arrays; read-write segment i at u(i)) and accessors that pick the
// segment from the address's high bits with a branch. False, with a
// diagnostic, when an access remains that no accessor covers.
bool segment_guest_memory(std::string& hlsl, std::uint8_t shift, std::uint8_t count, bool rw,
                          std::vector<std::string>& diagnostics) {
  const auto declaration = hlsl.find(kGuestMemoryDeclaration);
  if (declaration == std::string::npos) {
    diagnostics.push_back("segmented guest memory: the prelude's declaration was not found");
    return false;
  }
  std::ostringstream block;
  block << "#define XENON_GUEST_SEGMENT_SHIFT " << unsigned(shift) << "u\n"
        << "#define XENON_GUEST_SEGMENT_MASK " << ((1ull << shift) - 1ull) << "u\n"
        << "#define XENON_GUEST_MEMORY_BYTES " << memory::kPhysicalMemorySize << "u\n";
  for (unsigned i = 0; i < count; ++i) {
    if (rw) block << "RWByteAddressBuffer XenonGuestMemory" << i << " : register(u" << i << ");\n";
    else block << "ByteAddressBuffer XenonGuestMemory" << i << " : register(t" << (i ? 128u + i : 0u)
               << ");\n";
  }
  const auto each_segment = [&](const char* statement) {
    std::ostringstream cases;
    cases << "  const uint offset = address & XENON_GUEST_SEGMENT_MASK;\n"
          << "  [branch] switch (address >> XENON_GUEST_SEGMENT_SHIFT) {\n";
    for (unsigned i = 0; i < count; ++i) {
      std::string line = statement;
      replace_all(line, "SEGMENT", "XenonGuestMemory" + std::to_string(i));
      cases << "    case " << i << "u: " << line << "\n";
    }
    cases << "    default: break;\n  }\n";
    return cases.str();
  };
  block << "uint4 xenon_guest_load4(uint address) {\n"
        << each_segment("return SEGMENT.Load4(offset);")
        << "  return uint4(0u, 0u, 0u, 0u);\n}\n";
  if (rw) {
    block << "void xenon_guest_store(uint address, uint value) {\n"
          << each_segment("SEGMENT.Store(offset, value); break;") << "}\n"
          << "void xenon_guest_store2(uint address, uint2 value) {\n"
          << each_segment("SEGMENT.Store2(offset, value); break;") << "}\n"
          << "void xenon_guest_store4(uint address, uint4 value) {\n"
          << each_segment("SEGMENT.Store4(offset, value); break;") << "}\n"
          << "void xenon_guest_interlocked_and(uint address, uint value, out uint original) {\n"
          << "  original = 0u;\n"
          << each_segment("SEGMENT.InterlockedAnd(offset, value, original); break;") << "}\n"
          << "void xenon_guest_interlocked_or(uint address, uint value, out uint original) {\n"
          << "  original = 0u;\n"
          << each_segment("SEGMENT.InterlockedOr(offset, value, original); break;") << "}\n";
  }
  hlsl.replace(declaration, kGuestMemoryDeclaration.size(), block.str());
  replace_all(hlsl, "XenonGuestMemory.Load4(", "xenon_guest_load4(");
  replace_all(hlsl, "XenonGuestMemory.Store4(", "xenon_guest_store4(");
  replace_all(hlsl, "XenonGuestMemory.Store2(", "xenon_guest_store2(");
  replace_all(hlsl, "XenonGuestMemory.Store(", "xenon_guest_store(");
  replace_all(hlsl, "XenonGuestMemory.InterlockedAnd(", "xenon_guest_interlocked_and(");
  replace_all(hlsl, "XenonGuestMemory.InterlockedOr(", "xenon_guest_interlocked_or(");
  replace_all(hlsl, "XenonGuestMemory.GetDimensions(memory_size);",
              "memory_size = XENON_GUEST_MEMORY_BYTES;");
  if (hlsl.find("XenonGuestMemory.") != std::string::npos) {
    diagnostics.push_back(
        "segmented guest memory: a guest-memory access has no segmented accessor");
    return false;
  }
  return true;
}

void emit_prelude(std::ostringstream& out, bool guest_memory_rw,
                  bool memexport_enabled) {
  if (guest_memory_rw) out << "#define XENON_MEMORY_RW 1\n";
  out << R"hlsl(
cbuffer XenonShaderConstants : register(b0) {
  // Xenos has independent 256-vector banks for VS and PS. Pixel shader
  // lowering adds 256 to its architectural constant index.
  float4 XenonFloatConstants[512];
  uint4 XenonBoolConstants[8];
  uint4 XenonLoopConstants[32];
  uint4 XenonVertexFetchConstants[32];
  uint4 XenonDrawState[24];
};
#ifdef XENON_MEMORY_RW
RWByteAddressBuffer XenonGuestMemory : register(u0);
#else
ByteAddressBuffer XenonGuestMemory : register(t0);
#endif
Texture1D<float4> XenonTextures1D[32] : register(t1);
Texture2D<float4> XenonTextures2D[32] : register(t33);
Texture3D<float4> XenonTextures3D[32] : register(t65);
TextureCube<float4> XenonTexturesCube[32] : register(t97);
SamplerState XenonSamplers[32] : register(s0);

// The vertex grouper's index: the draw's index (generated or fetched) plus
// VGT_INDX_OFFSET, kept to its 24 bits and clamped to
// [VGT_MIN_VTX_INDX, VGT_MAX_VTX_INDX].
uint xenon_vertex_index(uint index) {
  uint4 vgt = XenonDrawState[1];
  return clamp((index + vgt.y) & 0xFFFFFFu, vgt.z & 0xFFFFFFu, vgt.w & 0xFFFFFFu);
}

// The guest's exported position as host clip coordinates over the whole
// render target. PA_CL_VTE_CNTL's formats first: VTX_W0_FMT clear means W
// holds 1/W0, and VTX_XY_FMT/VTX_Z_FMT mean those components were already
// divided by W0. Then the guest viewport transform (XenonDrawState[2], in
// render-target pixels times W) and the host's pixels-to-NDC mapping
// (XenonDrawState[3].yz = 2 / width, 2 / height; Y down in pixels, up in
// NDC - Vulkan flips its viewport to match).
float4 xenon_clip_position(float4 position) {
  uint flags = XenonDrawState[3].x;
  if ((flags & 4u) == 0u) position.w = 1.0 / position.w;
  if ((flags & 1u) != 0u) position.xy *= position.w;
  if ((flags & 2u) != 0u) position.z *= position.w;
  return position;
}
float4 xenon_host_position(float4 position) {
  position = xenon_clip_position(position);
  // The host depth transform's Z part (XenonDrawState[4].xy, see
  // host_depth_transform): OpenGL-style Z to 0...W, a reversed range flipped,
  // or the whole viewport Z transform when the guest disables clipping.
  float2 z_transform = asfloat(XenonDrawState[4].xy);
  position.z = position.z * z_transform.x + z_transform.y * position.w;
  float4 transform = asfloat(XenonDrawState[2]);
  float2 pixels = position.xy * transform.xy + transform.zw * position.w;
  float2 to_ndc = asfloat(XenonDrawState[3].yz);
  return float4(pixels.x * to_ndc.x - position.w, position.w - pixels.y * to_ndc.y,
                position.z, position.w);
}

// PA_CL_CLIP_CNTL UCP_ENA_n (XenonDrawState[4].w) with the planes in
// XenonDrawState[5..10]: the guest clip-space position's distance to each
// enabled plane; a disabled plane never clips.
void xenon_user_clip_distances(float4 clip_position, out float4 clip0, out float2 clip1) {
  uint enabled = XenonDrawState[4].w;
  float distances[6];
  [unroll] for (uint i = 0u; i < 6u; ++i)
    distances[i] = ((enabled >> i) & 1u) != 0u
                       ? dot(clip_position, asfloat(XenonDrawState[5u + i])) : 0.0;
  clip0 = float4(distances[0], distances[1], distances[2], distances[3]);
  clip1 = float2(distances[4], distances[5]);
}

bool xenon_bool(uint index) {
  uint word = XenonBoolConstants[(index >> 7) & 7][(index >> 5) & 3];
  return ((word >> (index & 31)) & 1) != 0;
}

bool xenon_compare(float value, float reference, uint function) {
  switch (function & 7u) {
    case 0u: return false;
    case 1u: return value < reference;
    case 2u: return value == reference;
    case 3u: return value <= reference;
    case 4u: return value > reference;
    case 5u: return value != reference;
    case 6u: return value >= reference;
    default: return true;
  }
}

uint xenon_alpha_to_mask(float alpha, float2 position) {
  uint sample_count = clamp(XenonDrawState[0].z, 1u, 4u);
  // Xenos offset index: Y is the low bit, X is the high bit.
  uint quadrant = (uint(floor(position.y)) & 1u) |
                  ((uint(floor(position.x)) & 1u) << 1u);
  uint offset = (XenonDrawState[0].w >> (quadrant * 2u)) & 3u;
  float a = saturate(alpha);
  float o = float(offset);
  if (sample_count >= 4u) {
    uint coverage = 0u;
    if (a >= 0.75 - o / 16.0) coverage |= 1u;
    if (a >= 0.25 - o / 16.0) coverage |= 2u;
    if (a >= 0.50 - o / 16.0) coverage |= 4u;
    if (a >= 1.00 - o / 16.0) coverage |= 8u;
    return coverage;
  }
  if (sample_count >= 2u) {
    // Native host 2x sample order is the reverse of Xenos vertical order.
    uint coverage = 0u;
    if (a >= 0.50 - o / 8.0) coverage |= 2u;
    if (a >= 1.00 - o / 8.0) coverage |= 1u;
    return coverage;
  }
  return a >= 1.00 - o / 4.0 ? 1u : 0u;
}

// Xbox 360 D24FS8 stores positive depth on a 20-bit mantissa / 4-bit exponent
// lattice. These are bit-for-bit counterparts of the host-side conversion.
uint xenon_float32_to_20e4(float value, bool round_nearest_even) {
  if (!(value > 0.0)) return 0u;
  uint bits = asuint(value);
  if (bits >= 0x3FFFFFF8u) return 0x00FFFFFFu;
  if (bits < 0x38800000u) {
    uint shift = min(113u - (bits >> 23u), 24u);
    bits = (0x00800000u | (bits & 0x007FFFFFu)) >> shift;
  } else {
    bits += 0xC8000000u;
  }
  if (round_nearest_even) bits += 3u + ((bits >> 3u) & 1u);
  return (bits >> 3u) & 0x00FFFFFFu;
}
float xenon_float20e4_to_float32(uint value) {
  value &= 0x00FFFFFFu;
  if (value == 0u) return 0.0;
  uint mantissa = value & 0x000FFFFFu;
  int exponent = int(value >> 20u);
  if (exponent == 0) {
    int highest = firstbithigh(mantissa);
    uint shift = 20u - uint(highest);
    exponent = 1 - int(shift);
    mantissa = (mantissa << shift) & 0x000FFFFFu;
  }
  uint ieee = (uint(exponent + 112) << 23u) | (mantissa << 3u);
  return asfloat(ieee);
}
float xenon_quantize_float20e4(float value, bool round_nearest_even) {
  return xenon_float20e4_to_float32(
      xenon_float32_to_20e4(value, round_nearest_even));
}

float xenon_legacy_mul(float a, float b) {
  return (a == 0.0 || b == 0.0) ? 0.0 : a * b;
}
float4 xenon_legacy_mul4(float4 a, float4 b) {
  return float4(xenon_legacy_mul(a.x,b.x), xenon_legacy_mul(a.y,b.y),
                xenon_legacy_mul(a.z,b.z), xenon_legacy_mul(a.w,b.w));
}

)hlsl"  // MSVC limits one literal to 16 KB (C2026); adjacent literals join.
R"hlsl(static const float XENON_FLT_MAX = 3.402823466e+38;
// Infinite results of the clamping reciprocal forms: _CLAMP saturates to
// +-FLT_MAX, _FF flushes to signed zero.
float xenon_clamp_infinity(float value, bool to_zero) {
  if (!isinf(value)) return value;
  if (to_zero) return value < 0.0 ? -0.0 : 0.0;
  return value < 0.0 ? -XENON_FLT_MAX : XENON_FLT_MAX;
}

)hlsl"  // MSVC limits one literal to 16 KB (C2026); adjacent literals join.
R"hlsl(// Xenos endian swap modes applied to fetched 32-bit words: 1 = 8-in-16,
// 2 = 8-in-32, 3 = 16-in-32 (8-in-32 is both of the others combined).
uint4 xenon_endian_swap(uint4 value, uint endian) {
  if (endian == 1u || endian == 2u)
    value = ((value & 0x00FF00FFu) << 8u) | ((value >> 8u) & 0x00FF00FFu);
  if (endian == 2u || endian == 3u)
    value = (value << 16u) | (value >> 16u);
  return value;
}

// The vertex a vfetch reads: its index operand floored, or rounded to the
// nearest (floor(x + 0.5)) for the rounded form. Titles compute indices with
// reciprocals (index = a * rcp(b)); the host's reciprocal may land a few ULPs
// below the Xenos result, so an index that is exactly 3 on the console
// arrives as 2.9999998 and floors to the previous vertex. Ring and grid
// indices (AC6's terrain and sea pick a slot as frac(n / 40) * 40) carry the
// error of the larger value they came from, so a slot of 1 arrives as
// 0.999999. The floor therefore tolerates 2.5e-4 - the fixed bias sal063's
// AC6 recomp cured the game's black-triangle terrain with - or 2^-21 of the
// index's magnitude where float spacing outgrows that, capped at a quarter,
// so an exact integer never floors up where floats grow sparse; both stay far
// below any fraction an index carries on purpose.
uint xenon_fetch_index(float index, bool rounded) {
  if (rounded) return (uint)floor(index + 0.5);
  return (uint)floor(index + min(max(2.5e-4, abs(index) * 4.76837158203125e-7), 0.25));
}

// A full vfetch's latched element: its byte address (the fetch constant's
// base plus index * stride dwords) and the constant's endian mode. vfetch_mini
// instructions read at this address plus their own offset.
uint2 xenon_vertex_fetch_base(uint fetch_constant, uint select, uint index, uint stride) {
  // Three vertex fetch constants share each fetch slot, packed by the host as
  // (dword address << 2) | endian in .x/.z/.w for selects 0/1/2.
  uint4 slot = XenonVertexFetchConstants[fetch_constant & 31];
  uint packed = select == 0u ? slot.x : (select == 1u ? slot.z : slot.w);
  return uint2((packed & ~3u) + 4 * index * stride, packed & 3u);
}

float4 xenon_vertex_decode(uint2 base, int offset, uint format, bool is_signed,
                           bool normalized) {
  uint address = base.x + 4 * offset;
  uint4 raw = xenon_endian_swap(XenonGuestMemory.Load4(address), base.y);
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
  } else if (format == 16) {
    // 10_11_11: X 11 bits, Y 11 bits, Z 10 bits; W defaults to 1.
    u=uint4(raw.x&2047,(raw.x>>11)&2047,raw.x>>22,1); maximum=uint4(2047,2047,1023,1);
    s=int4((int)(u.x<<21)>>21,(int)(u.y<<21)>>21,(int)(u.z<<22)>>22,1);
  } else if (format == 17) {
    // 11_11_10: X 10 bits, Y 11 bits, Z 11 bits; W defaults to 1.
    u=uint4(raw.x&1023,(raw.x>>10)&2047,raw.x>>21,1); maximum=uint4(1023,2047,2047,1);
    s=int4((int)(u.x<<22)>>22,(int)(u.y<<21)>>21,(int)(u.z<<21)>>21,1);
  } else if (format == 25 || format == 26) {
    u=uint4(raw.x&65535,raw.x>>16,raw.y&65535,raw.y>>16); maximum=65535;
    s=(int4)(u<<16)>>16;
    if (format == 25) { u.zw=uint2(0,1); s.zw=int2(0,1); maximum.zw=1; }
  }
  if (normalized) return is_signed ? max(float4(s) / float4(max(maximum >> 1, 1)), -1.0) : float4(u) / float4(maximum);
  return is_signed ? float4(s) : float4(u);
}
// Texture fetch constant scalars packed by the host into .y: the signed
// result exponent adjustment (low 16 bits) and the fetch-constant LOD bias in
// 1/32 units (high 16 bits).
int xenon_texture_exp_adjust(uint fetch_constant) {
  return (asint(XenonVertexFetchConstants[fetch_constant & 31].y) << 16) >> 16;
}
float xenon_texture_constant_lod_bias(uint fetch_constant) {
  return float(asint(XenonVertexFetchConstants[fetch_constant & 31].y) >> 16) / 32.0;
}
// Xenos texel offsets are in texel units, so for normalized coordinates they
// are divided by the size. The small epsilon (1.5/1024 of a texel, as in
// Xenia) resolves point-sampling ambiguity for coordinates exactly between
// texels.
static const float XENON_TEXEL_ROUNDING = 1.5 / 1024.0;

float4 xenon_texture_fetch(uint fetch_constant, uint dimension, float4 coord,
                           float instruction_lod_bias, bool computed_lod,
                           bool register_lod, bool register_gradients,
                           float4 grad_h, float4 grad_v, float register_lod_value,
                           bool unnormalized, float3 offset) {
  uint slot=fetch_constant & 31;
  // Explicit LOD sources, in D3D order: fetch constant + instruction + register.
  float lod=xenon_texture_constant_lod_bias(fetch_constant) + instruction_lod_bias +
            (register_lod ? register_lod_value : 0.0);
#ifdef XENON_PIXEL_SHADER
  bool gradients=computed_lod;
#else
  // Implicit derivatives do not exist outside pixel shaders.
  bool gradients=computed_lod && register_gradients;
#endif
  float scale=exp2(lod);
  if (dimension==0) {
    uint width, levels; XenonTextures1D[slot].GetDimensions(0,width,levels);
    float size=max(width,1);
    float t=offset.x + XENON_TEXEL_ROUNDING;
    float c=unnormalized ? (coord.x + t) / size : coord.x + t / size;
    float gh=grad_h.x, gv=grad_v.x;
    if (unnormalized) { gh/=size; gv/=size; }
    if (gradients) {
      if (register_gradients) return XenonTextures1D[slot].SampleGrad(XenonSamplers[slot],c,gh*scale,gv*scale);
#ifdef XENON_PIXEL_SHADER
      return XenonTextures1D[slot].SampleBias(XenonSamplers[slot],c,lod);
#endif
    }
    return XenonTextures1D[slot].SampleLevel(XenonSamplers[slot],c,lod);
  }
  if (dimension==1) {
    uint width,height,levels; XenonTextures2D[slot].GetDimensions(0,width,height,levels);
    float2 size=max(float2(width,height),1.0);
    float2 t=offset.xy + XENON_TEXEL_ROUNDING;
    float2 c=unnormalized ? (coord.xy + t) / size : coord.xy + t / size;
    float2 gh=grad_h.xy, gv=grad_v.xy;
    if (unnormalized) { gh/=size; gv/=size; }
    if (gradients) {
      if (register_gradients) return XenonTextures2D[slot].SampleGrad(XenonSamplers[slot],c,gh*scale,gv*scale);
#ifdef XENON_PIXEL_SHADER
      return XenonTextures2D[slot].SampleBias(XenonSamplers[slot],c,lod);
#endif
    }
    return XenonTextures2D[slot].SampleLevel(XenonSamplers[slot],c,lod);
  }
  if (dimension==2) {
    uint width,height,depth,levels; XenonTextures3D[slot].GetDimensions(0,width,height,depth,levels);
    float3 size=max(float3(width,height,depth),1.0);
    float3 t=offset + XENON_TEXEL_ROUNDING;
    float3 c=unnormalized ? (coord.xyz + t) / size : coord.xyz + t / size;
    float3 gh=grad_h.xyz, gv=grad_v.xyz;
    if (unnormalized) { gh/=size; gv/=size; }
    if (gradients) {
      if (register_gradients) return XenonTextures3D[slot].SampleGrad(XenonSamplers[slot],c,gh*scale,gv*scale);
#ifdef XENON_PIXEL_SHADER
      return XenonTextures3D[slot].SampleBias(XenonSamplers[slot],c,lod);
#endif
    }
    return XenonTextures3D[slot].SampleLevel(XenonSamplers[slot],c,lod);
  }
  // Cube: register gradients are already in cube space.
  float3 cube_coord=coord.xyz;
  if (gradients) {
    if (register_gradients) return XenonTexturesCube[slot].SampleGrad(XenonSamplers[slot],cube_coord,grad_h.xyz*scale,grad_v.xyz*scale);
#ifdef XENON_PIXEL_SHADER
    return XenonTexturesCube[slot].SampleBias(XenonSamplers[slot],cube_coord,lod);
#endif
  }
  return XenonTexturesCube[slot].SampleLevel(XenonSamplers[slot],cube_coord,lod);
}

// getTextureComputedLod: the LOD a computed-LOD fetch at these coordinates
// would use (implicit derivatives, so pixel shaders only - lowering rejects it
// elsewhere), including the fetch-constant and instruction biases.
float4 xenon_texture_computed_lod(uint fetch_constant, uint dimension, float4 coord,
                                  float instruction_lod_bias, bool unnormalized) {
  float lod=0.0;
#ifdef XENON_PIXEL_SHADER
  uint slot=fetch_constant & 31;
  if (dimension==0) {
    uint width,levels; XenonTextures1D[slot].GetDimensions(0,width,levels);
    float c=unnormalized ? coord.x / max(width,1) : coord.x;
    lod=XenonTextures1D[slot].CalculateLevelOfDetailUnclamped(XenonSamplers[slot],c);
  } else if (dimension==1) {
    uint width,height,levels; XenonTextures2D[slot].GetDimensions(0,width,height,levels);
    float2 c=unnormalized ? coord.xy / max(float2(width,height),1.0) : coord.xy;
    lod=XenonTextures2D[slot].CalculateLevelOfDetailUnclamped(XenonSamplers[slot],c);
  } else if (dimension==2) {
    uint width,height,depth,levels; XenonTextures3D[slot].GetDimensions(0,width,height,depth,levels);
    float3 c=unnormalized ? coord.xyz / max(float3(width,height,depth),1.0) : coord.xyz;
    lod=XenonTextures3D[slot].CalculateLevelOfDetailUnclamped(XenonSamplers[slot],c);
  } else {
    lod=XenonTexturesCube[slot].CalculateLevelOfDetailUnclamped(XenonSamplers[slot],coord.xyz);
  }
#endif
  lod+=xenon_texture_constant_lod_bias(fetch_constant) + instruction_lod_bias;
  return float4(lod, 0.0, 0.0, 0.0);
}

// getTextureWeights: bilinear lerp factors, frac(texel - 0.5) per axis, at
// LOD 0 (W, the mip lerp factor, is 0 - Xenia does not model it either).
// Cube maps use the same ST logic on their face coordinates.
float4 xenon_texture_weights(uint fetch_constant, uint dimension, float4 coord,
                             bool unnormalized, float3 offset) {
  uint slot=fetch_constant & 31;
  float3 size=1.0;
  if (dimension==0) {
    uint width,levels; XenonTextures1D[slot].GetDimensions(0,width,levels);
    size=float3(width,1,1);
  } else if (dimension==1) {
    uint width,height,levels; XenonTextures2D[slot].GetDimensions(0,width,height,levels);
    size=float3(width,height,1);
  } else if (dimension==2) {
    uint width,height,depth,levels; XenonTextures3D[slot].GetDimensions(0,width,height,depth,levels);
    size=float3(width,height,depth);
  } else {
    uint width,height,levels; XenonTexturesCube[slot].GetDimensions(0,width,height,levels);
    size=float3(width,height,1);
  }
  float3 texel=unnormalized ? coord.xyz : coord.xyz * size;
  float3 weights=frac(texel + offset + XENON_TEXEL_ROUNDING - 0.5);
  if (dimension==0) weights.yz=0.0;
  else if (dimension!=2) weights.z=0.0;
  return float4(weights, 0.0);
}

float4 xenon_apply_texture_exp_adjust(uint fetch_constant, float4 value) {
  int adjustment=xenon_texture_exp_adjust(fetch_constant);
  return ldexp(value, int4(adjustment,adjustment,adjustment,adjustment));
}
void xenon_write_fetch(inout float4 dest, float4 value, uint swizzle) {
  [unroll] for (uint i=0;i<4;++i) {
    uint s=(swizzle>>(i*3))&7;
    if (s<4) dest[i]=value[s]; else if (s==4) dest[i]=0.0; else if (s==5) dest[i]=1.0;
  }
}

)hlsl";
  if (memexport_enabled) {
    out << R"hlsl(

uint xenon_memexport_convert_component(float value, uint width,
                                       uint number_format) {
  if (isnan(value)) value = 0.0;
  uint mask = (1u << width) - 1u;
  bool signed_format = (number_format & 1u) != 0u;
  bool integer_format = (number_format & 2u) != 0u;
  if (signed_format) {
    float converted = value;
    if (integer_format) {
      float maximum = float((1u << (width - 1u)) - 1u);
      converted = clamp(converted, -1.0 - maximum, maximum);
    } else {
      converted = clamp(converted, -1.0, 1.0);
      if (width > 2u) converted *= float((1u << (width - 1u)) - 1u);
    }
    converted += converted < 0.0 ? -0.5 : 0.5;
    return asuint((int)converted) & mask;
  }
  float converted = value;
  if (integer_format) {
    converted = clamp(converted, 0.0, float(mask));
  } else {
    converted = saturate(converted);
    if (width > 1u) converted *= float(mask);
  }
  converted += 0.5;
  return (uint)converted & mask;
}

uint4 xenon_memexport_pack_32(float4 value, uint4 widths,
                              uint number_format) {
  uint4 converted = asuint(value);
  uint packed = 0u;
  uint offset = 0u;
  [unroll] for (uint i = 0u; i < 4u; ++i) {
    uint width = widths[i];
    if (width != 0u) {
      converted[i] = xenon_memexport_convert_component(value[i], width,
                                                        number_format);
      uint mask = (1u << width) - 1u;
      packed |= (converted[i] & mask) << offset;
      offset += width;
    }
  }
  converted.x = packed;
  return converted;
}

bool xenon_memexport_pack(float4 value, uint format, uint number_format,
                          out uint4 packed, out uint size_log2) {
  packed = asuint(value);
  size_log2 = 0xFFFFFFFFu;
  if (format == 2u || format == 8u || format == 9u) {
    packed = xenon_memexport_pack_32(value, uint4(8u, 0u, 0u, 0u), number_format);
    size_log2 = 0u;
  } else if (format == 3u) {
    packed = xenon_memexport_pack_32(value, uint4(5u, 5u, 5u, 1u), number_format);
    size_log2 = 1u;
  } else if (format == 4u) {
    packed = xenon_memexport_pack_32(value, uint4(5u, 6u, 5u, 0u), number_format);
    size_log2 = 1u;
  } else if (format == 5u) {
    packed = xenon_memexport_pack_32(value, uint4(5u, 5u, 6u, 0u), number_format);
    size_log2 = 1u;
  } else if (format == 6u || format == 14u || format == 50u) {
    packed = xenon_memexport_pack_32(value, uint4(8u, 8u, 8u, 8u), number_format);
    size_log2 = 2u;
  } else if (format == 7u || format == 54u) {
    packed = xenon_memexport_pack_32(value, uint4(10u, 10u, 10u, 2u), number_format);
    size_log2 = 2u;
  } else if (format == 10u) {
    packed = xenon_memexport_pack_32(value, uint4(8u, 8u, 0u, 0u), number_format);
    size_log2 = 1u;
  } else if (format == 15u) {
    packed = xenon_memexport_pack_32(value, uint4(4u, 4u, 4u, 4u), number_format);
    size_log2 = 1u;
  } else if (format == 16u || format == 55u) {
    packed = xenon_memexport_pack_32(value, uint4(11u, 11u, 10u, 0u), number_format);
    size_log2 = 2u;
  } else if (format == 17u || format == 56u) {
    packed = xenon_memexport_pack_32(value, uint4(10u, 11u, 11u, 0u), number_format);
    size_log2 = 2u;
  } else if (format == 24u) {
    packed = xenon_memexport_pack_32(value, uint4(16u, 0u, 0u, 0u), number_format);
    size_log2 = 1u;
  } else if (format == 25u) {
    packed = xenon_memexport_pack_32(value, uint4(16u, 16u, 0u, 0u), number_format);
    size_log2 = 2u;
  } else if (format == 26u) {
    uint4 converted = asuint(value);
    [unroll] for (uint i = 0u; i < 4u; ++i)
      converted[i] = xenon_memexport_convert_component(value[i], 16u, number_format);
    converted.xy = uint2((converted.x & 0xFFFFu) | (converted.y << 16u),
                         (converted.z & 0xFFFFu) | (converted.w << 16u));
    packed = converted;
    size_log2 = 3u;
  } else if (format == 30u) {
    packed.x = f32tof16(value.x);
    size_log2 = 1u;
  } else if (format == 31u) {
    packed.x = f32tof16(value.x) | (f32tof16(value.y) << 16u);
    packed.y = f32tof16(value.y);
    size_log2 = 2u;
  } else if (format == 32u) {
    uint4 half_values = uint4(f32tof16(value.x), f32tof16(value.y),
                              f32tof16(value.z), f32tof16(value.w));
    packed = half_values;
    packed.xy = uint2(half_values.x | (half_values.y << 16u),
                      half_values.z | (half_values.w << 16u));
    size_log2 = 3u;
  } else if (format == 36u) {
    size_log2 = 2u;
  } else if (format == 37u) {
    size_log2 = 3u;
  } else if (format == 38u) {
    size_log2 = 4u;
  }
  return size_log2 != 0xFFFFFFFFu;
}

uint xenon_memexport_swap8in16(uint value) {
  return ((value & 0x00FF00FFu) << 8u) | ((value >> 8u) & 0x00FF00FFu);
}
uint xenon_memexport_swap8in32(uint value) {
  value = xenon_memexport_swap8in16(value);
  return (value << 16u) | (value >> 16u);
}
uint xenon_memexport_swap16in32(uint value) {
  return (value << 16u) | (value >> 16u);
}
uint4 xenon_memexport_apply_endian(uint4 value, uint endian) {
  if (endian == 4u) {
    value = value.yxwz;
    endian = 2u;
  } else if (endian == 5u) {
    value = value.wzyx;
    endian = 2u;
  }
  if (endian == 1u) {
    value = uint4(xenon_memexport_swap8in16(value.x),
                  xenon_memexport_swap8in16(value.y),
                  xenon_memexport_swap8in16(value.z),
                  xenon_memexport_swap8in16(value.w));
  } else if (endian == 2u) {
    value = uint4(xenon_memexport_swap8in32(value.x),
                  xenon_memexport_swap8in32(value.y),
                  xenon_memexport_swap8in32(value.z),
                  xenon_memexport_swap8in32(value.w));
  } else if (endian == 3u) {
    value = uint4(xenon_memexport_swap16in32(value.x),
                  xenon_memexport_swap16in32(value.y),
                  xenon_memexport_swap16in32(value.z),
                  xenon_memexport_swap16in32(value.w));
  }
  return value;
}

void xenon_memexport_store(uint byte_address, uint size_log2, uint4 value) {
  if (size_log2 <= 1u) {
    uint width = 8u << size_log2;
    uint aligned_address = byte_address & ~3u;
    uint shift = (byte_address & 3u) * 8u;
    uint field_mask = ((1u << width) - 1u) << shift;
    uint ignored;
    XenonGuestMemory.InterlockedAnd(aligned_address, ~field_mask, ignored);
    XenonGuestMemory.InterlockedOr(aligned_address,
                                   (value.x << shift) & field_mask, ignored);
  } else if (size_log2 == 2u) {
    XenonGuestMemory.Store(byte_address, value.x);
  } else if (size_log2 == 3u) {
    XenonGuestMemory.Store2(byte_address, value.xy);
  } else if (size_log2 == 4u) {
    XenonGuestMemory.Store4(byte_address, value);
  }
}

void xenon_memexport_flush(float4 address_value, float4 m0, float4 m1,
                           float4 m2, float4 m3, float4 m4,
                           uint written_mask) {
  if (written_mask == 0u) return;
  uint4 address = asuint(address_value);
  if ((address.x >> 30u) != 1u || (address.y >> 23u) != 0x96u ||
      (address.z >> 23u) != 0x96u || (address.w >> 23u) != 0x96u) return;
  uint endian = address.z & 7u;
  uint number_format = (address.z >> 16u) & 7u;
  if (endian > 5u || (number_format > 3u && number_format != 7u)) return;

  uint memory_size = 0u;
  XenonGuestMemory.GetDimensions(memory_size);
  uint base_dwords = address.x & 0x3FFFFFFFu;
  if (base_dwords >= memory_size / 4u) return;
  uint base_address = base_dwords * 4u;
  uint base_index = address.y & 0x7FFFFFu;
  uint index_count = address.w & 0x7FFFFFu;
  uint format = (address.z >> 8u) & 0x3Fu;
  bool red_blue_swap = ((address.z >> 19u) & 1u) != 0u;
  float4 data[5];
  data[0] = m0; data[1] = m1; data[2] = m2; data[3] = m3; data[4] = m4;

  [unroll] for (uint i = 0u; i < 5u; ++i) {
    if ((written_mask & (1u << i)) == 0u) continue;
    uint index = base_index + i;
    if (index < base_index || index >= index_count) continue;
    float4 value = data[i];
    if (red_blue_swap) value.xz = value.zx;
    uint4 packed;
    uint size_log2;
    if (!xenon_memexport_pack(value, format, number_format, packed, size_log2))
      continue;
    packed = xenon_memexport_apply_endian(packed, endian);
    uint element_size = 1u << size_log2;
    if (base_address > memory_size || element_size > memory_size - base_address)
      continue;
    uint available_elements = (memory_size - base_address) >> size_log2;
    if (index >= available_elements) continue;
    xenon_memexport_store(base_address + (index << size_log2), size_log2,
                          packed);
  }
}
)hlsl";
  }
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
    ++result.unsupported_features;
    return result;
  }
  if (shader.reflection.temporary_register_count > options.temporary_register_limit ||
      options.temporary_register_limit > 64) {
    result.diagnostics.emplace_back("shader temporary register usage exceeds the Xenos limit");
    ++result.unsupported_features;
    return result;
  }

  static constexpr std::array<std::uint8_t, 13> supported_vertex_formats{
      6, 7, 16, 17, 25, 26, 31, 32, 33, 34, 35, 36, 37};
  for (const auto& instruction : shader.instructions) {
    if (instruction.kind == ShaderInstructionKind::Alu) {
      const auto& alu = instruction.alu;
      if (alu.vector_opcode > 29) {
        result.diagnostics.emplace_back("reserved Xenos vector ALU opcode");
        ++result.unsupported_instructions;
      }
      if (alu.scalar_opcode == 41 || alu.scalar_opcode > 50) {
        result.diagnostics.emplace_back("reserved Xenos scalar ALU opcode");
        ++result.unsupported_instructions;
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
        ++result.unsupported_fetch_formats;
      }
    } else {
      const auto& fetch = instruction.texture_fetch;
      switch (fetch.opcode) {
        case 1:   // tfetch
        case 19:  // getTextureWeights
        case 24:  // setTextureLod
        case 25:  // setTextureGradientsHorz
        case 26:  // setTextureGradientsVert
          break;
        case 17:  // getTextureComputedLod
        case 18:  // getTextureGradients
          if (shader.stage != ShaderStage::Pixel) {
            result.diagnostics.emplace_back(
                "texture LOD/gradient query needs implicit derivatives, which only "
                "exist in pixel shaders");
            ++result.unsupported_fetch_formats;
          }
          break;
        case 16:  // getTextureBorderColorFrac
          result.diagnostics.emplace_back(
              "getTextureBorderColorFrac has no lowering implementation");
          ++result.unsupported_fetch_formats;
          break;
        default:
          result.diagnostics.emplace_back("unknown Xenos texture fetch opcode");
          ++result.unsupported_fetch_formats;
          break;
      }
    }
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

  const bool emits_pixel_depth =
      shader.stage == ShaderStage::Pixel &&
      (shader.reflection.writes_depth ||
       options.pixel_depth_output != PixelDepthOutputMode::Native);
  const bool writes_color_target_zero =
      shader.stage == ShaderStage::Pixel &&
      std::find(shader.reflection.exports.begin(), shader.reflection.exports.end(),
                std::uint8_t{0}) != shader.reflection.exports.end();

  std::ostringstream out;
  out << "// Project Xenon generated HLSL; source hash 0x" << std::hex
      << shader.source_hash << std::dec << "\n";
  const bool memexport_enabled = shader.reflection.memory_exports != 0;
  const bool guest_memory_rw =
      memexport_enabled || options.force_guest_memory_rw;
  if (shader.stage == ShaderStage::Pixel) out << "#define XENON_PIXEL_SHADER 1\n";
  emit_prelude(out, guest_memory_rw, memexport_enabled);
  if (shader.stage == ShaderStage::Vertex) {
    // i16 carries the second position export (point size, edge flag, kill)
    // to a point sprite geometry shader, which turns it into the sprite
    // coordinates; the user clip distances come last.
    out << "struct XenonOutput { float4 position : SV_Position;";
    for (unsigned i=0;i<17;++i) out << " float4 i" << i << " : TEXCOORD" << i << ";";
    out << " float4 clip0 : SV_ClipDistance0; float2 clip1 : SV_ClipDistance1;";
    out << " };\nXenonOutput " << options.entry_point << "(uint vertex_id : SV_VertexID) {\n";
  } else {
    out << "struct XenonInput { float4 position : SV_Position;";
    for (unsigned i=0;i<16;++i) out << " float4 i" << i << " : TEXCOORD" << i << ";";
    // A point's PsParamGen takes its sprite coordinates from i16.
    if (options.param_gen_point) out << " float4 i16 : TEXCOORD16;";
    if (options.param_gen_interpolator < 16u && !options.param_gen_point)
      out << " bool front_face : SV_IsFrontFace;";
    if (options.force_sample_frequency)
      out << " uint sample_index : SV_SampleIndex;";
    out << " };\nstruct XenonOutput {";
    for (unsigned i=0;i<4;++i) out << " float4 c" << i << " : SV_Target" << i << ";";
    if (emits_pixel_depth) out << " float depth : SV_Depth;";
    out << " uint coverage : SV_Coverage;";
    out << " };\nXenonOutput " << options.entry_point << "(XenonInput input) {\n";
  }
  // The temporaries: an array only for a shader that indexes them (see
  // temporary()).
  struct RegisterMode {
    explicit RegisterMode(bool array) { t_register_array = array; }
    ~RegisterMode() { t_register_array = true; }
  } register_mode{uses_relative_temporaries(shader)};
  if (t_register_array) {
    out << "  float4 r[64];\n"
        << "  [unroll] for (uint i=0;i<64;++i) r[i]=0.0.xxxx;\n";
  } else {
    out << "  float4";
    for (unsigned i = 0; i < 64; ++i) out << (i ? ", r_" : " r_") << i << "=0.0.xxxx";
    out << ";\n";
  }
  out << "  float4 e[64];\n"
      << "  [unroll] for (uint i=0;i<64;++i) e[i]=0.0.xxxx;\n"
      << "  bool p0=false; int a0=0; int aL=0; float4 ps=0.0.xxxx;\n"
      << "  uint2 vfetch_base=uint2(0u, 0u);\n"
      << "  float4 xenon_grad_h=0.0.xxxx, xenon_grad_v=0.0.xxxx; float xenon_register_lod=0.0;\n"
      << "  uint pc=0, guard=0, call_depth=0, loop_depth=0; bool running=true;\n"
      << "  uint call_stack[4], loop_remaining[4]; int loop_value[4], loop_step[4];\n";
  if (memexport_enabled) out << "  uint memexport_written_mask=0u;\n";
  if (shader.stage == ShaderStage::Vertex)
    out << "  " << temporary(0, false) << ".x = (float)xenon_vertex_index(vertex_id);\n";
  else {
    for (unsigned i=0;i<16;++i) {
      if (i == options.param_gen_interpolator) continue;
      out << "  " << temporary(i, false) << " = input.i" << i << ";\n";
    }
    if (options.param_gen_interpolator < 16u) {
      // PsParamGen (SQ_PROGRAM_CNTL.PARAM_GEN): |XY| the pixel's position
      // (whole numbers, as Direct3D 9's vPos), X's sign set for a back face,
      // Y's for a point, Z's for a line; a point's ZW are its sprite
      // coordinates. XenonDrawState[4].z holds the primitive class.
      out << "  {\n"
          << "    uint xenon_primitive = XenonDrawState[4].z;\n"
          << "    float2 xenon_pixel = abs(floor(input.position.xy));\n"
          << "    float4 xenon_param = float4(xenon_pixel, 0.0, 0.0);\n";
      if (options.param_gen_point) {
        out << "    xenon_param.y = asfloat(asuint(xenon_param.y) | 0x80000000u);\n"
            << "    xenon_param.zw = saturate(input.i16.xy);\n";
      } else {
        out << "    if ((xenon_primitive & 1u) != 0u && !input.front_face)\n"
            << "      xenon_param.x = asfloat(asuint(xenon_param.x) | 0x80000000u);\n"
            << "    if ((xenon_primitive & 2u) != 0u) xenon_param.z = asfloat(0x80000000u);\n";
      }
      out << "    " << temporary(options.param_gen_interpolator, false) << " = xenon_param;\n"
          << "  }\n";
    }
  }

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
          << "        if (execute) {\n";
      for (std::uint32_t address = cf.target; address < cf.target + cf.count; ++address) {
        auto found = instructions.find(address);
        if (found == instructions.end()) {
          result.diagnostics.emplace_back(
              "control flow references an instruction absent from the decoded IR");
          continue;
        }
        if (memexport_enabled && found->second->serialize) {
          out << "          xenon_memexport_flush(e[32], e[33], e[34], e[35], e[36], "
                 "e[37], memexport_written_mask);\n"
              << "          memexport_written_mask=0u; e[32]=0.0.xxxx;\n"
              << "          [unroll] for (uint mi=33u; mi<=37u; ++mi) e[mi]=0.0.xxxx;\n";
        }
        if (found->second->kind == ShaderInstructionKind::Alu) {
          emit_alu(out, found->second->alu, shader.stage, memexport_enabled);
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
    } else if (opcode == ControlFlowOpcode::Alloc) {
      if (memexport_enabled) {
        out << "        xenon_memexport_flush(e[32], e[33], e[34], e[35], e[36], "
               "e[37], memexport_written_mask);\n"
            << "        memexport_written_mask=0u; e[32]=0.0.xxxx;\n"
            << "        [unroll] for (uint mi=33u; mi<=37u; ++mi) e[mi]=0.0.xxxx;\n";
      }
      out << "        pc=" << next << ";\n";
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
  if (memexport_enabled) {
    out << "  xenon_memexport_flush(e[32], e[33], e[34], e[35], e[36], e[37], "
           "memexport_written_mask);\n";
  }
  out << "  XenonOutput output;\n";
  if (shader.stage == ShaderStage::Vertex) {
    out << "  output.position=xenon_host_position(e[62]);\n";
    for (unsigned i=0;i<16;++i) out << "  output.i" << i << "=e[" << i << "];\n";
    out << "  output.i16=e[63];\n"
        << "  xenon_user_clip_distances(xenon_clip_position(e[62]), output.clip0, output.clip1);\n";
  } else {
    for (unsigned i=0;i<4;++i) out << "  output.c" << i << "=e[" << i << "];\n";
    if (writes_color_target_zero) {
      out << "  if ((XenonDrawState[0].x & 1u) != 0u && "
             "!xenon_compare(e[0].w, asfloat(XenonDrawState[1].x), "
             "XenonDrawState[0].y)) discard;\n"
          << "  output.coverage = (XenonDrawState[0].x & 2u) != 0u ? "
             "xenon_alpha_to_mask(e[0].w, input.position.xy) : 0xFFFFFFFFu;\n";
    } else {
      out << "  output.coverage = 0xFFFFFFFFu;\n";
    }
    if (emits_pixel_depth) {
      out << "  float xenon_depth = "
          << (shader.reflection.writes_depth ? "e[61].x" : "input.position.z")
          << ";\n";
      if (options.force_sample_frequency) {
        // A static SV_SampleIndex use makes MSAA depth conversion run at sample
        // frequency on D3D12 and Vulkan. The branch is unreachable for a valid
        // sample index, but intentionally prevents the input from being DCE'd.
        out << "  if (input.sample_index == 0xFFFFFFFFu) xenon_depth = 0.0;\n";
      }
      if (options.pixel_depth_output != PixelDepthOutputMode::Native) {
        // A D24FS8 target holds half the guest depth on the host, as its
        // clears, loads and readbacks do, and its viewport range is halved to
        // match (host_viewport_depth). Interpolated Z is doubled back to guest
        // depth, an exported depth is guest depth clamped as the viewport
        // would, and either is rounded to 20e4 before being halved again.
        out << (shader.reflection.writes_depth ? "  xenon_depth = saturate(xenon_depth);\n"
                                               : "  xenon_depth *= 2.0;\n");
        out << "  xenon_depth = xenon_quantize_float20e4(xenon_depth, "
            << (options.pixel_depth_output == PixelDepthOutputMode::Float20e4NearestEven
                    ? "true"
                    : "false")
            << ") * 0.5;\n";
      }
      out << "  output.depth=xenon_depth;\n";
    }
  }
  out << "  return output;\n}\n";
  result.hlsl = out.str();
  if (options.guest_memory_segment_count > 1u &&
      !segment_guest_memory(result.hlsl, options.guest_memory_segment_shift,
                            options.guest_memory_segment_count, guest_memory_rw,
                            result.diagnostics))
    ++result.unsupported_features;
  result.translation_hash = hash_text(result.hlsl);
  result.complete = result.diagnostics.empty();
  return result;
}

LoweredShader make_rectangle_list_geometry_shader() {
  LoweredShader result{};
  result.stage = ShaderStage::Geometry;
  result.entry_point = "main";
  result.profile = "gs_6_0";
  result.hlsl = R"hlsl(
// Project Xenon native RectangleList expansion.
struct XenonVertex {
  float4 position : SV_Position;
  float4 i0 : TEXCOORD0; float4 i1 : TEXCOORD1;
  float4 i2 : TEXCOORD2; float4 i3 : TEXCOORD3;
  float4 i4 : TEXCOORD4; float4 i5 : TEXCOORD5;
  float4 i6 : TEXCOORD6; float4 i7 : TEXCOORD7;
  float4 i8 : TEXCOORD8; float4 i9 : TEXCOORD9;
  float4 i10 : TEXCOORD10; float4 i11 : TEXCOORD11;
  float4 i12 : TEXCOORD12; float4 i13 : TEXCOORD13;
  float4 i14 : TEXCOORD14; float4 i15 : TEXCOORD15;
  float4 i16 : TEXCOORD16;
  float4 clip0 : SV_ClipDistance0; float2 clip1 : SV_ClipDistance1;
};

XenonVertex xenon_rectangle_corner(XenonVertex a, XenonVertex b,
                                    XenonVertex c) {
  XenonVertex d;
  // The Xenos rectangle rule is applied after perspective division. Keep C's
  // W and rebuild clip-space XY so the generated corner lands at A+B-C in
  // screen space. Z and interpolants use the matching affine relationship.
  float2 ndc = a.position.xy / a.position.w +
               b.position.xy / b.position.w - c.position.xy / c.position.w;
  d.position = float4(ndc * c.position.w,
                      (a.position.z / a.position.w +
                       b.position.z / b.position.w -
                       c.position.z / c.position.w) * c.position.w,
                      c.position.w);
  d.i0=a.i0+b.i0-c.i0; d.i1=a.i1+b.i1-c.i1;
  d.i2=a.i2+b.i2-c.i2; d.i3=a.i3+b.i3-c.i3;
  d.i4=a.i4+b.i4-c.i4; d.i5=a.i5+b.i5-c.i5;
  d.i6=a.i6+b.i6-c.i6; d.i7=a.i7+b.i7-c.i7;
  d.i8=a.i8+b.i8-c.i8; d.i9=a.i9+b.i9-c.i9;
  d.i10=a.i10+b.i10-c.i10; d.i11=a.i11+b.i11-c.i11;
  d.i12=a.i12+b.i12-c.i12; d.i13=a.i13+b.i13-c.i13;
  d.i14=a.i14+b.i14-c.i14; d.i15=a.i15+b.i15-c.i15;
  d.i16=a.i16+b.i16-c.i16;
  d.clip0=a.clip0+b.clip0-c.clip0; d.clip1=a.clip1+b.clip1-c.clip1;
  return d;
}

[maxvertexcount(4)]
void main(triangle XenonVertex input[3],
          inout TriangleStream<XenonVertex> stream) {
  float2 p0=input[0].position.xy/input[0].position.w;
  float2 p1=input[1].position.xy/input[1].position.w;
  float2 p2=input[2].position.xy/input[2].position.w;
  float d01=dot(p0-p1,p0-p1);
  float d12=dot(p1-p2,p1-p2);
  float d20=dot(p2-p0,p2-p0);
  XenonVertex a, b, c;
  if (d01 >= d12 && d01 >= d20) { a=input[0]; b=input[1]; c=input[2]; }
  else if (d12 >= d20) { a=input[1]; b=input[2]; c=input[0]; }
  else { a=input[2]; b=input[0]; c=input[1]; }
  // A four-vertex strip rasterizes (v0,v1,v2) and (v1,v2,v3): the shared
  // v1-v2 edge must be the A-B diagonal or one corner of the rectangle is
  // never covered. C,A,B is a cyclic rotation of the input order in every
  // case above, so the title's winding (and therefore culling) is kept.
  XenonVertex d = xenon_rectangle_corner(a,b,c);
  stream.Append(c);
  stream.Append(a);
  stream.Append(b);
  stream.Append(d);
  stream.RestartStrip();
}
)hlsl";
  result.source_hash = hash_text("xenon.rectangle-list.geometry.v3");
  result.translation_hash = hash_text(result.hlsl);
  result.complete = true;
  return result;
}

LoweredShader make_point_sprite_geometry_shader() {
  LoweredShader result{};
  result.stage = ShaderStage::Geometry;
  result.entry_point = "main";
  result.profile = "gs_6_0";
  result.hlsl = R"hlsl(
// Project Xenon native point sprite expansion. Xenos draws every point as a
// screen-aligned square: its diameter is the vertex shader's exported size
// (second position export X, in the sprite export modes) clamped to
// PA_SU_POINT_MINMAX, or PA_SU_POINT_SIZE's. The sprite coordinates run from
// 0 at the top left to 1 at the bottom right, in i16.xy for PsParamGen.
cbuffer XenonShaderConstants : register(b0) {
  float4 XenonFloatConstants[512];
  uint4 XenonBoolConstants[8];
  uint4 XenonLoopConstants[32];
  uint4 XenonVertexFetchConstants[32];
  uint4 XenonDrawState[24];
};
struct XenonVertex {
  float4 position : SV_Position;
  float4 i0 : TEXCOORD0; float4 i1 : TEXCOORD1;
  float4 i2 : TEXCOORD2; float4 i3 : TEXCOORD3;
  float4 i4 : TEXCOORD4; float4 i5 : TEXCOORD5;
  float4 i6 : TEXCOORD6; float4 i7 : TEXCOORD7;
  float4 i8 : TEXCOORD8; float4 i9 : TEXCOORD9;
  float4 i10 : TEXCOORD10; float4 i11 : TEXCOORD11;
  float4 i12 : TEXCOORD12; float4 i13 : TEXCOORD13;
  float4 i14 : TEXCOORD14; float4 i15 : TEXCOORD15;
  float4 i16 : TEXCOORD16;
  float4 clip0 : SV_ClipDistance0; float2 clip1 : SV_ClipDistance1;
};

[maxvertexcount(4)]
void main(point XenonVertex input[1], inout TriangleStream<XenonVertex> stream) {
  XenonVertex v = input[0];
  float4 sizes = asfloat(XenonDrawState[11]);
  float2 diameter = sizes.xy;
  if ((XenonDrawState[4].z & 8u) != 0u) diameter = clamp(v.i16.xx, sizes.z, sizes.w);
  // Pixels to clip space: half the diameter, times 2 / render-target size,
  // times W. NDC Y points up, so the top row is +Y.
  float2 radius = diameter * 0.5 * asfloat(XenonDrawState[3].yz) * v.position.w;
  const float2 corners[4] = {float2(-1.0, 1.0), float2(1.0, 1.0),
                             float2(-1.0, -1.0), float2(1.0, -1.0)};
  [unroll] for (uint i = 0u; i < 4u; ++i) {
    XenonVertex corner = v;
    corner.position.xy = v.position.xy + corners[i] * radius;
    corner.i16 = float4(corners[i].x * 0.5 + 0.5, 0.5 - corners[i].y * 0.5, 0.0, 0.0);
    stream.Append(corner);
  }
  stream.RestartStrip();
}
)hlsl";
  result.source_hash = hash_text("xenon.point-sprite.geometry.v1");
  result.translation_hash = hash_text(result.hlsl);
  result.complete = true;
  return result;
}

LoweredShader make_transfer_fullscreen_vertex_shader() {
  LoweredShader result{};
  result.stage = ShaderStage::Vertex;
  result.entry_point = "main";
  result.profile = "vs_6_0";
  result.hlsl = R"hlsl(
struct Output { float4 position : SV_Position; };
Output main(uint id : SV_VertexID) {
  Output o;
  o.position=float4(float2((id << 1) & 2, id & 2) * float2(2,-2) + float2(-1,1),0,1);
  return o;
}
)hlsl";
  result.source_hash = hash_text("xenon.transfer.fullscreen.v1");
  result.translation_hash = hash_text(result.hlsl);
  result.complete = true;
  return result;
}

LoweredShader make_color_sample_read_shader(MsaaSamples samples) {
  const auto count = 1u << static_cast<unsigned>(samples);
  LoweredShader result{};
  result.stage = ShaderStage::Pixel;
  result.entry_point = "main";
  result.profile = "ps_6_0";
  std::ostringstream out;
  out << "Texture2DMS<float4," << count << "> Source : register(t0);\n"
      << "cbuffer Transfer : register(b0) { uint2 origin; uint sample; uint pad; };\n"
      << "float4 main(float4 position : SV_Position) : SV_Target0 {\n"
      << "  return Source.Load(int2(position.xy) + int2(origin), sample);\n}\n";
  result.hlsl = out.str();
  result.source_hash = hash_text("xenon.transfer.sample.read.v1") ^ count;
  result.translation_hash = hash_text(result.hlsl);
  result.complete = count == 1 || count == 2 || count == 4;
  if (!result.complete) result.diagnostics.emplace_back("unsupported transfer sample count");
  return result;
}

LoweredShader make_color_sample_write_shader() {
  LoweredShader result{};
  result.stage = ShaderStage::Pixel;
  result.entry_point = "main";
  result.profile = "ps_6_0";
  result.hlsl = R"hlsl(
Texture2D<float4> Source : register(t0);
cbuffer Transfer : register(b0) { uint2 origin; uint sample; uint pad; };
float4 main(float4 position : SV_Position,
            uint host_sample : SV_SampleIndex) : SV_Target0 {
  if (host_sample != sample) discard;
  return Source.Load(int3(int2(position.xy) - int2(origin), 0));
}
)hlsl";
  result.source_hash = hash_text("xenon.transfer.sample.write.v1");
  result.translation_hash = hash_text(result.hlsl);
  result.complete = true;
  return result;
}

LoweredShader make_depth_sample_read_shader(MsaaSamples samples) {
  const auto count = 1u << static_cast<unsigned>(samples);
  LoweredShader result{};
  result.stage = ShaderStage::Pixel;
  result.entry_point = "main";
  result.profile = "ps_6_0";
  std::ostringstream out;
  if (count == 1)
    out << "Texture2D<float> Depth : register(t0);\n"
        << "Texture2D<uint4> Stencil : register(t1);\n";
  else
    out << "Texture2DMS<float," << count << "> Depth : register(t0);\n"
        << "Texture2DMS<uint4," << count << "> Stencil : register(t1);\n";
  out
      << "cbuffer Transfer : register(b0) { uint2 origin; uint sample; uint pad; };\n"
      << "struct Output { float depth : SV_Target0; uint stencil : SV_Target1; };\n"
      << "Output main(float4 position : SV_Position) { Output o;\n"
      << "  int2 p=int2(position.xy)+int2(origin);\n"
      << (count == 1
              ? "  o.depth=Depth.Load(int3(p,0)); o.stencil=Stencil.Load(int3(p,0)).y; return o; }\n"
              : "  o.depth=Depth.Load(p,sample); o.stencil=Stencil.Load(p,sample).y; return o; }\n");
  result.hlsl = out.str();
  result.source_hash = hash_text("xenon.transfer.depth.read.v1") ^ count;
  result.translation_hash = hash_text(result.hlsl);
  result.complete = count == 1 || count == 2 || count == 4;
  if (!result.complete) result.diagnostics.emplace_back("unsupported depth transfer sample count");
  return result;
}

LoweredShader make_depth_sample_write_shader() {
  LoweredShader result{};
  result.stage = ShaderStage::Pixel;
  result.entry_point = "main";
  result.profile = "ps_6_0";
  result.hlsl = R"hlsl(
Texture2D<float> Depth : register(t0);
Texture2D<uint> Stencil : register(t1);
cbuffer Transfer : register(b0) { uint2 origin; uint sample; uint pad; };
struct Output { float depth : SV_Depth; uint stencil : SV_StencilRef; };
Output main(float4 position : SV_Position,
            uint host_sample : SV_SampleIndex) {
  if (host_sample != sample) discard;
  int2 p=int2(position.xy)-int2(origin);
  Output o; o.depth=Depth.Load(int3(p,0));
  o.stencil=Stencil.Load(int3(p,0)); return o;
}
)hlsl";
  result.source_hash = hash_text("xenon.transfer.depth.write.v1");
  result.translation_hash = hash_text(result.hlsl);
  result.complete = true;
  return result;
}

LoweredShader make_depth_only_sample_write_shader() {
  LoweredShader result{};
  result.stage = ShaderStage::Pixel;
  result.entry_point = "main";
  result.profile = "ps_6_0";
  result.hlsl = R"hlsl(
Texture2D<float> Depth : register(t0);
cbuffer Transfer : register(b0) { uint2 origin; uint sample; uint pad; };
float main(float4 position : SV_Position,
           uint host_sample : SV_SampleIndex) : SV_Depth {
  if (host_sample != sample) discard;
  return Depth.Load(int3(int2(position.xy)-int2(origin),0));
}
)hlsl";
  result.source_hash = hash_text("xenon.transfer.depth.write.only.v1");
  result.translation_hash = hash_text(result.hlsl);
  result.complete = true;
  return result;
}

namespace {

// The body shared by the direct EDRAM transfer shaders: from the destination
// host sample at SV_Position, the source sample holding the same EDRAM word
// (source_x, source_y, source_host), discarding samples that hold none.
// edram_transfer_source() is its CPU reference.
constexpr std::string_view kEdramTransferMappingHlsl = R"hlsl(  const uint x = uint(position.x);
  const uint y = uint(position.y);
  const uint dst_base = transfer.destination.x, dst_pitch = transfer.destination.y;
  const uint dst_msaa = transfer.destination.z, dst_mode = transfer.destination.w;
  const uint src_base = transfer.source.x, src_pitch = transfer.source.y;
  const uint src_msaa = transfer.source.z, src_mode = transfer.source.w;
  const uint stride = transfer.extent.x, depth_flags = transfer.extent.y;
  // The destination host sample's guest sample.
  uint guest = host_sample;
  if (dst_mode == 1u) {
    guest = host_sample != 0u ? 0u : 1u;
  } else if (dst_mode == 2u) {
    if (host_sample == 0u) guest = 0u;
    else if (host_sample == 3u) guest = 1u;
    else discard;
  }
  // Its EDRAM word.
  const uint sample_x = (x << (dst_msaa == 2u ? 1u : 0u)) | (dst_msaa == 2u ? (guest & 1u) : 0u);
  const uint sample_y = (y << (dst_msaa == 0u ? 0u : 1u)) |
                        (dst_msaa == 0u ? 0u : (dst_msaa == 1u ? guest : (guest >> 1u)));
  uint in_tile_x = sample_x % 80u;
  if ((depth_flags & 1u) != 0u) in_tile_x = (in_tile_x + 40u) % 80u;
  const uint row_in_tile = sample_y % 16u;
  const uint tile = (dst_base + (sample_y / 16u) * dst_pitch + (sample_x / 80u) * stride) % 2048u;
)hlsl" R"hlsl(  // The source sample holding that word.
  const uint relative = (tile + 2048u - src_base) % 2048u;
  if ((depth_flags & 2u) != 0u) in_tile_x = (in_tile_x + 40u) % 80u;
  const uint source_sample_x = ((relative % src_pitch) / stride) * 80u + in_tile_x;
  const uint source_sample_y = (relative / src_pitch) * 16u + row_in_tile;
  const uint source_x = source_sample_x >> (src_msaa == 2u ? 1u : 0u);
  const uint source_y = source_sample_y >> (src_msaa == 0u ? 0u : 1u);
  if (source_x >= transfer.extent.z || source_y >= transfer.extent.w) discard;
  uint source_guest = 0u;
  if (src_msaa == 2u) source_guest = (source_sample_y & 1u) * 2u + (source_sample_x & 1u);
  else if (src_msaa == 1u) source_guest = source_sample_y & 1u;
  const uint source_host = src_mode == 1u ? (source_guest != 0u ? 0u : 1u)
                         : (src_mode == 2u ? (source_guest != 0u ? 3u : 0u) : source_guest);
)hlsl";

constexpr std::string_view kEdramTransferConstantsBinding = R"hlsl(#ifdef __spirv__
[[vk::push_constant]] TransferConstants transfer;
#else
ConstantBuffer<TransferConstants> transfer : register(b0);
#endif
)hlsl";

}  // namespace

std::string unorm24_depth_write_hlsl(Unorm24DepthWrite write) {
  // uint -> float rounds to nearest even, as unorm24_depth_for_write() does;
  // the power-of-two scales are exact.
  if (write == Unorm24DepthWrite::PowerOfTwo)
    return R"hlsl(float xe_unorm24_depth_out(uint code) {
  return float(code & 0xFFFFFFu) * (1.0 / 16777216.0);
}
)hlsl";
  if (write == Unorm24DepthWrite::Split)
    return R"hlsl(float xe_unorm24_depth_out(uint code) {
  code &= 0xFFFFFFu;
  if (code < 0x800000u) return float(code) * (1.0 / 16777216.0);
  return float(2u * code + 1u + (code >> 23u)) * (1.0 / 33554432.0);
}
)hlsl";
  return R"hlsl(float xe_unorm24_depth_out(uint code) {
  code &= 0xFFFFFFu;
  return float(2u * code + 1u + (code >> 23u)) * (1.0 / 33554432.0);
}
)hlsl";
}

LoweredShader make_edram_depth_transfer_shader(MsaaSamples source_host_samples,
                                               EdramDepthTransferPass pass,
                                               DepthRenderTargetFormat format,
                                               Unorm24DepthWrite unorm24_write) {
  const auto count = 1u << static_cast<unsigned>(source_host_samples);
  const bool unorm24 = format == DepthRenderTargetFormat::D24S8;
  LoweredShader result{};
  result.stage = ShaderStage::Pixel;
  result.entry_point = "main";
  result.profile = "ps_6_0";
  std::ostringstream out;
  out << "struct TransferConstants { uint4 destination; uint4 source; uint4 extent; "
         "uint4 stencil; };\n"
      << kEdramTransferConstantsBinding;
  if (count == 1) {
    out << "Texture2D<float> SourceDepth : register(t0);\n"
        << "Texture2D<uint4> SourceStencil : register(t1);\n";
  } else {
    out << "Texture2DMS<float," << count << "> SourceDepth : register(t0);\n"
        << "Texture2DMS<uint4," << count << "> SourceStencil : register(t1);\n";
  }
  if (unorm24) out << edram_word_codec_hlsl() << unorm24_depth_write_hlsl(unorm24_write);
  switch (pass) {
    case EdramDepthTransferPass::DepthAndStencil:
      out << "struct Output { float depth : SV_Depth; uint stencil : SV_StencilRef; };\n"
             "Output main(float4 position : SV_Position, uint host_sample : SV_SampleIndex) {\n";
      break;
    case EdramDepthTransferPass::Depth:
      out << "float main(float4 position : SV_Position,\n"
             "           uint host_sample : SV_SampleIndex) : SV_Depth {\n";
      break;
    case EdramDepthTransferPass::StencilBit:
      out << "void main(float4 position : SV_Position, uint host_sample : SV_SampleIndex) {\n";
      break;
  }
  out << kEdramTransferMappingHlsl;
  const std::string at = count == 1 ? "int3(int2(source_x, source_y), 0)"
                                    : "int2(source_x, source_y), source_host";
  // A D24 source samples as code / (2^24 - 1); its code (xe_pack_unorm24, as
  // resolves read it) is written back as this device stores it.
  const std::string sampled = "SourceDepth.Load(" + at + ")";
  const std::string depth =
      unorm24 ? "xe_unorm24_depth_out(xe_pack_unorm24(" + sampled + "))" : sampled;
  const std::string stencil = "SourceStencil.Load(" + at + ").y";
  switch (pass) {
    case EdramDepthTransferPass::DepthAndStencil:
      out << "  Output o; o.depth = " << depth << "; o.stencil = " << stencil
          << "; return o;\n}\n";
      break;
    case EdramDepthTransferPass::Depth:
      out << "  return " << depth << ";\n}\n";
      break;
    case EdramDepthTransferPass::StencilBit:
      out << "  if ((" << stencil << " & transfer.stencil.x) == 0u) discard;\n}\n";
      break;
  }
  result.hlsl = out.str();
  result.source_hash = hash_text("xenon.transfer.edram.depth.v2") ^ count ^
                       (std::uint64_t(pass) << 8u) ^ (std::uint64_t(unorm24) << 16u) ^
                       (std::uint64_t(unorm24_write) << 17u);
  result.translation_hash = hash_text(result.hlsl);
  result.complete = count == 1 || count == 2 || count == 4;
  if (!result.complete) result.diagnostics.emplace_back("unsupported EDRAM transfer sample count");
  return result;
}

LoweredShader make_edram_color_transfer_shader(MsaaSamples source_host_samples, bool integer) {
  const auto count = 1u << static_cast<unsigned>(source_host_samples);
  const std::string_view type = integer ? "uint4" : "float4";
  LoweredShader result{};
  result.stage = ShaderStage::Pixel;
  result.entry_point = "main";
  result.profile = "ps_6_0";
  std::ostringstream out;
  out << "struct TransferConstants { uint4 destination; uint4 source; uint4 extent; };\n"
      << kEdramTransferConstantsBinding;
  if (count == 1)
    out << "Texture2D<" << type << "> Source : register(t0);\n";
  else
    out << "Texture2DMS<" << type << "," << count << "> Source : register(t0);\n";
  out << type << R"hlsl( main(float4 position : SV_Position,
            uint host_sample : SV_SampleIndex) : SV_Target0 {
)hlsl" << kEdramTransferMappingHlsl;
  if (count == 1)
    out << "  return Source.Load(int3(int2(source_x, source_y), 0));\n}\n";
  else
    out << "  return Source.Load(int2(source_x, source_y), source_host);\n}\n";
  result.hlsl = out.str();
  result.source_hash = hash_text(integer ? "xenon.transfer.edram.color.uint.v1"
                                         : "xenon.transfer.edram.color.v1") ^
                       count;
  result.translation_hash = hash_text(result.hlsl);
  result.complete = count == 1 || count == 2 || count == 4;
  if (!result.complete) result.diagnostics.emplace_back("unsupported EDRAM transfer sample count");
  return result;
}

namespace {

// HLSL ports of the CPU conversions between an EDRAM word and a host value
// (pack_color_sample / unpack_color_sample for the formats whose host storage
// is the word's own encoding, host_to_depth_stencil / depth_stencil_to_host),
// for the formats edram_converting_transfer_supported() accepts.
constexpr std::string_view kEdramWordCodecHlsl = R"hlsl(uint xe_unorm_bits(float v, float scale) {
  return uint(floor(saturate(v) * scale + 0.5));
}
uint xe_pack_rgba8(float4 v) {
  return xe_unorm_bits(v.x, 255.0) | (xe_unorm_bits(v.y, 255.0) << 8u) |
         (xe_unorm_bits(v.z, 255.0) << 16u) | (xe_unorm_bits(v.w, 255.0) << 24u);
}
float4 xe_unpack_rgba8(uint w) {
  return float4(uint4(w, w >> 8u, w >> 16u, w >> 24u) & 0xFFu) / 255.0;
}
uint xe_pack_rgb10a2(float4 v) {
  return xe_unorm_bits(v.x, 1023.0) | (xe_unorm_bits(v.y, 1023.0) << 10u) |
         (xe_unorm_bits(v.z, 1023.0) << 20u) | (xe_unorm_bits(v.w, 3.0) << 30u);
}
float4 xe_unpack_rgb10a2(uint w) {
  return float4(float3(uint3(w, w >> 10u, w >> 20u) & 0x3FFu) / 1023.0, float(w >> 30u) / 3.0);
}
// 16_16_FLOAT through the image's R16G16_UINT view: the halves are the word.
uint xe_pack_rg16(uint4 v) { return (v.x & 0xFFFFu) | (v.y << 16u); }
uint4 xe_unpack_rg16(uint w) { return uint4(w & 0xFFFFu, w >> 16u, 0u, 0u); }
uint xe_pack_r32f(float4 v) { return asuint(v.x); }
float4 xe_unpack_r32f(uint w) { return float4(asfloat(w), 0.0, 0.0, 1.0); }
// nearbyint(clamp(d, 0, 1) * 16777215) without doubles: x = i + (f - d) with
// a = d * 2^24 = i + f exact, rounding half to even.
uint xe_pack_unorm24(float d) {
  if (!(d > 0.0)) return 0u;
  d = min(d, 1.0);
  const float a = d * 16777216.0;
  const float i = floor(a);
  const float g = (a - i) - d;
  const uint base = uint(i);
  int r = 0;
  if (g > 0.5 || (g == 0.5 && (base & 1u) != 0u)) r = 1;
  else if (g < -0.5 || (g == -0.5 && (base & 1u) != 0u)) r = -1;
  return min(uint(int(base) + r), 0xFFFFFFu);
}
float xe_unpack_unorm24(uint bits) { return float(bits + (bits >> 23u)) * (1.0 / 16777216.0); }
uint xe_pack_float20e4(float v) {
  if (!(v > 0.0)) return 0u;
  uint bits = asuint(v);
  if (bits >= 0x3FFFFFF8u) return 0xFFFFFFu;
  if (bits < 0x38800000u) {
    const uint shift = min(113u - (bits >> 23u), 24u);
    bits = (0x00800000u | (bits & 0x007FFFFFu)) >> shift;
  } else {
    bits += 0xC8000000u;
  }
  bits += 3u + ((bits >> 3u) & 1u);
  return (bits >> 3u) & 0xFFFFFFu;
}
float xe_unpack_float20e4(uint value) {
  value &= 0xFFFFFFu;
  if (value == 0u) return 0.0;
  uint mantissa = value & 0xFFFFFu;
  int exponent = int(value >> 20u);
  if (exponent == 0) {
    const uint shift = (31u - firstbithigh(mantissa)) - 11u;
    exponent = 1 - int(shift);
    mantissa = (mantissa << shift) & 0xFFFFFu;
  }
  return asfloat((uint(exponent + 112) << 23u) | (mantissa << 3u));
}
)hlsl";

// The packing and unpacking functions for a color format the converting
// transfer accepts.
std::pair<std::string_view, std::string_view> color_word_codec(ColorRenderTargetFormat format) {
  switch (storage_color_format(format)) {
    case ColorRenderTargetFormat::R8G8B8A8: return {"xe_pack_rgba8", "xe_unpack_rgba8"};
    case ColorRenderTargetFormat::R10G10B10A2: return {"xe_pack_rgb10a2", "xe_unpack_rgb10a2"};
    case ColorRenderTargetFormat::R16G16Float: return {"xe_pack_rg16", "xe_unpack_rg16"};
    case ColorRenderTargetFormat::R32Float: return {"xe_pack_r32f", "xe_unpack_r32f"};
    default: return {};
  }
}

// Declares the converting transfer's source and computes `word`, the EDRAM
// word the selected source sample holds, after the shared mapping.
bool write_converting_source(std::ostringstream& out, MsaaSamples source_host_samples,
                             EdramTransferEndpoint source, bool declarations) {
  const auto count = 1u << static_cast<unsigned>(source_host_samples);
  const std::string at = count == 1 ? "int3(int2(source_x, source_y), 0)"
                                    : "int2(source_x, source_y), source_host";
  if (source.depth) {
    if (declarations) {
      if (count == 1)
        out << "Texture2D<float> SourceDepth : register(t0);\n"
            << "Texture2D<uint4> SourceStencil : register(t1);\n";
      else
        out << "Texture2DMS<float," << count << "> SourceDepth : register(t0);\n"
            << "Texture2DMS<uint4," << count << "> SourceStencil : register(t1);\n";
      return true;
    }
    const auto format = static_cast<DepthRenderTargetFormat>(source.format);
    const std::string depth = "SourceDepth.Load(" + at + ")";
    out << "  const uint depth_bits = "
        << (format == DepthRenderTargetFormat::D24FS8
                ? "xe_pack_float20e4(" + depth + " * 2.0)"
                : "xe_pack_unorm24(" + depth + ")")
        << ";\n  const uint word = (depth_bits << 8u) | (SourceStencil.Load(" << at
        << ").y & 0xFFu);\n";
    return format == DepthRenderTargetFormat::D24S8 || format == DepthRenderTargetFormat::D24FS8;
  }
  if (declarations) {
    const std::string_view type =
        color_host_transfers_as_integer(static_cast<ColorRenderTargetFormat>(source.format))
            ? "uint4"
            : "float4";
    if (count == 1)
      out << "Texture2D<" << type << "> Source : register(t0);\n";
    else
      out << "Texture2DMS<" << type << "," << count << "> Source : register(t0);\n";
    return true;
  }
  const auto pack = color_word_codec(static_cast<ColorRenderTargetFormat>(source.format)).first;
  if (pack.empty()) return false;
  out << "  const uint word = " << pack << "(Source.Load(" << at << "));\n";
  return true;
}

std::uint64_t converting_transfer_hash(MsaaSamples source_host_samples,
                                       EdramTransferEndpoint source,
                                       EdramTransferEndpoint destination,
                                       std::uint32_t pass) {
  return (std::uint64_t(1u) << static_cast<unsigned>(source_host_samples)) |
         (std::uint64_t(source.depth) << 8u) | (std::uint64_t(source.format) << 16u) |
         (std::uint64_t(destination.depth) << 24u) | (std::uint64_t(destination.format) << 32u) |
         (std::uint64_t(pass) << 40u);
}

}  // namespace

std::string_view edram_word_codec_hlsl() noexcept { return kEdramWordCodecHlsl; }

LoweredShader make_edram_converting_color_transfer_shader(MsaaSamples source_host_samples,
                                                          EdramTransferEndpoint source,
                                                          ColorRenderTargetFormat destination) {
  const auto count = 1u << static_cast<unsigned>(source_host_samples);
  LoweredShader result{};
  result.stage = ShaderStage::Pixel;
  result.entry_point = "main";
  result.profile = "ps_6_0";
  const auto unpack = color_word_codec(destination).second;
  std::ostringstream out;
  out << "struct TransferConstants { uint4 destination; uint4 source; uint4 extent; };\n"
      << kEdramTransferConstantsBinding;
  write_converting_source(out, source_host_samples, source, true);
  out << kEdramWordCodecHlsl << (color_host_transfers_as_integer(destination) ? "uint4" : "float4")
      << " main(float4 position : SV_Position, uint host_sample : SV_SampleIndex)"
         " : SV_Target0 {\n"
      << kEdramTransferMappingHlsl;
  const bool source_ok = write_converting_source(out, source_host_samples, source, false);
  out << "  return " << unpack << "(word);\n}\n";
  result.hlsl = out.str();
  const EdramTransferEndpoint destination_endpoint{false, static_cast<std::uint8_t>(destination)};
  result.source_hash = hash_text("xenon.transfer.edram.converting.color.v2") ^
                       converting_transfer_hash(source_host_samples, source,
                                                destination_endpoint, 0u);
  result.translation_hash = hash_text(result.hlsl);
  result.complete = (count == 1 || count == 2 || count == 4) && source_ok && !unpack.empty() &&
                    edram_converting_transfer_supported(source, destination_endpoint);
  if (!result.complete)
    result.diagnostics.emplace_back("unsupported converting EDRAM color transfer");
  return result;
}

LoweredShader make_edram_converting_depth_transfer_shader(MsaaSamples source_host_samples,
                                                          EdramTransferEndpoint source,
                                                          DepthRenderTargetFormat destination,
                                                          EdramDepthTransferPass pass,
                                                          Unorm24DepthWrite unorm24_write) {
  const auto count = 1u << static_cast<unsigned>(source_host_samples);
  LoweredShader result{};
  result.stage = ShaderStage::Pixel;
  result.entry_point = "main";
  result.profile = "ps_6_0";
  std::ostringstream out;
  out << "struct TransferConstants { uint4 destination; uint4 source; uint4 extent; "
         "uint4 stencil; };\n"
      << kEdramTransferConstantsBinding;
  write_converting_source(out, source_host_samples, source, true);
  out << kEdramWordCodecHlsl << unorm24_depth_write_hlsl(unorm24_write);
  switch (pass) {
    case EdramDepthTransferPass::DepthAndStencil:
      out << "struct Output { float depth : SV_Depth; uint stencil : SV_StencilRef; };\n"
             "Output main(float4 position : SV_Position, uint host_sample : SV_SampleIndex) {\n";
      break;
    case EdramDepthTransferPass::Depth:
      out << "float main(float4 position : SV_Position,\n"
             "           uint host_sample : SV_SampleIndex) : SV_Depth {\n";
      break;
    case EdramDepthTransferPass::StencilBit:
      out << "void main(float4 position : SV_Position, uint host_sample : SV_SampleIndex) {\n";
      break;
  }
  out << kEdramTransferMappingHlsl;
  const bool source_ok = write_converting_source(out, source_host_samples, source, false);
  // The host stores D24FS8 depth at half its guest value (depth_stencil_to_host).
  const std::string depth = destination == DepthRenderTargetFormat::D24FS8
                                ? "xe_unpack_float20e4(word >> 8u) * 0.5"
                                : "xe_unorm24_depth_out(word >> 8u)";
  switch (pass) {
    case EdramDepthTransferPass::DepthAndStencil:
      out << "  Output o; o.depth = " << depth << "; o.stencil = word & 0xFFu; return o;\n}\n";
      break;
    case EdramDepthTransferPass::Depth:
      out << "  return " << depth << ";\n}\n";
      break;
    case EdramDepthTransferPass::StencilBit:
      out << "  if ((word & transfer.stencil.x) == 0u) discard;\n}\n";
      break;
  }
  result.hlsl = out.str();
  const EdramTransferEndpoint destination_endpoint{true, static_cast<std::uint8_t>(destination)};
  result.source_hash = hash_text("xenon.transfer.edram.converting.depth.v3") ^
                       converting_transfer_hash(source_host_samples, source,
                                                destination_endpoint, std::uint32_t(pass)) ^
                       (std::uint64_t(unorm24_write) << 60u);
  result.translation_hash = hash_text(result.hlsl);
  result.complete = (count == 1 || count == 2 || count == 4) && source_ok &&
                    edram_converting_transfer_supported(source, destination_endpoint);
  if (!result.complete)
    result.diagnostics.emplace_back("unsupported converting EDRAM depth transfer");
  return result;
}

namespace {

// The canonical load's source: the EDRAM image as a raw buffer, and `word`,
// the word of the destination sample (the mapping's EDRAM half - tile, row
// and in-tile column - with the byte layout edram_sample_address() gives).
constexpr std::string_view kCanonicalEdramSourceHlsl =
    "ByteAddressBuffer CanonicalEdram : register(t0);\n";
constexpr std::string_view kCanonicalEdramWordHlsl =
    "  const uint word = CanonicalEdram.Load(tile * 5120u + (row_in_tile * 80u + in_tile_x) * 4u);\n";

std::string_view edram_destination_word_hlsl() {
  // kEdramTransferMappingHlsl up to the destination's EDRAM word.
  constexpr std::string_view marker = "  // The source sample holding that word.";
  return kEdramTransferMappingHlsl.substr(0, kEdramTransferMappingHlsl.find(marker));
}

}  // namespace

LoweredShader make_edram_canonical_color_load_shader(ColorRenderTargetFormat destination) {
  LoweredShader result{};
  result.stage = ShaderStage::Pixel;
  result.entry_point = "main";
  result.profile = "ps_6_0";
  const auto unpack = color_word_codec(destination).second;
  std::ostringstream out;
  out << "struct TransferConstants { uint4 destination; uint4 source; uint4 extent; };\n"
      << kEdramTransferConstantsBinding << kCanonicalEdramSourceHlsl << kEdramWordCodecHlsl
      << (color_host_transfers_as_integer(destination) ? "uint4" : "float4")
      << " main(float4 position : SV_Position, uint host_sample : SV_SampleIndex)"
         " : SV_Target0 {\n"
      << edram_destination_word_hlsl() << kCanonicalEdramWordHlsl << "  return " << unpack
      << "(word);\n}\n";
  result.hlsl = out.str();
  const EdramTransferEndpoint endpoint{false, static_cast<std::uint8_t>(destination)};
  result.source_hash = hash_text("xenon.transfer.edram.canonical.color.v1") ^
                       (std::uint64_t(destination) << 32u);
  result.translation_hash = hash_text(result.hlsl);
  result.complete = !unpack.empty() && edram_canonical_load_supported(endpoint);
  if (!result.complete) result.diagnostics.emplace_back("unsupported canonical EDRAM color load");
  return result;
}

LoweredShader make_edram_canonical_depth_load_shader(DepthRenderTargetFormat destination,
                                                     EdramDepthTransferPass pass,
                                                     Unorm24DepthWrite unorm24_write) {
  LoweredShader result{};
  result.stage = ShaderStage::Pixel;
  result.entry_point = "main";
  result.profile = "ps_6_0";
  std::ostringstream out;
  out << "struct TransferConstants { uint4 destination; uint4 source; uint4 extent; "
         "uint4 stencil; };\n"
      << kEdramTransferConstantsBinding << kCanonicalEdramSourceHlsl << kEdramWordCodecHlsl
      << unorm24_depth_write_hlsl(unorm24_write);
  switch (pass) {
    case EdramDepthTransferPass::DepthAndStencil:
      out << "struct Output { float depth : SV_Depth; uint stencil : SV_StencilRef; };\n"
             "Output main(float4 position : SV_Position, uint host_sample : SV_SampleIndex) {\n";
      break;
    case EdramDepthTransferPass::Depth:
      out << "float main(float4 position : SV_Position,\n"
             "           uint host_sample : SV_SampleIndex) : SV_Depth {\n";
      break;
    case EdramDepthTransferPass::StencilBit:
      out << "void main(float4 position : SV_Position, uint host_sample : SV_SampleIndex) {\n";
      break;
  }
  out << edram_destination_word_hlsl() << kCanonicalEdramWordHlsl;
  // As the converting transfer writes a word: D24FS8 at half its guest value
  // (depth_stencil_to_host), D24S8 as this device stores the code.
  const std::string depth = destination == DepthRenderTargetFormat::D24FS8
                                ? "xe_unpack_float20e4(word >> 8u) * 0.5"
                                : "xe_unorm24_depth_out(word >> 8u)";
  switch (pass) {
    case EdramDepthTransferPass::DepthAndStencil:
      out << "  Output o; o.depth = " << depth << "; o.stencil = word & 0xFFu; return o;\n}\n";
      break;
    case EdramDepthTransferPass::Depth:
      out << "  return " << depth << ";\n}\n";
      break;
    case EdramDepthTransferPass::StencilBit:
      out << "  if ((word & transfer.stencil.x) == 0u) discard;\n}\n";
      break;
  }
  result.hlsl = out.str();
  const EdramTransferEndpoint endpoint{true, static_cast<std::uint8_t>(destination)};
  result.source_hash = hash_text("xenon.transfer.edram.canonical.depth.v1") ^
                       (std::uint64_t(destination) << 32u) ^ (std::uint64_t(pass) << 40u) ^
                       (std::uint64_t(unorm24_write) << 48u);
  result.translation_hash = hash_text(result.hlsl);
  result.complete = edram_canonical_load_supported(endpoint);
  if (!result.complete) result.diagnostics.emplace_back("unsupported canonical EDRAM depth load");
  return result;
}

LoweredShader make_stencil_mask_write_shader() {
  LoweredShader result{};
  result.stage = ShaderStage::Pixel;
  result.entry_point = "main";
  result.profile = "ps_6_0";
  result.hlsl = R"hlsl(
Texture2D<uint> Stencil : register(t1);
cbuffer Transfer : register(b0) { uint2 origin; uint sample; uint value; };
void main(float4 position : SV_Position,
          uint host_sample : SV_SampleIndex) {
  if (host_sample != sample) discard;
  if (Stencil.Load(int3(int2(position.xy)-int2(origin),0)) != value) discard;
}
)hlsl";
  result.source_hash = hash_text("xenon.transfer.stencil.mask.write.v1");
  result.translation_hash = hash_text(result.hlsl);
  result.complete = true;
  return result;
}

}  // namespace xenon::gpu

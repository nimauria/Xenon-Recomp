#include "xenon/gpu/vertex_position_evaluator.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>

#include "xenon/gpu/resource_ir.hpp"

namespace xenon::gpu {
namespace {

// The constant buffer's sections (ResourceStateTracker::write_constant_buffer
// and the HLSL cbuffer XenonShaderConstants).
constexpr std::size_t kBoolConstantsOffset = 8192;
constexpr std::size_t kLoopConstantsOffset = 8320;
constexpr std::size_t kVertexFetchOffset = 8832;
constexpr float kFltMax = 3.402823466e+38f;

using Float4 = std::array<float, 4>;

[[nodiscard]] std::uint32_t word_at(std::span<const std::byte> constants, std::size_t offset) noexcept {
  std::uint32_t value{};
  if (offset + 4u <= constants.size()) std::memcpy(&value, constants.data() + offset, 4u);
  return value;
}

[[nodiscard]] float half_to_float(std::uint32_t half) noexcept {
  const std::uint32_t sign = (half >> 15u) & 1u, exponent = (half >> 10u) & 31u,
                      mantissa = half & 1023u;
  float value;
  if (exponent == 0u) value = std::ldexp(static_cast<float>(mantissa), -24);
  else if (exponent == 31u) value = mantissa ? std::numeric_limits<float>::quiet_NaN()
                                             : std::numeric_limits<float>::infinity();
  else value = std::ldexp(static_cast<float>(mantissa | 1024u), static_cast<int>(exponent) - 25);
  return sign ? -value : value;
}

// HLSL float -> uint: negative and NaN to 0, saturating above.
[[nodiscard]] std::uint32_t to_uint(float value) noexcept {
  if (!(value > 0.0f)) return 0u;
  if (value >= 4294967296.0f) return 0xFFFFFFFFu;
  return static_cast<std::uint32_t>(value);
}

[[nodiscard]] float legacy_mul(float a, float b) noexcept {
  return (a == 0.0f || b == 0.0f) ? 0.0f : a * b;
}

[[nodiscard]] float clamp_infinity(float value, bool to_zero) noexcept {
  if (!std::isinf(value)) return value;
  if (to_zero) return value < 0.0f ? -0.0f : 0.0f;
  return value < 0.0f ? -kFltMax : kFltMax;
}

[[nodiscard]] Float4 splat(float v) noexcept { return {v, v, v, v}; }

[[nodiscard]] float saturate(float v) noexcept {
  // HLSL saturate maps NaN to 0.
  return std::isnan(v) ? 0.0f : std::clamp(v, 0.0f, 1.0f);
}

std::uint32_t endian_swap(std::uint32_t value, std::uint32_t endian) noexcept {
  if (endian == 1u || endian == 2u)
    value = ((value & 0x00FF00FFu) << 8u) | ((value >> 8u) & 0x00FF00FFu);
  if (endian == 2u || endian == 3u) value = (value << 16u) | (value >> 16u);
  return value;
}

struct Machine {
  const DecodedShader& shader;
  std::span<const std::byte> constants;
  const GuestPhysicalReader& memory;

  std::array<Float4, 64> r{};
  std::array<Float4, 64> e{};
  bool p0{false};
  std::int32_t a0{0};
  std::int32_t aL{0};
  Float4 ps{};
  std::uint32_t vfetch_address{};
  std::uint32_t vfetch_endian{};
  std::string error{};

  [[nodiscard]] Float4 float_constant(std::uint32_t index) const noexcept {
    Float4 value{};
    std::memcpy(value.data(), constants.data() + std::size_t(index & 255u) * 16u, 16u);
    return value;
  }
  [[nodiscard]] bool bool_constant(std::uint32_t index) const noexcept {
    const auto word = word_at(constants, kBoolConstantsOffset +
                                             ((index >> 7u) & 7u) * 16u + ((index >> 5u) & 3u) * 4u);
    return ((word >> (index & 31u)) & 1u) != 0u;
  }
  [[nodiscard]] std::uint32_t draw_state(std::size_t word) const noexcept {
    return word_at(constants, kDrawStateOffset + word * 4u);
  }

  [[nodiscard]] Float4 source(const AluSource& value, bool a0_constants) const noexcept {
    Float4 raw;
    if (value.temporary) {
      const auto index = (value.index + (value.relative ? static_cast<std::uint32_t>(aL) : 0u)) & 63u;
      raw = r[index];
    } else {
      const auto offset = value.relative ? static_cast<std::uint32_t>(a0_constants ? a0 : aL) : 0u;
      raw = float_constant((value.index + offset) & 255u);
    }
    Float4 out;
    for (unsigned i = 0; i < 4; ++i) {
      float v = raw[(value.swizzle >> (i * 2u)) & 3u];
      if (value.absolute_value) v = std::fabs(v);
      if (value.negate) v = -v;
      out[i] = v;
    }
    return out;
  }

  // shader_translation.cpp vector_operation().
  [[nodiscard]] Float4 vector_op(std::uint32_t op, const Float4& a, const Float4& b,
                                 const Float4& c) noexcept {
    Float4 out{};
    const auto each = [&](auto fn) {
      for (unsigned i = 0; i < 4; ++i) out[i] = fn(a[i], b[i], c[i]);
      return out;
    };
    switch (op) {
      case 0: return each([](float x, float y, float) { return x + y; });
      case 1: return each([](float x, float y, float) { return legacy_mul(x, y); });
      case 2: return each([](float x, float y, float) { return std::fmax(x, y); });
      case 3: return each([](float x, float y, float) { return std::fmin(x, y); });
      case 4: return each([](float x, float y, float) { return x == y ? 1.0f : 0.0f; });
      case 5: return each([](float x, float y, float) { return x > y ? 1.0f : 0.0f; });
      case 6: return each([](float x, float y, float) { return x >= y ? 1.0f : 0.0f; });
      case 7: return each([](float x, float y, float) { return x != y ? 1.0f : 0.0f; });
      case 8: return each([](float x, float, float) { return x - std::floor(x); });
      case 9: return each([](float x, float, float) { return std::trunc(x); });
      case 10: return each([](float x, float, float) { return std::floor(x); });
      case 11: return each([](float x, float y, float z) { return legacy_mul(x, y) + z; });
      case 12: return each([](float x, float y, float z) { return x == 0.0f ? y : z; });
      case 13: return each([](float x, float y, float z) { return x >= 0.0f ? y : z; });
      case 14: return each([](float x, float y, float z) { return x > 0.0f ? y : z; });
      // Each product of a dot product follows the 0 * anything = 0 rule.
      case 15:
        return splat(((legacy_mul(a[0], b[0]) + legacy_mul(a[1], b[1])) + legacy_mul(a[2], b[2])) +
                     legacy_mul(a[3], b[3]));
      case 16:
        return splat((legacy_mul(a[0], b[0]) + legacy_mul(a[1], b[1])) + legacy_mul(a[2], b[2]));
      case 17: return splat((legacy_mul(a[0], b[0]) + legacy_mul(a[1], b[1])) + c[0]);
      case 18: {
        const float x = a[2], y = a[3], z = a[0];
        const float qx = std::fabs(x), qy = std::fabs(y), qz = std::fabs(z);
        if (qz >= qx && qz >= qy)
          return {-y, z < 0.0f ? -x : x, 2.0f * z, z < 0.0f ? 5.0f : 4.0f};
        if (qy >= qx) return {y < 0.0f ? -z : z, x, 2.0f * y, y < 0.0f ? 3.0f : 2.0f};
        return {-y, x < 0.0f ? z : -z, 2.0f * x, x < 0.0f ? 1.0f : 0.0f};
      }
      case 19: return splat(std::fmax(std::fmax(a[0], a[1]), std::fmax(a[2], a[3])));
      case 20: case 21: case 22: case 23: {
        p0 = op == 20 ? (a[3] == 0.0f && b[3] == 0.0f)
           : op == 21 ? (a[3] == 0.0f && b[3] != 0.0f)
           : op == 22 ? (a[3] == 0.0f && b[3] > 0.0f)
                      : (a[3] == 0.0f && b[3] >= 0.0f);
        const bool b_test = op == 20 ? b[0] == 0.0f : op == 21 ? b[0] != 0.0f
                          : op == 22 ? b[0] > 0.0f : b[0] >= 0.0f;
        return (b_test && a[0] == 0.0f) ? splat(0.0f) : splat(a[0] + 1.0f);
      }
      case 24: case 25: case 26: case 27: {
        // Kills only matter to pixel shaders; the result is still written.
        bool killed = false;
        for (unsigned i = 0; i < 4; ++i)
          killed |= op == 24 ? a[i] == b[i] : op == 25 ? a[i] > b[i]
                  : op == 26 ? a[i] >= b[i] : a[i] != b[i];
        return splat(killed ? 1.0f : 0.0f);
      }
      case 28: return {1.0f, legacy_mul(a[1], b[1]), a[2], b[3]};
      case 29: {
        a0 = static_cast<std::int32_t>(std::clamp(std::floor(a[3] + 0.5f), -256.0f, 255.0f));
        return each([](float x, float y, float) { return std::fmax(x, y); });
      }
      default: return splat(0.0f);
    }
  }

  // shader_translation.cpp scalar_operation(): `a` is source 3, `b` the mulsc/addsc/subsc temporary.
  [[nodiscard]] Float4 scalar_op(std::uint32_t op, const Float4& a, const Float4& b,
                                 const Float4& previous) noexcept {
    const float x = a[3], y = a[0], p = previous[0];
    float v = 0.0f;
    switch (op) {
      case 0: v = x + y; break;
      case 1: v = x + p; break;
      case 2: v = legacy_mul(x, y); break;
      case 3: v = legacy_mul(x, p); break;
      case 4:
        v = (p == -kFltMax || !std::isfinite(p) || !std::isfinite(y) || y <= 0.0f) ? -kFltMax
                                                                                    : legacy_mul(x, p);
        break;
      case 5: v = x >= y ? x : y; break;
      case 6: v = x < y ? x : y; break;
      case 7: v = x == 0.0f ? 1.0f : 0.0f; break;
      case 8: v = x > 0.0f ? 1.0f : 0.0f; break;
      case 9: v = x >= 0.0f ? 1.0f : 0.0f; break;
      case 10: v = x != 0.0f ? 1.0f : 0.0f; break;
      case 11: v = x - std::floor(x); break;
      case 12: v = std::trunc(x); break;
      case 13: v = std::floor(x); break;
      case 14: v = std::exp2(x); break;
      case 15: case 16:
        v = x == 1.0f ? 0.0f : std::log2(x);
        if (op == 15 && std::isinf(v) && v < 0.0f) v = -kFltMax;
        break;
      case 17: case 18: case 19:
        v = x == 1.0f ? 1.0f : 1.0f / x;
        if (op != 19) v = clamp_infinity(v, op == 18);
        break;
      case 20: case 21: case 22:
        v = x == 1.0f ? 1.0f : 1.0f / std::sqrt(x);
        if (op != 22) v = clamp_infinity(v, op == 21);
        break;
      case 23: case 24:
        a0 = static_cast<std::int32_t>(
            std::clamp(std::floor(x + (op == 23 ? 0.5f : 0.0f)), -256.0f, 255.0f));
        v = x >= y ? x : y;
        break;
      case 25: v = x - y; break;
      case 26: v = x - p; break;
      case 27: case 28: case 29: case 30:
        p0 = op == 27 ? x == 0.0f : op == 28 ? x != 0.0f : op == 29 ? x > 0.0f : x >= 0.0f;
        v = p0 ? 0.0f : 1.0f;
        break;
      case 31: p0 = x == 1.0f; v = p0 ? 0.0f : (x == 0.0f ? 1.0f : x); break;
      case 32: p0 = x - 1.0f <= 0.0f; v = p0 ? 0.0f : x - 1.0f; break;
      case 33: p0 = false; v = kFltMax; break;
      case 34: p0 = x == 0.0f; v = p0 ? 0.0f : x; break;
      case 35: v = x == 0.0f ? 1.0f : 0.0f; break;
      case 36: v = x > 0.0f ? 1.0f : 0.0f; break;
      case 37: v = x >= 0.0f ? 1.0f : 0.0f; break;
      case 38: v = x != 0.0f ? 1.0f : 0.0f; break;
      case 39: v = x == 1.0f ? 1.0f : 0.0f; break;
      case 40: v = std::sqrt(x); break;
      case 42: case 43: v = legacy_mul(x, b[0]); break;
      case 44: case 45: v = x + b[0]; break;
      case 46: case 47: v = x - b[0]; break;
      case 48: v = std::sin(x); break;
      case 49: v = std::cos(x); break;
      case 50: return previous;
      default: break;
    }
    return splat(v);
  }

  [[nodiscard]] Float4& destination(const AluInstruction& alu, bool scalar) noexcept {
    if (alu.export_data) return e[alu.vector_destination & 63u];
    const auto index = scalar ? alu.scalar_destination : alu.vector_destination;
    const bool relative = scalar ? alu.scalar_destination_relative : alu.vector_destination_relative;
    return r[(index + (relative ? static_cast<std::uint32_t>(aL) : 0u)) & 63u];
  }

  [[nodiscard]] bool predicate(const Predicate& value) const noexcept {
    return !value.enabled || (value.condition ? p0 : !p0);
  }

  void alu(const AluInstruction& alu) noexcept {
    if (!predicate(alu.predicate)) return;
    const bool a0_constants = alu.constant_address_register_relative;
    const auto s0 = source(alu.sources[0], a0_constants);
    const auto s1 = source(alu.sources[1], a0_constants);
    const auto s2 = source(alu.sources[2], a0_constants);
    Float4 scalar_b = s2;
    if (alu.scalar_opcode >= 42u && alu.scalar_opcode <= 47u) {
      AluSource temporary = alu.sources[2];
      temporary.index = alu.scalar_constant_temporary;
      temporary.temporary = true;
      temporary.relative = false;
      scalar_b = source(temporary, a0_constants);
    }
    auto pv = vector_op(alu.vector_opcode, s0, s1, s2);
    auto ps_new = scalar_op(alu.scalar_opcode, s2, scalar_b, ps);
    if (alu.vector_clamp)
      for (auto& v : pv) v = saturate(v);
    if (alu.scalar_clamp)
      for (auto& v : ps_new) v = saturate(v);
    std::uint8_t vector_mask = alu.vector_write_mask, scalar_mask = alu.scalar_write_mask;
    std::uint8_t one_mask = 0, zero_mask = 0;
    if (alu.export_data) {
      one_mask = vector_mask & scalar_mask;
      if (alu.scalar_destination_relative) zero_mask = 0xFu & ~(vector_mask | scalar_mask);
      vector_mask &= static_cast<std::uint8_t>(~one_mask);
      scalar_mask &= static_cast<std::uint8_t>(~one_mask);
    }
    // In the order the HLSL writes them: vector, scalar, ones, zeros.
    auto& vector_target = destination(alu, false);
    for (unsigned i = 0; i < 4; ++i)
      if (vector_mask & (1u << i)) vector_target[i] = pv[i];
    auto& scalar_target = destination(alu, true);
    for (unsigned i = 0; i < 4; ++i)
      if (scalar_mask & (1u << i)) scalar_target[i] = ps_new[i];
    for (unsigned i = 0; i < 4; ++i) {
      if (one_mask & (1u << i)) vector_target[i] = 1.0f;
      if (zero_mask & (1u << i)) vector_target[i] = 0.0f;
    }
    ps = ps_new;
  }

  // xenon_fetch_index.
  [[nodiscard]] static std::uint32_t fetch_index(float index, bool rounded) noexcept {
    if (rounded) return to_uint(std::floor(index + 0.5f));
    return to_uint(std::floor(
        index + std::fmin(std::fmax(2.5e-4f, std::fabs(index) * 4.76837158203125e-7f), 0.25f)));
  }

  bool vertex_fetch(const VertexFetchInstruction& fetch) {
    if (!predicate(fetch.predicate)) return true;
    if (!fetch.mini_fetch) {
      const auto& index_register =
          r[(fetch.source_register + (fetch.source_relative ? static_cast<std::uint32_t>(aL) : 0u)) & 63u];
      const auto index = fetch_index(index_register[fetch.source_swizzle & 3u], fetch.index_rounded);
      const auto slot = kVertexFetchOffset + std::size_t(fetch.fetch_constant & 31u) * 16u;
      const auto packed = word_at(constants, slot + (fetch.fetch_constant_select == 0u ? 0u
                                                     : fetch.fetch_constant_select == 1u ? 8u : 12u));
      vfetch_address = (packed & ~3u) + 4u * index * fetch.stride_dwords;
      vfetch_endian = packed & 3u;
    }
    const std::uint32_t address =
        vfetch_address + 4u * static_cast<std::uint32_t>(fetch.offset_dwords);
    std::array<std::byte, 16> bytes{};
    if (!memory(address, bytes)) {
      error = "vertex fetch outside guest memory";
      return false;
    }
    std::array<std::uint32_t, 4> raw{};
    for (unsigned i = 0; i < 4; ++i) {
      std::uint32_t word{};
      std::memcpy(&word, bytes.data() + i * 4u, 4u);  // ByteAddressBuffer.Load4: little-endian
      raw[i] = endian_swap(word, vfetch_endian);
    }
    const auto format = fetch.data_format;
    const bool is_signed = fetch.signed_data;
    Float4 value{};
    if (format == 36) value = splat(std::bit_cast<float>(raw[0]));
    else if (format == 37) value = {std::bit_cast<float>(raw[0]), std::bit_cast<float>(raw[1]), 0.0f, 1.0f};
    else if (format == 57)
      value = {std::bit_cast<float>(raw[0]), std::bit_cast<float>(raw[1]), std::bit_cast<float>(raw[2]), 1.0f};
    else if (format == 38)
      for (unsigned i = 0; i < 4; ++i) value[i] = std::bit_cast<float>(raw[i]);
    else if (format == 33)
      value = splat(is_signed ? static_cast<float>(static_cast<std::int32_t>(raw[0]))
                              : static_cast<float>(raw[0]));
    else if (format == 34) {
      const auto f = [&](std::uint32_t w) {
        return is_signed ? static_cast<float>(static_cast<std::int32_t>(w)) : static_cast<float>(w);
      };
      value = {f(raw[0]), f(raw[1]), 0.0f, 1.0f};
    } else if (format == 35) {
      for (unsigned i = 0; i < 4; ++i)
        value[i] = is_signed ? static_cast<float>(static_cast<std::int32_t>(raw[i]))
                             : static_cast<float>(raw[i]);
    } else if (format == 31) {
      value = {half_to_float(raw[0] & 65535u), half_to_float(raw[0] >> 16u), 0.0f, 1.0f};
    } else if (format == 32) {
      value = {half_to_float(raw[0] & 65535u), half_to_float(raw[0] >> 16u),
               half_to_float(raw[1] & 65535u), half_to_float(raw[1] >> 16u)};
    } else {
      std::array<std::uint32_t, 4> u{};
      std::array<std::int32_t, 4> s{};
      std::array<std::uint32_t, 4> maximum{1u, 1u, 1u, 1u};
      const auto sext = [](std::uint32_t v, unsigned bits) {
        return static_cast<std::int32_t>(v << (32u - bits)) >> (32u - bits);
      };
      if (format == 6) {
        for (unsigned i = 0; i < 4; ++i) {
          u[i] = (raw[0] >> (i * 8u)) & 255u;
          s[i] = sext(u[i], 8u);
        }
        maximum = {255u, 255u, 255u, 255u};
      } else if (format == 7) {
        u = {raw[0] & 1023u, (raw[0] >> 10u) & 1023u, (raw[0] >> 20u) & 1023u, raw[0] >> 30u};
        maximum = {1023u, 1023u, 1023u, 3u};
        s = {sext(u[0], 10u), sext(u[1], 10u), sext(u[2], 10u), sext(u[3], 2u)};
      } else if (format == 16) {
        u = {raw[0] & 2047u, (raw[0] >> 11u) & 2047u, raw[0] >> 22u, 1u};
        maximum = {2047u, 2047u, 1023u, 1u};
        s = {sext(u[0], 11u), sext(u[1], 11u), sext(u[2], 10u), 1};
      } else if (format == 17) {
        u = {raw[0] & 1023u, (raw[0] >> 10u) & 2047u, raw[0] >> 21u, 1u};
        maximum = {1023u, 2047u, 2047u, 1u};
        s = {sext(u[0], 10u), sext(u[1], 11u), sext(u[2], 11u), 1};
      } else if (format == 25 || format == 26) {
        u = {raw[0] & 65535u, raw[0] >> 16u, raw[1] & 65535u, raw[1] >> 16u};
        maximum = {65535u, 65535u, 65535u, 65535u};
        for (unsigned i = 0; i < 4; ++i) s[i] = sext(u[i], 16u);
        if (format == 25) {
          u[2] = 0u;
          u[3] = 1u;
          s[2] = 0;
          s[3] = 1;
          maximum[2] = maximum[3] = 1u;
        }
      }
      for (unsigned i = 0; i < 4; ++i) {
        if (fetch.normalized) {
          value[i] = is_signed
                         ? std::fmax(static_cast<float>(s[i]) /
                                         static_cast<float>(std::max(maximum[i] >> 1u, 1u)),
                                     -1.0f)
                         : static_cast<float>(u[i]) / static_cast<float>(maximum[i]);
        } else {
          value[i] = is_signed ? static_cast<float>(s[i]) : static_cast<float>(u[i]);
        }
      }
    }
    auto& target =
        r[(fetch.destination_register + (fetch.destination_relative ? static_cast<std::uint32_t>(aL) : 0u)) &
          63u];
    for (unsigned i = 0; i < 4; ++i) {
      const auto select = (fetch.destination_swizzle >> (i * 3u)) & 7u;
      if (select < 4u) target[i] = value[select];
      else if (select == 4u) target[i] = 0.0f;
      else if (select == 5u) target[i] = 1.0f;
    }
    return true;
  }

  bool run(std::uint32_t vertex_id) {
    // r0.x = xenon_vertex_index(vertex_id): VGT_INDX_OFFSET added, 24 bits
    // kept, clamped to [VGT_MIN_VTX_INDX, VGT_MAX_VTX_INDX].
    const auto offset = draw_state(5), minimum = draw_state(6) & 0xFFFFFFu,
               maximum = draw_state(7) & 0xFFFFFFu;
    // HLSL clamp(x, lo, hi) is min(max(x, lo), hi), also when lo > hi.
    r[0][0] = static_cast<float>(std::min(std::max((vertex_id + offset) & 0xFFFFFFu, minimum), maximum));
    std::unordered_map<std::uint32_t, const DecodedInstruction*> instructions;
    for (const auto& instruction : shader.instructions) {
      const auto address = instruction.kind == ShaderInstructionKind::Alu ? instruction.alu.address
                           : instruction.kind == ShaderInstructionKind::VertexFetch
                               ? instruction.vertex_fetch.address
                               : instruction.texture_fetch.address;
      instructions.emplace(address, &instruction);
    }
    std::unordered_map<std::uint32_t, const DecodedControlFlow*> control_flow;
    for (const auto& cf : shader.control_flow) control_flow.emplace(cf.index, &cf);

    std::uint32_t pc = 0, guard = 0, call_depth = 0, loop_depth = 0;
    std::array<std::uint32_t, 4> call_stack{}, loop_remaining{};
    std::array<std::int32_t, 4> loop_value{}, loop_step{};
    bool running = true;
    while (running && guard++ < 65536u) {
      const auto found = control_flow.find(pc);
      if (found == control_flow.end()) break;  // default: running=false
      const auto& cf = *found->second;
      const auto next = cf.index + 1u;
      const auto opcode = cf.opcode;
      const auto value = static_cast<unsigned>(opcode);
      const bool is_exec = (value >= 1u && value <= 6u) || opcode == ControlFlowOpcode::CondExecPredClean ||
                           opcode == ControlFlowOpcode::CondExecPredCleanEnd;
      if (is_exec) {
        bool execute = true;
        if (opcode == ControlFlowOpcode::CondExec || opcode == ControlFlowOpcode::CondExecEnd ||
            opcode == ControlFlowOpcode::CondExecPredClean ||
            opcode == ControlFlowOpcode::CondExecPredCleanEnd) {
          execute = bool_constant(cf.bool_constant) == cf.condition;
        } else if (opcode == ControlFlowOpcode::CondExecPred ||
                   opcode == ControlFlowOpcode::CondExecPredEnd) {
          execute = cf.predicate.condition ? p0 : !p0;
        }
        const bool ends = opcode == ControlFlowOpcode::ExecEnd || opcode == ControlFlowOpcode::CondExecEnd ||
                          opcode == ControlFlowOpcode::CondExecPredEnd ||
                          opcode == ControlFlowOpcode::CondExecPredCleanEnd;
        if (execute) {
          for (std::uint32_t address = cf.target; address < cf.target + cf.count; ++address) {
            const auto instruction = instructions.find(address);
            if (instruction == instructions.end()) {
              error = "control flow references an instruction absent from the decoded IR";
              return false;
            }
            const auto& decoded = *instruction->second;
            if (decoded.kind == ShaderInstructionKind::Alu) {
              alu(decoded.alu);
            } else if (decoded.kind == ShaderInstructionKind::VertexFetch) {
              if (!vertex_fetch(decoded.vertex_fetch)) return false;
            } else {
              const auto op = decoded.texture_fetch.opcode;
              if (op != 24u && op != 25u && op != 26u) {
                error = "texture fetch in a vertex shader has no CPU evaluation";
                return false;
              }
            }
          }
          if (ends) running = false;
        }
        if (running) pc = next;
      } else if (opcode == ControlFlowOpcode::LoopStart) {
        const auto loop_data = word_at(constants, kLoopConstantsOffset + std::size_t(cf.loop_constant & 31u) * 16u);
        const auto count = loop_data & 255u;
        if (count == 0u || loop_depth >= 4u) {
          pc = cf.target;
          continue;
        }
        const auto slot = loop_depth++;
        loop_remaining[slot] = count;
        loop_value[slot] = static_cast<std::int32_t>((loop_data >> 8u) & 255u);
        loop_step[slot] = static_cast<std::int32_t>(loop_data << 8u) >> 24;
        aL = loop_value[slot];
        pc = next;
      } else if (opcode == ControlFlowOpcode::LoopEnd) {
        const bool break_loop = cf.predicate.enabled ? (cf.predicate.condition ? p0 : !p0) : false;
        if (loop_depth == 0u) {
          pc = next;
          continue;
        }
        const auto slot = loop_depth - 1u;
        if (!break_loop && --loop_remaining[slot] != 0u) {
          loop_value[slot] += loop_step[slot];
          aL = loop_value[slot];
          pc = cf.target;
        } else {
          --loop_depth;
          aL = loop_depth ? loop_value[loop_depth - 1u] : 0;
          pc = next;
        }
      } else if (opcode == ControlFlowOpcode::CondCall || opcode == ControlFlowOpcode::CondJump) {
        bool take = true;
        if (!cf.unconditional) {
          take = cf.predicate.enabled ? (cf.predicate.condition ? p0 : !p0)
                                      : bool_constant(cf.bool_constant) == cf.condition;
        }
        if (opcode == ControlFlowOpcode::CondCall) {
          if (take && call_depth < 4u) {
            call_stack[call_depth++] = next;
            pc = cf.target;
          } else {
            pc = next;
          }
        } else {
          pc = take ? cf.target : next;
        }
      } else if (opcode == ControlFlowOpcode::Return) {
        if (call_depth) pc = call_stack[--call_depth];
        else running = false;
      } else {
        pc = next;
      }
    }
    return true;
  }
};

}  // namespace

std::string VertexPositionEvaluator::unsupported_reason(const DecodedShader& shader) {
  if (shader.stage != ShaderStage::Vertex) return "not a vertex shader";
  if (!shader.complete) return "incomplete decoded shader";
  if (shader.reflection.temporary_register_count > 64u) return "more than 64 temporaries";
  static constexpr std::array<std::uint8_t, 15> kFormats{6,  7,  16, 17, 25, 26, 31, 32,
                                                         33, 34, 35, 36, 37, 38, 57};
  for (const auto& instruction : shader.instructions) {
    if (instruction.kind == ShaderInstructionKind::Alu) {
      if (instruction.alu.vector_opcode > 29u) return "reserved vector ALU opcode";
      if (instruction.alu.scalar_opcode == 41u || instruction.alu.scalar_opcode > 50u)
        return "reserved scalar ALU opcode";
    } else if (instruction.kind == ShaderInstructionKind::VertexFetch) {
      if (std::find(kFormats.begin(), kFormats.end(), instruction.vertex_fetch.data_format) ==
          kFormats.end())
        return "vertex format without an evaluation";
    } else {
      const auto op = instruction.texture_fetch.opcode;
      if (op != 24u && op != 25u && op != 26u) return "texture fetch";
    }
  }
  return {};
}

VertexEvaluation VertexPositionEvaluator::evaluate(const DecodedShader& shader,
                                                   std::span<const std::byte> constants,
                                                   std::uint32_t vertex_id,
                                                   const GuestPhysicalReader& memory) {
  VertexEvaluation result{};
  if (constants.size() < kShaderConstantBufferBytes) {
    result.error = "constant buffer is smaller than the shader constant ABI";
    return result;
  }
  if (auto reason = unsupported_reason(shader); !reason.empty()) {
    result.error = std::move(reason);
    return result;
  }
  Machine machine{shader, constants, memory};
  if (!machine.run(vertex_id)) {
    result.error = machine.error;
    return result;
  }
  result.position = machine.e[62];
  // xenon_clip_position: PA_CL_VTE_CNTL VTX_XY_FMT (1), VTX_Z_FMT (2) and
  // VTX_W0_FMT (4) from XenonDrawState[3].x.
  auto position = result.position;
  const auto flags = machine.draw_state(12);
  if ((flags & 4u) == 0u) position[3] = 1.0f / position[3];
  if ((flags & 1u) != 0u) {
    position[0] *= position[3];
    position[1] *= position[3];
  }
  // xenon_host_position's pixel mapping, then the divide by W the rasterizer
  // makes: pixel = (xy * scale + offset * w) / w.
  const auto transform = [&](std::size_t word) { return std::bit_cast<float>(machine.draw_state(word)); };
  const float w = position[3];
  if (!std::isfinite(w) || w == 0.0f) {
    result.error = "exported position has no finite, non-zero W";
    return result;
  }
  result.pixel = {(position[0] * transform(8) + transform(10) * w) / w,
                  (position[1] * transform(9) + transform(11) * w) / w};
  if (!std::isfinite(result.pixel[0]) || !std::isfinite(result.pixel[1])) {
    result.error = "exported position is not finite";
    return result;
  }
  result.valid = true;
  return result;
}

}  // namespace xenon::gpu

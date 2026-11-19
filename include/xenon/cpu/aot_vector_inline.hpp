#pragma once

// Vector semantics generated code calls with the semantic known at compile
// time. execute_vector() is one out-of-line switch over every VMX semantic
// that reads and writes each lane through a byte-by-byte big-endian accessor
// and tests VSCR[NJ] per lane; AC6's audio mixer and its skinning and
// particle code spent a third of their thread in it. vector_compute<S>()
// inlines the hot semantics as SSE on the whole register (AVX2 forms where
// the generated game is built for x86-64-v3) and calls execute_vector() for
// the rest.
//
// Each inline form gives exactly execute_vector()'s bits: the same IEEE
// single-precision operations in the same order under the same MXCSR (packed
// and scalar SSE round alike), VSCR[NJ] flushing of inputs, products and
// results as read_vmx_f32/vmx_multiply_add/write_vmx_f32 do, NaN operand
// order kept, and immediates decoded from the guest word by the same fields.
// tests/cpu/x86_64/vector_inline.cpp compares every one against
// execute_vector() over random and special inputs, with NJ on and off.

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "xenon/cpu/vector_semantic.hpp"

#if defined(_M_X64) || defined(__x86_64__)
#define XENON_VECTOR_INLINE_SSE 1
#include <emmintrin.h>
#if defined(__AVX2__)
#include <immintrin.h>
#endif
#endif

namespace xenon::cpu::aot {

// A VMX arithmetic result whose NaN operands propagate in operand order: the
// first NaN operand, quieted, as AltiVec (and the PPC FPU) specify. A host
// add or multiply cannot be relied on for this - compilers commute them, so
// which of two NaNs came back was a codegen accident that differed between
// execute_vector() and code inlining the same operation.
[[nodiscard]] inline float vmx_quiet(float value) noexcept {
  return std::bit_cast<float>(std::bit_cast<std::uint32_t>(value) | 0x00400000u);
}
[[nodiscard]] inline float vmx_nan_order(float result, float x, float y) noexcept {
  if (std::isnan(x)) return vmx_quiet(x);
  if (std::isnan(y)) return vmx_quiet(y);
  return result;
}

#if defined(XENON_VECTOR_INLINE_SSE)
namespace vector_inline {

#if defined(_MSC_VER) && !defined(__clang__)
#define XENON_VECTOR_FORCEINLINE __forceinline
#else
#define XENON_VECTOR_FORCEINLINE inline __attribute__((always_inline))
#endif

XENON_VECTOR_FORCEINLINE __m128i load(const Vector128& v) noexcept {
  return _mm_load_si128(reinterpret_cast<const __m128i*>(v.bytes.data()));
}
XENON_VECTOR_FORCEINLINE Vector128 store(__m128i x) noexcept {
  Vector128 r;
  _mm_store_si128(reinterpret_cast<__m128i*>(r.bytes.data()), x);
  return r;
}
// Big-endian lanes <-> host lanes.
XENON_VECTOR_FORCEINLINE __m128i swap16(__m128i v) noexcept {
  return _mm_or_si128(_mm_slli_epi16(v, 8), _mm_srli_epi16(v, 8));
}
XENON_VECTOR_FORCEINLINE __m128i swap32(__m128i v) noexcept {
#if defined(__AVX2__)
  return _mm_shuffle_epi8(v, _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3));
#else
  v = swap16(v);
  v = _mm_shufflelo_epi16(v, _MM_SHUFFLE(2, 3, 0, 1));
  return _mm_shufflehi_epi16(v, _MM_SHUFFLE(2, 3, 0, 1));
#endif
}
XENON_VECTOR_FORCEINLINE __m128 load_f32(const Vector128& v) noexcept {
  return _mm_castsi128_ps(swap32(load(v)));
}
XENON_VECTOR_FORCEINLINE Vector128 store_f32(__m128 v) noexcept {
  return store(swap32(_mm_castps_si128(v)));
}
// vmx_flush_subnormal() on every lane: with VSCR[NJ] set a denormal keeps
// only its sign (a zero is unchanged by the same mask).
XENON_VECTOR_FORCEINLINE __m128 flush(__m128 v, bool non_java) noexcept {
  if (!non_java) return v;
  const __m128i bits = _mm_castps_si128(v);
  const __m128i exponent_zero = _mm_cmpeq_epi32(
      _mm_and_si128(bits, _mm_set1_epi32(0x7F800000)), _mm_setzero_si128());
  return _mm_castsi128_ps(
      _mm_andnot_si128(_mm_and_si128(exponent_zero, _mm_set1_epi32(0x7FFFFFFF)), bits));
}
XENON_VECTOR_FORCEINLINE __m128 read_vmx(const Vector128& v, bool non_java) noexcept {
  return flush(load_f32(v), non_java);
}
XENON_VECTOR_FORCEINLINE Vector128 write_vmx(__m128 v, bool non_java) noexcept {
  return store_f32(flush(v, non_java));
}
XENON_VECTOR_FORCEINLINE __m128 select(__m128 otherwise, __m128 chosen, __m128 mask) noexcept {
  return _mm_or_ps(_mm_andnot_ps(mask, otherwise), _mm_and_ps(mask, chosen));
}
// vmx_nan_order() on every lane.
XENON_VECTOR_FORCEINLINE __m128 nan_order(__m128 result, __m128 x, __m128 y) noexcept {
  const __m128 quiet = _mm_castsi128_ps(_mm_set1_epi32(0x00400000));
  result = select(result, _mm_or_ps(y, quiet), _mm_cmpunord_ps(y, y));
  return select(result, _mm_or_ps(x, quiet), _mm_cmpunord_ps(x, x));
}
XENON_VECTOR_FORCEINLINE __m128 add(__m128 x, __m128 y) noexcept {
  return nan_order(_mm_add_ps(x, y), x, y);
}
XENON_VECTOR_FORCEINLINE __m128 sub(__m128 x, __m128 y) noexcept {
  return nan_order(_mm_sub_ps(x, y), x, y);
}
XENON_VECTOR_FORCEINLINE __m128 mul(__m128 x, __m128 y) noexcept {
  return nan_order(_mm_mul_ps(x, y), x, y);
}
XENON_VECTOR_FORCEINLINE __m128 negate(__m128 v) noexcept {
  return _mm_xor_ps(v, _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(0x80000000u))));
}
// vmx_multiply_add(x, y, z) = flush(x * y) + z, lane-wise.
XENON_VECTOR_FORCEINLINE __m128 multiply_add(__m128 x, __m128 y, __m128 z, bool nj) noexcept {
  return add(flush(mul(x, y), nj), z);
}
// vmx_negative_multiply_subtract(x, y, z) = -(flush(x * y) - z).
XENON_VECTOR_FORCEINLINE __m128 negative_multiply_subtract(__m128 x, __m128 y, __m128 z,
                                                           bool nj) noexcept {
  return negate(sub(flush(mul(x, y), nj), z));
}
XENON_VECTOR_FORCEINLINE Vector128 mask_result(__m128 mask) noexcept {
  // All-ones or zero lanes read the same in either byte order.
  return store(_mm_castps_si128(mask));
}
XENON_VECTOR_FORCEINLINE std::uint32_t word(const Vector128& v, unsigned lane) noexcept {
  std::uint32_t w;
  std::memcpy(&w, v.bytes.data() + (lane & 3u) * 4u, 4u);
  return w;  // the lane's bytes as they are (big-endian)
}
XENON_VECTOR_FORCEINLINE void set_word(Vector128& v, unsigned lane, std::uint32_t w) noexcept {
  std::memcpy(v.bytes.data() + (lane & 3u) * 4u, &w, 4u);
}

}  // namespace vector_inline
#endif

template <VectorSemantic S>
inline constexpr bool kVectorComputeInline =
#if defined(XENON_VECTOR_INLINE_SSE)
    S == VectorSemantic::vaddfp || S == VectorSemantic::vaddfp128 ||
    S == VectorSemantic::vsubfp || S == VectorSemantic::vsubfp128 ||
    S == VectorSemantic::vmulfp128 || S == VectorSemantic::vmaddfp ||
    S == VectorSemantic::vmaddfp128 || S == VectorSemantic::vmaddcfp128 ||
    S == VectorSemantic::vnmsubfp || S == VectorSemantic::vnmsubfp128 ||
    S == VectorSemantic::vmsum3fp128 || S == VectorSemantic::vmsum4fp128 ||
    S == VectorSemantic::vcmpeqfp || S == VectorSemantic::vcmpeqfp128 ||
    S == VectorSemantic::vcmpgefp || S == VectorSemantic::vcmpgefp128 ||
    S == VectorSemantic::vcmpgtfp || S == VectorSemantic::vcmpgtfp128 ||
    S == VectorSemantic::vrefp || S == VectorSemantic::vrefp128 ||
    S == VectorSemantic::vrsqrtefp || S == VectorSemantic::vrsqrtefp128 ||
    S == VectorSemantic::vcfsx || S == VectorSemantic::vcsxwfp128 ||
    S == VectorSemantic::vmrghw || S == VectorSemantic::vmrghw128 ||
    S == VectorSemantic::vmrglw || S == VectorSemantic::vmrglw128 ||
    S == VectorSemantic::vmrghh || S == VectorSemantic::vmrglh ||
    S == VectorSemantic::vmrghb || S == VectorSemantic::vmrglb ||
    S == VectorSemantic::vsldoi || S == VectorSemantic::vsldoi128 ||
    S == VectorSemantic::vperm || S == VectorSemantic::vperm128 ||
    S == VectorSemantic::vpermwi128 || S == VectorSemantic::vrlimi128 ||
    S == VectorSemantic::vupkhsh || S == VectorSemantic::vupkhsh128 ||
    S == VectorSemantic::vupklsh || S == VectorSemantic::vupklsh128 ||
    S == VectorSemantic::vupkhsb || S == VectorSemantic::vupkhsb128 ||
    S == VectorSemantic::vupklsb || S == VectorSemantic::vupklsb128 ||
    S == VectorSemantic::vcmpequb || S == VectorSemantic::vcmpequh ||
    S == VectorSemantic::vcmpequw || S == VectorSemantic::vcmpequw128 ||
    S == VectorSemantic::vslw || S == VectorSemantic::vslw128 ||
    S == VectorSemantic::vsrw || S == VectorSemantic::vsrw128 ||
    S == VectorSemantic::vsraw || S == VectorSemantic::vsraw128;
#else
    false;
#endif

// The semantic S of guest instruction `w` on (a, b, c, d), as
// execute_vector(S, w, state, a, b, c, d) computes it.
template <VectorSemantic S>
[[nodiscard]] inline Vector128 vector_compute(std::uint32_t w, CpuState& state,
                                              const Vector128& a, const Vector128& b,
                                              const Vector128& c, const Vector128& d) {
#if defined(XENON_VECTOR_INLINE_SSE)
  using namespace vector_inline;
  using V = VectorSemantic;
  if constexpr (kVectorComputeInline<S>) {
    const bool nj = state.vector_non_java();
    (void)nj;
    (void)d;
    if constexpr (S == V::vaddfp || S == V::vaddfp128) {
      return write_vmx(add(read_vmx(a, nj), read_vmx(b, nj)), nj);
    } else if constexpr (S == V::vsubfp || S == V::vsubfp128) {
      return write_vmx(sub(read_vmx(a, nj), read_vmx(b, nj)), nj);
    } else if constexpr (S == V::vmulfp128) {
      return write_vmx(mul(read_vmx(a, nj), read_vmx(b, nj)), nj);
    } else if constexpr (S == V::vmaddfp) {
      return write_vmx(multiply_add(read_vmx(a, nj), read_vmx(c, nj), read_vmx(b, nj), nj), nj);
    } else if constexpr (S == V::vmaddfp128) {
      return write_vmx(multiply_add(read_vmx(b, nj), read_vmx(c, nj), read_vmx(a, nj), nj), nj);
    } else if constexpr (S == V::vmaddcfp128) {
      return write_vmx(multiply_add(read_vmx(b, nj), read_vmx(a, nj), read_vmx(c, nj), nj), nj);
    } else if constexpr (S == V::vnmsubfp) {
      return write_vmx(
          negative_multiply_subtract(read_vmx(a, nj), read_vmx(c, nj), read_vmx(b, nj), nj), nj);
    } else if constexpr (S == V::vnmsubfp128) {
      return write_vmx(
          negative_multiply_subtract(read_vmx(b, nj), read_vmx(c, nj), read_vmx(a, nj), nj), nj);
    } else if constexpr (S == V::vmsum3fp128 || S == V::vmsum4fp128) {
      // The products of flushed inputs, summed in lane order from 0.0f.
      alignas(16) float products[4];
      _mm_store_ps(products, mul(read_vmx(b, nj), read_vmx(c, nj)));
      float z = 0.0f;
      z = vmx_nan_order(z + products[0], z, products[0]);
      z = vmx_nan_order(z + products[1], z, products[1]);
      z = vmx_nan_order(z + products[2], z, products[2]);
      if constexpr (S == V::vmsum4fp128) z = vmx_nan_order(z + products[3], z, products[3]);
      return write_vmx(_mm_set1_ps(z), nj);
    } else if constexpr (S == V::vcmpeqfp || S == V::vcmpeqfp128) {
      return mask_result(_mm_cmpeq_ps(read_vmx(a, nj), read_vmx(b, nj)));
    } else if constexpr (S == V::vcmpgefp || S == V::vcmpgefp128) {
      return mask_result(_mm_cmpge_ps(read_vmx(a, nj), read_vmx(b, nj)));
    } else if constexpr (S == V::vcmpgtfp || S == V::vcmpgtfp128) {
      return mask_result(_mm_cmpgt_ps(read_vmx(a, nj), read_vmx(b, nj)));
    } else if constexpr (S == V::vrefp || S == V::vrefp128) {
      return write_vmx(_mm_div_ps(_mm_set1_ps(1.0f), read_vmx(a, nj)), nj);
    } else if constexpr (S == V::vrsqrtefp || S == V::vrsqrtefp128) {
      return write_vmx(_mm_div_ps(_mm_set1_ps(1.0f), _mm_sqrt_ps(read_vmx(a, nj))), nj);
    } else if constexpr (S == V::vcfsx || S == V::vcsxwfp128) {
      // vx_imm5 and vx128_imm3 are the same field.
      const auto shift = (w >> 16) & 31u;
      const auto scale = std::bit_cast<float>((127u - shift) << 23u);  // 2^-shift
      return write_vmx(_mm_mul_ps(_mm_cvtepi32_ps(swap32(load(a))), _mm_set1_ps(scale)), nj);
    } else if constexpr (S == V::vmrghw || S == V::vmrghw128) {
      return store(_mm_unpacklo_epi32(load(a), load(b)));
    } else if constexpr (S == V::vmrglw || S == V::vmrglw128) {
      return store(_mm_unpackhi_epi32(load(a), load(b)));
    } else if constexpr (S == V::vmrghh) {
      return store(_mm_unpacklo_epi16(load(a), load(b)));
    } else if constexpr (S == V::vmrglh) {
      return store(_mm_unpackhi_epi16(load(a), load(b)));
    } else if constexpr (S == V::vmrghb) {
      return store(_mm_unpacklo_epi8(load(a), load(b)));
    } else if constexpr (S == V::vmrglb) {
      return store(_mm_unpackhi_epi8(load(a), load(b)));
    } else if constexpr (S == V::vsldoi || S == V::vsldoi128) {
      // va_sh4 and vx128_sh5 are the same field: bytes sh.. of a:b.
      alignas(16) std::uint8_t joined[32];
      _mm_store_si128(reinterpret_cast<__m128i*>(joined), load(a));
      _mm_store_si128(reinterpret_cast<__m128i*>(joined + 16), load(b));
      Vector128 r;
      std::memcpy(r.bytes.data(), joined + ((w >> 6) & 15u), 16u);
      return r;
    } else if constexpr (S == V::vperm || S == V::vperm128) {
#if defined(__AVX2__)
      const __m128i control = _mm_and_si128(load(c), _mm_set1_epi8(0x1F));
      const __m128i index = _mm_and_si128(control, _mm_set1_epi8(0x0F));
      const __m128i from_b = _mm_cmpgt_epi8(control, _mm_set1_epi8(0x0F));
      return store(_mm_blendv_epi8(_mm_shuffle_epi8(load(a), index),
                                   _mm_shuffle_epi8(load(b), index), from_b));
#else
      alignas(16) std::uint8_t joined[32];
      _mm_store_si128(reinterpret_cast<__m128i*>(joined), load(a));
      _mm_store_si128(reinterpret_cast<__m128i*>(joined + 16), load(b));
      Vector128 r;
      for (unsigned i = 0; i < 16u; ++i) r.bytes[i] = joined[c.bytes[i] & 31u];
      return r;
#endif
    } else if constexpr (S == V::vpermwi128) {
      const auto p = ((w >> 16) & 31u) | (((w >> 6) & 7u) << 5);
      Vector128 r;
      for (unsigned i = 0; i < 4u; ++i) set_word(r, i, word(a, (p >> (6u - 2u * i)) & 3u));
      return r;
    } else if constexpr (S == V::vrlimi128) {
      Vector128 r = a;
      const unsigned rotate = (w >> 6) & 3u;
      const unsigned select = (w >> 16) & 15u;
      for (unsigned i = 0; i < 4u; ++i)
        if (select & (1u << (3u - i))) set_word(r, i, word(b, (i + rotate) & 3u));
      return r;
    } else if constexpr (S == V::vupkhsh || S == V::vupkhsh128) {
      const __m128i h = swap16(load(a));
      return store(swap32(_mm_srai_epi32(_mm_unpacklo_epi16(h, h), 16)));
    } else if constexpr (S == V::vupklsh || S == V::vupklsh128) {
      const __m128i h = swap16(load(a));
      return store(swap32(_mm_srai_epi32(_mm_unpackhi_epi16(h, h), 16)));
    } else if constexpr (S == V::vupkhsb || S == V::vupkhsb128) {
      const __m128i x = load(a);
      return store(swap16(_mm_srai_epi16(_mm_unpacklo_epi8(x, x), 8)));
    } else if constexpr (S == V::vupklsb || S == V::vupklsb128) {
      const __m128i x = load(a);
      return store(swap16(_mm_srai_epi16(_mm_unpackhi_epi8(x, x), 8)));
    } else if constexpr (S == V::vcmpequb) {
      return store(_mm_cmpeq_epi8(load(a), load(b)));
    } else if constexpr (S == V::vcmpequh) {
      return store(_mm_cmpeq_epi16(load(a), load(b)));
    } else if constexpr (S == V::vcmpequw || S == V::vcmpequw128) {
      return store(_mm_cmpeq_epi32(load(a), load(b)));
    } else if constexpr (S == V::vslw || S == V::vslw128 || S == V::vsrw || S == V::vsrw128 ||
                         S == V::vsraw || S == V::vsraw128) {
      const __m128i value = swap32(load(a));
      const __m128i count = _mm_and_si128(swap32(load(b)), _mm_set1_epi32(31));
#if defined(__AVX2__)
      if constexpr (S == V::vslw || S == V::vslw128)
        return store(swap32(_mm_sllv_epi32(value, count)));
      else if constexpr (S == V::vsrw || S == V::vsrw128)
        return store(swap32(_mm_srlv_epi32(value, count)));
      else
        return store(swap32(_mm_srav_epi32(value, count)));
#else
      alignas(16) std::uint32_t lanes[4];
      alignas(16) std::uint32_t counts[4];
      _mm_store_si128(reinterpret_cast<__m128i*>(lanes), value);
      _mm_store_si128(reinterpret_cast<__m128i*>(counts), count);
      for (unsigned i = 0; i < 4u; ++i) {
        if constexpr (S == V::vslw || S == V::vslw128)
          lanes[i] <<= counts[i];
        else if constexpr (S == V::vsrw || S == V::vsrw128)
          lanes[i] >>= counts[i];
        else
          lanes[i] = static_cast<std::uint32_t>(static_cast<std::int32_t>(lanes[i]) >>
                                                static_cast<int>(counts[i]));
      }
      return store(swap32(_mm_load_si128(reinterpret_cast<const __m128i*>(lanes))));
#endif
    }
  }
#endif
  return execute_vector(S, w, state, a, b, c, d);
}

}  // namespace xenon::cpu::aot

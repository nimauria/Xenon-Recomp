// Regression (AC6's audio mixer, skinning and particle code spent a third of
// their thread in the out-of-line execute_vector() switch and the byte-range
// path of stvlx/stvrx; mission audio starved and cut out): generated code now
// calls vector_compute<S>(), which inlines the hot VMX semantics as SSE, and
// the VMX partial loads and stores take a native quadword path.
//
// Every inlined semantic must give execute_vector()'s exact bits and VSCR -
// NaN payloads, signed zeros, denormals flushed or kept by VSCR[NJ], products
// that underflow, saturating immediates - over random and special inputs.
// This file is built twice: as the runtime is (SSE2) and, where the host has
// it, as generated games are (x86-64-v3, the AVX2 forms).

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <utility>

#include "xenon/cpu/aot_semantics.hpp"
#include "xenon/memory/address_space.hpp"

using namespace xenon::cpu;
using aot::VectorSemantic;

namespace {

constexpr std::array<std::uint32_t, 24> kSpecialWords{
    0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u,  // 0, -0, 1, -1
    0x00000001u, 0x807FFFFFu, 0x00400000u, 0x80000010u,  // denormals
    0x7F800000u, 0xFF800000u,                            // infinities
    0x7FC00000u, 0xFFC00000u, 0x7FC12345u, 0x7F800001u,  // NaNs: quiet, payload, signalling
    0xFFA00001u, 0x7F7FFFFFu, 0x00800000u, 0x80800000u,  // signalling, max, min normal
    0x1F800000u, 0x9F800000u,                            // squares underflow to denormal
    0x7FFFFFFFu, 0x4B000001u, 0xCF000000u, 0x0000FFFFu,  // integer extremes, odd halves
};

struct Random {
  std::uint64_t state;
  std::uint32_t next() {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<std::uint32_t>(state >> 32u);
  }
  std::uint32_t word() {
    const auto pick = next();
    if (pick % 3u == 0u) return kSpecialWords[(pick >> 8u) % kSpecialWords.size()];
    if (pick % 3u == 1u) {
      // A random float near 1, where products and sums stay finite.
      return 0x3E000000u + (next() & 0x01FFFFFFu) + ((pick & 0x100u) ? 0x80000000u : 0u);
    }
    return next();
  }
  Vector128 vector() {
    Vector128 v;
    for (unsigned lane = 0; lane < 4u; ++lane) v.set_u32_be(lane, word());
    return v;
  }
};

void print_vector(const char* name, const Vector128& v) {
  std::printf("  %s =", name);
  for (unsigned lane = 0; lane < 4u; ++lane) std::printf(" %08X", v.u32_be(lane));
  std::printf("\n");
}

template <VectorSemantic S>
void check_semantic(Random& random) {
  static_assert(aot::kVectorComputeInline<S>, "check only semantics vector_compute inlines");
  for (std::uint32_t iteration = 0; iteration < 40000u; ++iteration) {
    const auto a = random.vector();
    const auto b = random.vector();
    const auto c = random.vector();  // vperm: random controls reach every index
    const auto d = random.vector();
    const auto w = random.next();
    CpuState inline_state{};
    CpuState reference_state{};
    inline_state.vscr = reference_state.vscr =
        ((iteration & 1u) ? vscr_bits::NJ : 0u) | ((iteration & 2u) ? vscr_bits::SAT : 0u);
    const auto got = aot::vector_compute<S>(w, inline_state, a, b, c, d);
    const auto want = aot::execute_vector(S, w, reference_state, a, b, c, d);
    if (got.bytes != want.bytes || inline_state.vscr != reference_state.vscr) {
      std::printf("vector_compute differs from execute_vector: semantic %u, word %08X, vscr %08X\n",
                  static_cast<unsigned>(S), w,
                  static_cast<unsigned>(reference_state.vscr));
      print_vector("a", a);
      print_vector("b", b);
      print_vector("c", c);
      print_vector("got", got);
      print_vector("want", want);
      std::fflush(stdout);
      assert(false);
    }
  }
}

template <VectorSemantic... S>
void check_semantics(Random& random) {
  (check_semantic<S>(random), ...);
}

void test_inline_semantics_match_execute_vector() {
#if defined(XENON_VECTOR_INLINE_SSE)
  Random random{0x5EED5EEDull};
  using V = VectorSemantic;
  check_semantics<V::vaddfp, V::vaddfp128, V::vsubfp, V::vsubfp128, V::vmulfp128, V::vmaddfp,
                  V::vmaddfp128, V::vmaddcfp128, V::vnmsubfp, V::vnmsubfp128, V::vmsum3fp128,
                  V::vmsum4fp128, V::vcmpeqfp, V::vcmpeqfp128, V::vcmpgefp, V::vcmpgefp128,
                  V::vcmpgtfp, V::vcmpgtfp128, V::vrefp, V::vrefp128, V::vrsqrtefp,
                  V::vrsqrtefp128, V::vcfsx, V::vcsxwfp128, V::vmrghw, V::vmrghw128, V::vmrglw,
                  V::vmrglw128, V::vmrghh, V::vmrglh, V::vmrghb, V::vmrglb, V::vsldoi,
                  V::vsldoi128, V::vperm, V::vperm128, V::vpermwi128, V::vrlimi128, V::vupkhsh,
                  V::vupkhsh128, V::vupklsh, V::vupklsh128, V::vupkhsb, V::vupkhsb128,
                  V::vupklsb, V::vupklsb128, V::vcmpequb, V::vcmpequh, V::vcmpequw,
                  V::vcmpequw128, V::vslw, V::vslw128, V::vsrw, V::vsrw128, V::vsraw,
                  V::vsraw128>(random);
#if defined(__AVX2__)
  std::cout << "  [PASS] inlined VMX semantics (AVX2 forms) match execute_vector\n";
#else
  std::cout << "  [PASS] inlined VMX semantics (SSE2 forms) match execute_vector\n";
#endif
#else
  std::cout << "  [PASS] no inline VMX forms on this host; vector_compute is execute_vector\n";
#endif
}

// The partial loads and stores against a model of the block: each offset,
// left and right, on native RAM (the quadword fast path) and through the
// checked byte-range path, with the block's other bytes untouched and the
// page published dirty.
void test_partial_quadword_accesses() {
  using namespace xenon::memory;
  constexpr GuestAddress kBase = 0x00300000u;
  for (const bool native : {true, false}) {
    AddressSpace memory;
    assert(memory.initialize());
    memory.set_native_access(native);
    assert(memory.commit_fixed(kBase, kBasePageSize, kReadWrite));
    auto access = memory.access_context();
    assert(access.native_inline() == native);
    Vector128 value;
    for (unsigned i = 0; i < 16u; ++i) value.bytes[i] = static_cast<std::uint8_t>(0xA0u + i);
    Vector128 fill;
    for (unsigned i = 0; i < 16u; ++i) fill.bytes[i] = static_cast<std::uint8_t>(0x10u + i);
    for (unsigned offset = 0; offset < 16u; ++offset) {
      for (const bool left : {true, false}) {
        const GuestAddress block = kBase + 0x40u;
        access.write128(block - 16u, fill);
        access.write128(block, fill);
        access.write128(block + 16u, fill);
        (void)memory.coherency().current_epoch();  // collect the setup stores
        const auto before = memory.coherency().current_epoch();
        const GuestAddress ea = block + offset;
        if (left) aot::vector_store_left(value, access, ea);
        else aot::vector_store_right(value, access, ea);
        std::array<std::uint8_t, 48> bytes{};
        for (unsigned i = 0; i < 48u; ++i) bytes[i] = access.read8(block - 16u + i);
        std::array<std::uint8_t, 48> expected{};
        for (unsigned i = 0; i < 48u; ++i) expected[i] = fill.bytes[i % 16u];
        // stvlx: bytes 0..15-offset of the register from ea up; stvrx: the
        // last `offset` bytes of the register below ea.
        if (left) {
          for (unsigned i = 0; i + offset < 16u; ++i) expected[16u + offset + i] = value.bytes[i];
        } else {
          for (unsigned i = 0; i < offset; ++i) expected[16u + i] = value.bytes[16u - offset + i];
        }
        assert(bytes == expected);
        const bool stored = left || offset != 0u;
        assert((memory.coherency().current_epoch() > before) == stored);

        // The loads read back what a model of the block gives.
        const auto loaded = left ? aot::vector_load_left({}, access, ea)
                                 : aot::vector_load_right({}, access, ea);
        Vector128 model{};
        if (left) {
          for (unsigned i = 0; i + offset < 16u; ++i) model.bytes[i] = expected[16u + offset + i];
        } else {
          for (unsigned i = 0; i < offset; ++i) model.bytes[16u - offset + i] = expected[16u + i];
        }
        assert(loaded.bytes == model.bytes);
      }
    }
  }
  std::cout << "  [PASS] VMX partial quadword loads and stores, native and checked\n";
}

}  // namespace

int main() {
  test_inline_semantics_match_execute_vector();
  test_partial_quadword_accesses();
  std::cout << "xenon_cpu_vector_inline: ok\n";
  return 0;
}

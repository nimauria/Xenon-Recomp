// Semantics of the instructions the dynamic fallback gained to reach parity with
// the native backend: carry arithmetic (addc/adde/addme/addze, subfc/subfe/subfme/
// subfze/subfic), overflow-enabled add/subtract, atomics (lwarx/ldarx/stwcx/stdcx),
// load/store multiple and string, FPSCR moves, MSR access and stfiwx.
//
// The parity sweep (fallback_lifter_parity_tests) proves the fallback *accepts*
// each instruction; these tests prove it computes the right thing. Expected values
// come from independent formulas (bit-level carry logic, plain byte arithmetic),
// not from the code under test.

#include <bit>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

#include "xenon/cpu/dynamic_fallback.hpp"
#include "xenon/memory/address_space.hpp"

using namespace xenon::cpu;
using namespace xenon::memory;

namespace {

constexpr GuestAddress kCode = 0x00640000u;
constexpr GuestAddress kData = kCode + kBasePageSize;
constexpr GuestAddress kReturn = 0x0064F000u;

// ---- Encoders ------------------------------------------------------------------
std::uint32_t xo31(unsigned xo, unsigned rt, unsigned ra, unsigned rb, bool oe = false, bool rc = false) {
  return (31u << 26) | (rt << 21) | (ra << 16) | (rb << 11) | ((oe ? 1u : 0u) << 10) | (xo << 1) |
         (rc ? 1u : 0u);
}
std::uint32_t d_form(unsigned opcode, unsigned rt, unsigned ra, std::int16_t simm) {
  return (opcode << 26) | (rt << 21) | (ra << 16) | static_cast<std::uint16_t>(simm);
}
std::uint32_t fp63(unsigned xo, unsigned rt, unsigned ra_field, unsigned rb_field, bool rc = false) {
  return (63u << 26) | (rt << 21) | (ra_field << 16) | (rb_field << 11) | (xo << 1) | (rc ? 1u : 0u);
}

// ---- Independent references ------------------------------------------------------
// Carry out of the top bit of x + y (+ carry_in), from the bit-level identity
// carry = (x&y | (x|y)&~sum) >> 63, applied twice for the three-input form.
bool carry64(std::uint64_t x, std::uint64_t y, bool carry_in) {
  const std::uint64_t s1 = x + y;
  const bool c1 = ((x & y) | ((x | y) & ~s1)) >> 63;
  const std::uint64_t s2 = s1 + (carry_in ? 1u : 0u);
  const bool c2 = ((s1 & (carry_in ? 1u : 0u)) | ((s1 | (carry_in ? 1u : 0u)) & ~s2)) >> 63;
  return c1 || c2;
}
// Signed overflow of x + y + carry_in: same-signed operands with a differently
// signed result (carry-in is 0 or 1, so this identity holds for it too).
bool overflow64(std::uint64_t x, std::uint64_t y, bool carry_in) {
  const std::uint64_t r = x + y + (carry_in ? 1u : 0u);
  const bool sx = x >> 63, sy = y >> 63, sr = r >> 63;
  return sx == sy && sr != sx;
}

struct Harness {
  AddressSpace memory;
  NullRuntimeServices runtime;
  CpuState state{};
  DynamicFallbackExecutor fallback{};
  ExecutionContext context;

  Harness() : context(state, memory, runtime) {
    const bool ok = memory.initialize();
    assert(ok);
    const bool code = memory.commit_fixed(kCode, kBasePageSize, kReadWriteExecute);
    const bool data = memory.commit_fixed(kData, kBasePageSize, kReadWrite);
    assert(code && data);
    static_cast<void>(ok);
    static_cast<void>(code);
    static_cast<void>(data);
    fallback.bind(context);
  }

  // Runs the given words followed by a blr, with `state` prepared by `setup`.
  template <class Setup>
  void run(const std::vector<std::uint32_t>& words, Setup&& setup) {
    GuestAddress at = kCode;
    for (const auto word : words) {
      memory.write32_be(at, word);
      at += 4u;
    }
    memory.write32_be(at, 0x4E800020u);  // blr
    state = {};
    state.lr = kReturn;
    setup(state);
    const auto result = context.try_dynamic_fallback(kCode, CompiledLookupKind::Call);
    assert(result.handled && result.result.reason == FlowReason::Return);
  }
};

constexpr std::uint64_t kValues[] = {
    0x0000000000000000ull, 0x0000000000000001ull, 0xFFFFFFFFFFFFFFFFull, 0x8000000000000000ull,
    0x7FFFFFFFFFFFFFFFull, 0x123456789ABCDEF0ull, 0x00000000FFFFFFFFull, 0x0000000100000000ull,
};

// addc/adde/addme/addze: value, XER.CA, XER.OV/SO (with OE) and CR0 (with Rc).
void test_add_carry_family() {
  Harness h;
  for (const auto a : kValues) {
    for (const auto b : kValues) {
      for (const bool ca : {false, true}) {
        // addc r3,r4,r5 : CA out only, ignores incoming CA.
        h.run({xo31(10, 3, 4, 5, /*oe=*/true, /*rc=*/true)}, [&](CpuState& s) {
          s.gpr[4] = a; s.gpr[5] = b; s.set_xer_ca(ca);
        });
        assert(h.state.gpr[3] == a + b);
        assert(h.state.xer_ca() == carry64(a, b, false));
        assert(((h.state.xer & xer_bits::OV) != 0u) == overflow64(a, b, false));
        assert(((h.state.xer & xer_bits::SO) != 0u) == overflow64(a, b, false));
        assert(h.state.cr_field(0) == (((a + b) >> 63) ? 0x8u : (a + b) != 0u ? 0x4u : 0x2u) +
                                          (((h.state.xer & xer_bits::SO) != 0u) ? 1u : 0u));
        // adde r3,r4,r5 : a + b + CA
        h.run({xo31(138, 3, 4, 5, true, false)}, [&](CpuState& s) {
          s.gpr[4] = a; s.gpr[5] = b; s.set_xer_ca(ca);
        });
        assert(h.state.gpr[3] == a + b + (ca ? 1u : 0u));
        assert(h.state.xer_ca() == carry64(a, b, ca));
        assert(((h.state.xer & xer_bits::OV) != 0u) == overflow64(a, b, ca));
      }
    }
    for (const bool ca : {false, true}) {
      // addme r3,r4 : a + (-1) + CA ; addze r3,r4 : a + 0 + CA
      h.run({xo31(234, 3, 4, 0)}, [&](CpuState& s) { s.gpr[4] = a; s.set_xer_ca(ca); });
      assert(h.state.gpr[3] == a + ~0ull + (ca ? 1u : 0u));
      assert(h.state.xer_ca() == carry64(a, ~0ull, ca));
      h.run({xo31(202, 3, 4, 0, true)}, [&](CpuState& s) { s.gpr[4] = a; s.set_xer_ca(ca); });
      assert(h.state.gpr[3] == a + (ca ? 1u : 0u));
      assert(h.state.xer_ca() == carry64(a, 0, ca));
      assert(((h.state.xer & xer_bits::OV) != 0u) == overflow64(a, 0, ca));
    }
  }
}

// subfc/subfe/subfme/subfze: rB - rA style; CA is "no borrow".
void test_subtract_carry_family() {
  Harness h;
  for (const auto ra : kValues) {
    for (const auto rb : kValues) {
      for (const bool ca : {false, true}) {
        // subfc r3,r4,r5 : rB - rA ; CA = rB >= rA (unsigned)
        h.run({xo31(8, 3, 4, 5, true)}, [&](CpuState& s) { s.gpr[4] = ra; s.gpr[5] = rb; s.set_xer_ca(ca); });
        assert(h.state.gpr[3] == rb - ra);
        assert(h.state.xer_ca() == (rb >= ra));
        assert(((h.state.xer & xer_bits::OV) != 0u) == overflow64(rb, ~ra, true));
        // subfe r3,r4,r5 : rB + ~rA + CA
        h.run({xo31(136, 3, 4, 5, true)}, [&](CpuState& s) { s.gpr[4] = ra; s.gpr[5] = rb; s.set_xer_ca(ca); });
        assert(h.state.gpr[3] == rb + ~ra + (ca ? 1u : 0u));
        assert(h.state.xer_ca() == carry64(rb, ~ra, ca));
        assert(((h.state.xer & xer_bits::OV) != 0u) == overflow64(rb, ~ra, ca));
      }
    }
    for (const bool ca : {false, true}) {
      // subfme r3,r4 : -1 + ~rA + CA ; subfze r3,r4 : 0 + ~rA + CA
      h.run({xo31(232, 3, 4, 0)}, [&](CpuState& s) { s.gpr[4] = ra; s.set_xer_ca(ca); });
      assert(h.state.gpr[3] == ~0ull + ~ra + (ca ? 1u : 0u));
      assert(h.state.xer_ca() == carry64(~0ull, ~ra, ca));
      h.run({xo31(200, 3, 4, 0)}, [&](CpuState& s) { s.gpr[4] = ra; s.set_xer_ca(ca); });
      assert(h.state.gpr[3] == ~ra + (ca ? 1u : 0u));
      assert(h.state.xer_ca() == carry64(0, ~ra, ca));
    }
    // subfic r3,r4,imm : imm - rA, CA = imm >= rA (imm sign-extended, unsigned compare)
    for (const std::int16_t imm : {std::int16_t{0}, std::int16_t{1}, std::int16_t{-1}, std::int16_t{0x7FFF}}) {
      h.run({d_form(8, 3, 4, imm)}, [&](CpuState& s) { s.gpr[4] = ra; });
      const auto immediate = static_cast<std::uint64_t>(static_cast<std::int64_t>(imm));
      assert(h.state.gpr[3] == immediate - ra);
      assert(h.state.xer_ca() == (immediate >= ra));
    }
  }
}

// addo/subfo (the OE forms of plain add/subf) now set XER.OV/SO too.
void test_overflow_enabled_add_and_subtract() {
  Harness h;
  // addo r3,r4,r5 with INT64_MAX + 1 overflows.
  h.run({xo31(266, 3, 4, 5, /*oe=*/true)}, [](CpuState& s) { s.gpr[4] = 0x7FFFFFFFFFFFFFFFull; s.gpr[5] = 1; });
  assert((h.state.xer & xer_bits::OV) != 0u && (h.state.xer & xer_bits::SO) != 0u);
  // No overflow clears OV but leaves the sticky SO.
  h.run({xo31(266, 3, 4, 5, true)}, [](CpuState& s) {
    s.gpr[4] = 1; s.gpr[5] = 2; s.xer |= xer_bits::SO | xer_bits::OV;
  });
  assert((h.state.xer & xer_bits::OV) == 0u && (h.state.xer & xer_bits::SO) != 0u);
  // subfo r3,r4,r5 : rB - rA ; INT64_MIN - 1 overflows.
  h.run({xo31(40, 3, 4, 5, true)}, [](CpuState& s) { s.gpr[4] = 1; s.gpr[5] = 0x8000000000000000ull; });
  assert((h.state.xer & xer_bits::OV) != 0u);
  // Without OE, XER is untouched.
  h.run({xo31(266, 3, 4, 5, false)}, [](CpuState& s) { s.gpr[4] = 0x7FFFFFFFFFFFFFFFull; s.gpr[5] = 1; });
  assert((h.state.xer & xer_bits::OV) == 0u);
}

// lwarx/stwcx and ldarx/stdcx: a store-conditional with a live reservation stores
// and reports EQ; without one (or after another store to the line) it does not.
void test_atomics() {
  Harness h;
  // lwarx r3,0,r4 ; addi r3,r3,1 ; stwcx. r3,0,r4  (an atomic increment)
  h.memory.write32_be(kData, 41u);
  h.run({xo31(20, 3, 0, 4), d_form(14, 3, 3, 1), xo31(150, 3, 0, 4, false, true)},
        [](CpuState& s) { s.gpr[4] = kData; });
  assert(h.memory.read32_be(kData) == 42u && "the conditional store went through");
  assert((h.state.cr_field(0) & 0x2u) != 0u && "CR0.EQ reports success");
  assert(!h.state.reservation.valid && "a store-conditional always clears the reservation");

  // No reservation: the store must fail and leave memory alone, CR0.EQ clear.
  h.memory.write32_be(kData, 7u);
  h.run({xo31(150, 3, 0, 4, false, true)}, [](CpuState& s) { s.gpr[3] = 99; s.gpr[4] = kData; });
  assert(h.memory.read32_be(kData) == 7u && (h.state.cr_field(0) & 0x2u) == 0u);

  // An intervening store to the reserved word breaks the reservation.
  h.memory.write32_be(kData, 100u);
  h.run({xo31(20, 3, 0, 4), d_form(36, 5, 4, 0) /* stw r5,0(r4) */, xo31(150, 6, 0, 4, false, true)},
        [](CpuState& s) { s.gpr[4] = kData; s.gpr[5] = 555; s.gpr[6] = 999; });
  assert(h.memory.read32_be(kData) == 555u && "only the interfering store landed");
  assert((h.state.cr_field(0) & 0x2u) == 0u);

  // The load-reserve result is the zero-extended word; SO is copied into CR0.SO.
  h.memory.write32_be(kData, 0xFFFFFFFEu);
  h.run({xo31(20, 3, 0, 4), xo31(150, 3, 0, 4, false, true)},
        [](CpuState& s) { s.gpr[4] = kData; s.xer |= xer_bits::SO; });
  assert(h.state.gpr[3] == 0xFFFFFFFEull);
  assert((h.state.cr_field(0) & 0x1u) != 0u && "XER.SO is copied to CR0.SO");

  // Doubleword forms.
  h.memory.write64_be(kData, 0x1122334455667788ull);
  h.run({xo31(84, 3, 0, 4), d_form(14, 3, 3, 1), xo31(214, 3, 0, 4, false, true)},
        [](CpuState& s) { s.gpr[4] = kData; });
  assert(h.memory.read64_be(kData) == 0x1122334455667789ull && (h.state.cr_field(0) & 0x2u) != 0u);

  // A width mismatch (ldarx then stwcx) fails.
  h.memory.write64_be(kData, 5u);
  h.run({xo31(84, 3, 0, 4), xo31(150, 3, 0, 4, false, true)}, [](CpuState& s) { s.gpr[4] = kData; });
  assert((h.state.cr_field(0) & 0x2u) == 0u);
}

void test_load_store_multiple() {
  Harness h;
  // stmw r29,8(r4) writes r29,r30,r31 to consecutive words; lmw reads them back.
  h.run({d_form(47, 29, 4, 8)}, [](CpuState& s) {
    s.gpr[4] = kData; s.gpr[29] = 0x1111111111111111ull; s.gpr[30] = 0x22222222AAAAAAAAull; s.gpr[31] = 0x3333333300000003ull;
  });
  assert(h.memory.read32_be(kData + 8u) == 0x11111111u);
  assert(h.memory.read32_be(kData + 12u) == 0xAAAAAAAAu);
  assert(h.memory.read32_be(kData + 16u) == 0x00000003u);
  h.run({d_form(46, 29, 4, 8)}, [](CpuState& s) { s.gpr[4] = kData; s.gpr[29] = ~0ull; s.gpr[30] = ~0ull; s.gpr[31] = ~0ull; });
  assert(h.state.gpr[29] == 0x11111111ull && h.state.gpr[30] == 0xAAAAAAAAull && h.state.gpr[31] == 3ull &&
         "lmw zero-extends each word");
}

void test_load_store_string() {
  Harness h;
  // stswi r5,r4,7 : store the top 7 bytes of r5,r6 (register order) at (r4).
  h.run({xo31(725, 5, 4, 7)}, [](CpuState& s) {
    s.gpr[4] = kData; s.gpr[5] = 0x0102030405060708ull; s.gpr[6] = 0x1112131415161718ull;
  });
  // String ops move the high-order bytes of each 32-bit register value first.
  const std::uint8_t expect[] = {0x05, 0x06, 0x07, 0x08, 0x15, 0x16, 0x17};
  for (unsigned i = 0; i < 7; ++i) assert(h.memory.read8(kData + i) == expect[i]);
  // lswi r7,r4,7 reads them back into r7,r8 (upper bytes first).
  h.run({xo31(597, 7, 4, 7)}, [](CpuState& s) { s.gpr[4] = kData; s.gpr[7] = ~0ull; s.gpr[8] = ~0ull; });
  assert((h.state.gpr[7] & 0xFFFFFFFFull) == 0x05060708ull);
  assert((h.state.gpr[8] & 0xFF000000ull) == 0x15000000ull || (h.state.gpr[8] >> 24 & 0xFF) == 0x15u);
  // lswx: the byte count comes from XER[25:31].
  h.run({xo31(533, 9, 4, 5)}, [](CpuState& s) { s.gpr[4] = kData; s.gpr[5] = 0; s.xer = 4; });
  assert((h.state.gpr[9] & 0xFFFFFFFFull) == 0x05060708ull);
}

void test_fpscr_and_msr_and_stfiwx() {
  Harness h;
  // mtfsfi crf, imm writes an FPSCR nibble; mcrfs copies FPSCR field to CR.
  h.run({fp63(134, 4 << 2 /* crfd=4 */, 0, 0xA << 1 /* imm at bits 12-15 */)}, [](CpuState&) {});
  // mtfsb1 sets an FPSCR bit, mtfsb0 clears it.
  h.run({fp63(38, 30, 0, 0)}, [](CpuState&) {});  // mtfsb1 30
  assert(((h.state.fpscr >> 1) & 1u) != 0u && "architectural bit 30 is FPSCR[RN] low bit");
  h.run({fp63(70, 30, 0, 0)}, [](CpuState& s) { s.fpscr = 0xFFFFFFFFu; });  // mtfsb0 30
  assert(((h.state.fpscr >> 1) & 1u) == 0u);
  // mtfsf FM=0xFF, frB copies the low word of frB into the FPSCR.
  h.run({(63u << 26) | (0xFFu << 17) | (3u << 11) | (711u << 1)}, [](CpuState& s) {
    s.fpr_bits[3] = 0xDEADBEEF00000000ull | 0x000000A0ull;
  });
  assert((h.state.fpscr & 0xF0u) == 0xA0u && "the FPSCR fields were written from frB");

  // MSR moves.
  h.run({xo31(83, 3, 0, 0)}, [](CpuState& s) { s.msr = 0x8000ull; });
  assert(h.state.gpr[3] == 0x8000ull);

  // stfiwx f3,r4,r5 : stores the low 32 bits of FPR 3 verbatim.
  h.run({xo31(983, 3, 4, 5)}, [](CpuState& s) {
    s.gpr[4] = kData; s.gpr[5] = 8; s.fpr_bits[3] = 0x400921FB54442D18ull;
  });
  assert(h.memory.read32_be(kData + 8u) == 0x54442D18u);
}

}  // namespace

int main() {
  std::cout << "Testing the dynamic fallback's parity instructions...\n";
  std::cerr << "[test] add_carry_family" << std::endl; test_add_carry_family();
  std::cerr << "[test] subtract_carry_family" << std::endl; test_subtract_carry_family();
  std::cerr << "[test] overflow_enabled_add_and_subtract" << std::endl; test_overflow_enabled_add_and_subtract();
  std::cerr << "[test] atomics" << std::endl; test_atomics();
  std::cerr << "[test] load_store_multiple" << std::endl; test_load_store_multiple();
  std::cerr << "[test] load_store_string" << std::endl; test_load_store_string();
  std::cerr << "[test] fpscr_and_msr_and_stfiwx" << std::endl; test_fpscr_and_msr_and_stfiwx();
  std::cout << "All dynamic fallback parity semantics tests passed!\n";
  return 0;
}

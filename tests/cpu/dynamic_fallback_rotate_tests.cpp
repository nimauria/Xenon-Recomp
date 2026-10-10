// Doubleword rotate family (rldicl, rldicr, rldic, rldimi, rldcl, rldcr) in the
// dynamic fallback interpreter.
//
// Real-title context: Ace Combat 6 reaches a function (0x821DCC78) that the
// recompiler never discovered, only through an indirect call at runtime, so it
// executes in the fallback; the third instruction there, `rldicl r10,r10,0,32`
// (0x794A0020), used to trap with "unsupported instruction" and ended the run.
// The compiled path (PPC lifter) has always implemented these; the fallback must
// agree with it bit for bit.
//
// Each case executes one instruction through the real fallback and compares the
// result with an independent reference built from the PowerPC architecture
// definition (rotate left, then AND with a mask of architectural bits mb..me,
// where bit 0 is the MOST significant bit) using plain bit loops - it shares no
// code with the interpreter or the lifter.

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
constexpr GuestAddress kReturn = 0x0064F000u;

// ---- Independent reference ---------------------------------------------------
std::uint64_t rotl64(std::uint64_t value, unsigned amount) {
  amount &= 63u;
  if (amount == 0u) return value;
  return (value << amount) | (value >> (64u - amount));
}

// Ones in architectural bit positions mb..me (bit 0 = MSB); wraps if mb > me.
std::uint64_t arch_mask(unsigned mb, unsigned me) {
  std::uint64_t mask = 0;
  const auto set_bit = [&mask](unsigned arch_bit) { mask |= std::uint64_t{1} << (63u - arch_bit); };
  if (mb <= me) {
    for (unsigned b = mb; b <= me; ++b) set_bit(b);
  } else {
    for (unsigned b = mb; b < 64u; ++b) set_bit(b);
    for (unsigned b = 0; b <= me; ++b) set_bit(b);
  }
  return mask;
}

// ---- Encoders (opcode 30) ----------------------------------------------------
// MD-form: rs[21:25] ra[16:20] sh[11:15] mb/me[5:10] xo[2:4] sh5[1] rc[0].
// The 6-bit mask field is stored as mask[0:4] in bits 6-10 and mask[5] in bit 5.
std::uint32_t md(unsigned xo, unsigned rs, unsigned ra, unsigned sh, unsigned mask_field, bool rc) {
  return (30u << 26) | (rs << 21) | (ra << 16) | ((sh & 31u) << 11) |
         ((mask_field & 31u) << 6) | (mask_field & 32u) | (xo << 2) | (((sh >> 5) & 1u) << 1) |
         (rc ? 1u : 0u);
}
// MDS-form (rldcl/rldcr): rs ra rb[11:15] mask[5:10] xo[1:4] rc[0].
std::uint32_t mds(unsigned xo, unsigned rs, unsigned ra, unsigned rb, unsigned mask_field, bool rc) {
  return (30u << 26) | (rs << 21) | (ra << 16) | (rb << 11) | ((mask_field & 31u) << 6) |
         (mask_field & 32u) | (xo << 1) | (rc ? 1u : 0u);
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
    const bool committed = memory.commit_fixed(kCode, kBasePageSize, kReadWriteExecute);
    assert(committed);
    static_cast<void>(ok);
    static_cast<void>(committed);
    fallback.bind(context);
  }

  // Runs `word` at kCode followed by a blr; returns the CpuState afterwards.
  CpuState run(std::uint32_t word, std::uint64_t rs_value, std::uint64_t ra_value,
               std::uint64_t rb_value, unsigned rs, unsigned ra, unsigned rb) {
    memory.write32_be(kCode + 0u, word);
    memory.write32_be(kCode + 4u, 0x4E800020u);  // blr
    state = {};
    state.lr = kReturn;
    state.gpr[rs] = rs_value;
    state.gpr[ra] = (ra == rs) ? rs_value : ra_value;
    state.gpr[rb] = rb_value;
    const auto result = context.try_dynamic_fallback(kCode, CompiledLookupKind::Call);
    assert(result.handled && "the fallback must execute the instruction, not trap on it");
    assert(result.result.reason == FlowReason::Return);
    return state;
  }
};

constexpr std::uint64_t kPatterns[] = {
    0x0123456789ABCDEFull, 0xFFFFFFFFFFFFFFFFull, 0x0000000000000001ull, 0x8000000000000000ull,
    0xDEADBEEFCAFEF00Dull, 0x00000000FFFFFFFFull, 0xFFFFFFFF00000000ull, 0x5555AAAA3333CCCCull,
};

// rldicl: rotate left by SH, clear the mb high bits -> mask(mb..63).
void test_rldicl() {
  Harness h;
  for (const auto value : kPatterns) {
    for (unsigned sh : {0u, 1u, 8u, 31u, 32u, 33u, 63u}) {
      for (unsigned mb : {0u, 1u, 32u, 40u, 63u}) {
        const auto out = h.run(md(0, 4, 5, sh, mb, false), value, 0xAAAAAAAAAAAAAAAAull, 0, 4, 5, 6);
        assert(out.gpr[5] == (rotl64(value, sh) & arch_mask(mb, 63)) && "rldicl");
        assert(out.gpr[4] == value && "the source register is unchanged");
      }
    }
  }
  // The exact instruction Ace Combat 6 trapped on: rldicl r10,r10,0,32 (clrldi
  // r10,r10,32) keeps only the low 32 bits.
  const auto out = h.run(0x794A0020u, 0xFFFFFFFF12345678ull, 0, 0, 10, 10, 6);
  assert(out.gpr[10] == 0x0000000012345678ull);
}

// rldicr: rotate left by SH, keep bits 0..me -> mask(0..me). The field is ME.
void test_rldicr() {
  Harness h;
  for (const auto value : kPatterns) {
    for (unsigned sh : {0u, 4u, 32u, 60u, 63u}) {
      for (unsigned me : {0u, 15u, 31u, 47u, 63u}) {
        const auto out = h.run(md(1, 4, 5, sh, me, false), value, 0, 0, 4, 5, 6);
        assert(out.gpr[5] == (rotl64(value, sh) & arch_mask(0, me)) && "rldicr");
      }
    }
  }
  // Ace Combat 6's next word: 0x7929FFE6 is rldicr r9,r9,63,63.
  const auto out = h.run(0x7929FFE6u, 0x00000000000000FFull, 0, 0, 9, 9, 6);
  assert(out.gpr[9] == (rotl64(0xFFull, 63) & arch_mask(0, 63)));
}

// rldic: rotate left by SH, mask(sh-derived): bits mb .. 63-sh.
void test_rldic() {
  Harness h;
  for (const auto value : kPatterns) {
    for (unsigned sh : {0u, 3u, 16u, 32u, 62u}) {
      for (unsigned mb : {0u, 8u, 33u}) {
        if (mb > 63u - sh) continue;  // keep the mask non-wrapping
        const auto out = h.run(md(2, 4, 5, sh, mb, false), value, 0, 0, 4, 5, 6);
        assert(out.gpr[5] == (rotl64(value, sh) & arch_mask(mb, 63 - sh)) && "rldic");
      }
    }
  }
}

// rldimi: like rldic but the unmasked bits come from the OLD rA.
void test_rldimi() {
  Harness h;
  const std::uint64_t old_ra = 0xA5A5A5A5A5A5A5A5ull;
  for (const auto value : kPatterns) {
    for (unsigned sh : {0u, 5u, 32u, 48u}) {
      for (unsigned mb : {0u, 10u, 30u}) {
        if (mb > 63u - sh) continue;
        const auto mask = arch_mask(mb, 63 - sh);
        const auto out = h.run(md(3, 4, 5, sh, mb, false), value, old_ra, 0, 4, 5, 6);
        assert(out.gpr[5] == ((rotl64(value, sh) & mask) | (old_ra & ~mask)) && "rldimi inserts");
      }
    }
  }
}

// rldcl / rldcr: the rotate amount is the low six bits of rB.
void test_rldcl_rldcr() {
  Harness h;
  for (const auto value : kPatterns) {
    for (std::uint64_t amount : {0ull, 7ull, 32ull, 63ull, 64ull + 5ull, 0xFFFFFFFFFFFFFFC1ull}) {
      for (unsigned field : {0u, 16u, 32u, 63u}) {
        const auto cl = h.run(mds(8, 4, 5, 6, field, false), value, 0, amount, 4, 5, 6);
        assert(cl.gpr[5] == (rotl64(value, static_cast<unsigned>(amount & 63u)) & arch_mask(field, 63)) &&
               "rldcl");
        const auto cr = h.run(mds(9, 4, 5, 6, field, false), value, 0, amount, 4, 5, 6);
        assert(cr.gpr[5] == (rotl64(value, static_cast<unsigned>(amount & 63u)) & arch_mask(0, field)) &&
               "rldcr");
      }
    }
  }
}

// The record forms set CR0 from the 64-bit result (LT/GT/EQ, SO copied from XER).
void test_record_forms_update_cr0() {
  Harness h;
  auto cr0 = [](const CpuState& s) { return s.cr_field(0); };
  // rldicl. producing zero -> EQ
  auto out = h.run(md(0, 4, 5, 0, 63, true), 0xFFFFFFFFFFFFFFFEull, 0, 0, 4, 5, 6);
  assert(out.gpr[5] == 0u && cr0(out) == 0x2u);
  // positive
  out = h.run(md(0, 4, 5, 0, 32, true), 0xFFFFFFFF00000001ull, 0, 0, 4, 5, 6);
  assert(out.gpr[5] == 1u && cr0(out) == 0x4u);
  // negative (the sign bit of the 64-bit result decides, not bit 31)
  out = h.run(md(0, 4, 5, 0, 0, true), 0x8000000000000000ull, 0, 0, 4, 5, 6);
  assert(out.gpr[5] == 0x8000000000000000ull && cr0(out) == 0x8u);
  // Non-record form leaves CR0 alone.
  out = h.run(md(0, 4, 5, 0, 0, false), 0x8000000000000000ull, 0, 0, 4, 5, 6);
  assert(cr0(out) == 0u);
}

}  // namespace

int main() {
  std::cout << "Testing doubleword rotate instructions in the dynamic fallback...\n";
  test_rldicl();
  test_rldicr();
  test_rldic();
  test_rldimi();
  test_rldcl_rldcr();
  test_record_forms_update_cr0();
  std::cout << "All doubleword rotate fallback tests passed!\n";
  return 0;
}

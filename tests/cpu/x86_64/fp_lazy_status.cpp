// Exact, cheap FPSCR status for the default (Tier A) floating-point path, the
// strict per-operation path (Tier B) that it must never interleave with, the
// integer lfs/stfs conversions, and the host boundary. The operands go through
// fp_operand() so the compiler cannot fold an operation away and lose the host
// flag it is expected to raise.
#include <bit>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>

#include "xenon/cpu/aot_semantics.hpp"
#include "xenon/cpu/state.hpp"

using namespace xenon::cpu;
namespace fb = xenon::cpu::fpscr_bits;

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
const double kQnan = std::numeric_limits<double>::quiet_NaN();
const double kSnan = std::bit_cast<double>(0x7FF0000000000001ull);

double v(double x) { return aot::fp_operand(x); }

// A thread-clean starting point: no stale host flags, Tier A selected.
CpuState fresh() {
  aot::fp_strict_mode = false;
  fp_legacy_status = false;
  CpuState discard{};
  discard.fp_sync();
  CpuState s{};
  return s;
}

std::uint32_t observed(CpuState& s) { return s.observe_fpscr(); }

void test_tier_a_status() {
  // Exact result: nothing sticky, FPRF classifies the result.
  {
    CpuState s = fresh();
    const double r = aot::fp_add(s, v(1.0), v(1.0));
    aot::update_fpscr(s, r);
    assert(s.fpscr == 0 && "nothing is folded before an observation point");
    const auto f = observed(s);
    assert((f & (fb::FX | fb::XX | fb::OX | fb::UX | fb::ZX | fb::VX)) == 0);
    assert(((f & fb::FPRF_MASK) >> 12) == 0x04u);
  }
  // Inexact: XX and FX, FI stays clear (Tier A does not track FR/FI).
  {
    CpuState s = fresh();
    aot::update_fpscr(s, aot::fp_add(s, v(1.0), v(0x1p-60)));
    const auto f = observed(s);
    assert((f & (fb::FX | fb::XX)) == (fb::FX | fb::XX));
    assert((f & (fb::FR | fb::FI)) == 0);
    // The fold consumed the host flag: a second observation adds nothing new.
    s.fpscr &= ~(fb::FX | fb::XX);
    assert((observed(s) & (fb::FX | fb::XX)) == 0);
  }
  // Divide by zero, overflow, underflow.
  {
    CpuState s = fresh();
    aot::update_fpscr(s, aot::fp_div(s, v(1.0), v(0.0)));
    const auto f = observed(s);
    assert((f & (fb::ZX | fb::FX)) == (fb::ZX | fb::FX));
    assert(((f & fb::FPRF_MASK) >> 12) == 0x05u);  // +infinity
  }
  {
    CpuState s = fresh();
    aot::update_fpscr(s, aot::fp_mul(s, v(1e300), v(1e300)));
    const auto f = observed(s);
    assert((f & (fb::OX | fb::XX | fb::FX)) == (fb::OX | fb::XX | fb::FX));
  }
  {
    CpuState s = fresh();
    aot::update_fpscr(s, aot::fp_mul(s, v(1e-300), v(1e-300)));
    const auto f = observed(s);
    assert((f & (fb::UX | fb::XX | fb::FX)) == (fb::UX | fb::XX | fb::FX));
    assert(((f & fb::FPRF_MASK) >> 12) == 0x02u);  // +0
  }
  // frsp raises the narrowing flags.
  {
    CpuState s = fresh();
    aot::update_fpscr(s, aot::fp_round_single(s, v(1.0 + 0x1p-40)));
    assert((observed(s) & fb::XX) != 0);
    s = fresh();
    aot::update_fpscr(s, aot::fp_round_single(s, v(1e300)));
    assert((observed(s) & fb::OX) != 0);
  }
}

void test_tier_a_invalid() {
  const auto check = [](CpuState& s, std::uint32_t detail) {
    const auto f = observed(s);
    assert((f & detail) == detail && "VX detail bit");
    assert((f & (fb::VX | fb::FX)) == (fb::VX | fb::FX));
    // No other VX* detail leaks in.
    constexpr std::uint32_t all = aot::fpscr_invalid_detail_mask;
    assert((f & all) == detail);
  };
  { CpuState s = fresh(); aot::update_fpscr(s, aot::fp_div(s, v(0.0), v(0.0))); check(s, fb::VXZDZ); }
  { CpuState s = fresh(); aot::update_fpscr(s, aot::fp_div(s, v(kInf), v(kInf))); check(s, fb::VXIDI); }
  { CpuState s = fresh(); aot::update_fpscr(s, aot::fp_sub(s, v(kInf), v(kInf))); check(s, fb::VXISI); }
  { CpuState s = fresh(); aot::update_fpscr(s, aot::fp_mul(s, v(0.0), v(kInf))); check(s, fb::VXIMZ); }
  { CpuState s = fresh(); aot::update_fpscr(s, aot::fp_sqrt(s, v(-1.0))); check(s, fb::VXSQRT); }
  { CpuState s = fresh(); aot::update_fpscr(s, aot::fp_fma(s, v(kInf), v(0.0), v(1.0))); check(s, fb::VXIMZ); }
  { CpuState s = fresh(); aot::update_fpscr(s, aot::fp_fma(s, v(kInf), v(1.0), v(-kInf))); check(s, fb::VXISI); }
  { CpuState s = fresh(); aot::update_fpscr(s, aot::fp_add(s, v(kSnan), v(1.0))); check(s, fb::VXSNAN); }
  { CpuState s = fresh(); aot::update_fpscr(s, aot::fp_round_single(s, v(kSnan))); check(s, fb::VXSNAN); }
  // A quiet NaN is propagated without any exception.
  {
    CpuState s = fresh();
    const double r = aot::fp_add(s, v(kQnan), v(1.0));
    aot::update_fpscr(s, r);
    const auto f = observed(s);
    assert((f & (aot::fpscr_invalid_detail_mask | fb::VX | fb::FX)) == 0);
    assert(((f & fb::FPRF_MASK) >> 12) == 0x11u);
  }
}

void test_observation_points() {
  // mcrfs: copies the field, then clears its sticky bits for good.
  {
    CpuState s = fresh();
    aot::update_fpscr(s, aot::fp_add(s, v(1.0), v(0x1p-60)));  // XX pending
    aot::move_fpscr_field_to_cr(s, 3, 1);                      // UX ZX XX VXSNAN
    assert(s.cr_field(3) == 0x2u);
    assert((observed(s) & fb::XX) == 0);
  }
  // mtfsf/mtfsfi/mtfsb: a write after pending status replaces it, and the
  // pending result must not resurface afterwards.
  {
    CpuState s = fresh();
    aot::update_fpscr(s, aot::fp_add(s, v(1.0), v(0x1p-60)));
    aot::write_fpscr_fields(s, 0xFFu, 0u);
    assert(s.fpscr == 0);
    assert(observed(s) == 0);
  }
  {
    CpuState s = fresh();
    aot::update_fpscr(s, aot::fp_div(s, v(1.0), v(0.0)));
    aot::set_fpscr_bit(s, 5, false);  // ZX is architectural bit 5
    assert((observed(s) & fb::ZX) == 0);
  }
  // Rc=1 (CR1 from FPSCR) observes the pending state.
  {
    CpuState s = fresh();
    aot::update_fpscr(s, aot::fp_mul(s, v(1e300), v(1e300)));
    s.update_cr1_from_fpscr();
    assert((s.cr_field(1) & 0x9u) == 0x9u);  // FX and OX
  }
  // mffs sees every kind, and the control bits come through untouched.
  {
    CpuState s = fresh();
    aot::update_fpscr(s, aot::fp_add(s, v(2.0), v(3.0)));
    assert((observed(s) & 0xFFu) == 0);
  }
  // A compare replaces the pending FPRF but keeps the sticky host flags.
  {
    CpuState s = fresh();
    aot::update_fpscr(s, aot::fp_add(s, v(1.0), v(0x1p-60)));
    aot::update_fp_compare_status(s, 1.0, 2.0, false);
    const auto f = observed(s);
    assert(((f & fb::FPRF_MASK) >> 12) == 0x8u);
    assert((f & fb::XX) != 0);
  }
  // fctiw: invalid conversion and inexact are recorded immediately and the
  // earlier pending FPRF is committed first, so a later fold cannot undo FI.
  {
    CpuState s = fresh();
    aot::update_fpscr(s, aot::fp_add(s, v(1.0), v(1.0)));
    (void)aot::fp_to_integer_bits(s, v(2.5), 32, true);
    aot::update_fp_conversion_status(s);
    const auto f = observed(s);
    assert((f & (fb::XX | fb::FI)) == (fb::XX | fb::FI));
    (void)aot::fp_to_integer_bits(s, v(1e30), 32, true);
    aot::update_fp_conversion_status(s);
    assert((observed(s) & fb::VXCVI) != 0);
  }
}

void test_tier_b() {
  // An enabled exception selects the exact per-operation path: status is
  // visible immediately, FEX is derived, FI is exact.
  {
    CpuState s = fresh();
    s.fpscr = fb::XE;
    aot::update_fpscr(s, aot::fp_add(s, v(1.0), v(0x1p-60)));
    assert((s.fpscr & (fb::XX | fb::FX | fb::FEX | fb::FI)) ==
           (fb::XX | fb::FX | fb::FEX | fb::FI));
  }
  // A non-nearest rounding mode rounds exactly and reports exactly.
  {
    CpuState s = fresh();
    s.fpscr = static_cast<std::uint32_t>(FpRoundingMode::TowardPositive);
    const double r = aot::fp_add(s, v(1.0), v(0x1p-60));
    assert(r == std::nextafter(1.0, 2.0));
    aot::update_fpscr(s, r);
    assert((s.fpscr & (fb::XX | fb::FI)) == (fb::XX | fb::FI));
  }
  // NI flushes denormals.
  {
    CpuState s = fresh();
    s.fpscr = fb::NI;
    const double r = aot::fp_add(s, v(1e-310), v(0.0));
    assert(r == 0.0);
    CpuState t = fresh();
    assert(aot::fp_add(t, v(1e-310), v(0.0)) == 1e-310);
  }
  // Tier A status pending when the guest switches to Tier B is folded first, so
  // the later operation owns FPRF and the earlier XX survives.
  {
    CpuState s = fresh();
    aot::update_fpscr(s, aot::fp_add(s, v(1.0), v(0x1p-60)));   // pending, XX in MXCSR
    s.fpscr = static_cast<std::uint32_t>(FpRoundingMode::TowardZero);
    const double r = aot::fp_add(s, v(-3.0), v(0.0));
    aot::update_fpscr(s, r);
    assert(((s.fpscr & fb::FPRF_MASK) >> 12) == 0x08u);
    assert((s.fpscr & fb::XX) != 0);
  }
  // Strict mode keeps the old always-exact behaviour.
  {
    CpuState s = fresh();
    aot::fp_strict_mode = true;
    aot::update_fpscr(s, aot::fp_div(s, v(1.0), v(0.0)));
    assert((s.fpscr & (fb::ZX | fb::FX)) == (fb::ZX | fb::FX));
    aot::fp_strict_mode = false;
  }
}

void test_kill_switch() {
  CpuState s = fresh();
  fp_legacy_status = true;
  aot::update_fpscr(s, aot::fp_div(s, v(0.0), v(0.0)));
  aot::update_fpscr(s, aot::fp_add(s, v(1.0), v(0x1p-60)));
  assert(observed(s) == 0 && "the previous behaviour: native FP never updates status");
  fp_legacy_status = false;
}

void test_host_boundary() {
  CpuState s = fresh();
  aot::update_fpscr(s, aot::fp_div(s, v(1.0), v(0.0)));  // guest ZX pending
  {
    FpHostBoundary boundary(s);
    // Host code: an inexact operation and an overflow on the same thread.
    volatile double h = v(1.0) / v(3.0);
    volatile double o = v(1e300) * v(1e300);
    (void)h; (void)o;
  }
  const auto f = observed(s);
  assert((f & fb::ZX) != 0 && "guest status survives the boundary");
  assert((f & (fb::OX | fb::XX)) == 0 && "host flags never reach the guest");
}

void test_integer_conversions() {
  // No host flag is raised, whatever the value.
  CpuState discard{};
  discard.fp_sync();
  std::mt19937_64 rng(0x1F5u);
  const auto csr_flags = [] { return _mm_getcsr() & 0x3Fu; };
  std::uint64_t compared = 0;
  for (int i = 0; i < 400000; ++i) {
    std::uint64_t bits = rng();
    if (i & 1) {  // concentrate on the exponents around the single range
      const std::uint64_t e = 860 + (rng() % 320);
      bits = (bits & 0x800FFFFFFFFFFFFFull) | (e << 52);
    }
    const double d = std::bit_cast<double>(bits);
    const std::uint32_t got = aot::fp_double_to_single_bits(bits);
    assert(csr_flags() == 0);
    if (std::isnan(d)) {
      assert((got & 0x7F800000u) == 0x7F800000u && (got & 0x7FFFFFu) != 0);
      continue;
    }
    const float ref = static_cast<float>(d);  // hardware round-to-nearest-even
    discard.fp_sync();
    assert(got == std::bit_cast<std::uint32_t>(ref));
    ++compared;
  }
  assert(compared > 300000);
  for (int i = 0; i < 400000; ++i) {
    const std::uint32_t w = static_cast<std::uint32_t>(rng());
    const float f = std::bit_cast<float>(w);
    const std::uint64_t got = aot::fp_single_to_double_bits(w);
    assert(csr_flags() == 0);
    if (std::isnan(f)) {
      assert(((got >> 52) & 0x7FFu) == 0x7FFu && (got & 0xFFFFFFFFFFFFFull) != 0);
      assert(aot::fp_double_to_single_bits(got) == w && "payload and quiet bit round-trip");
      continue;
    }
    assert(got == std::bit_cast<std::uint64_t>(static_cast<double>(f)));
    discard.fp_sync();
  }
  // Rounding boundaries by hand.
  assert(aot::fp_double_to_single_bits(std::bit_cast<std::uint64_t>(0x1p-149)) == 1u);
  assert(aot::fp_double_to_single_bits(std::bit_cast<std::uint64_t>(0x1p-150)) == 0u);   // tie to even
  assert(aot::fp_double_to_single_bits(std::bit_cast<std::uint64_t>(0x1.8p-150)) == 1u);
  assert(aot::fp_double_to_single_bits(std::bit_cast<std::uint64_t>(0x1p128)) == 0x7F800000u);
  assert(aot::fp_double_to_single_bits(std::bit_cast<std::uint64_t>(-1e300)) == 0xFF800000u);
  assert(aot::fp_double_to_single_bits(std::bit_cast<std::uint64_t>(-0.0)) == 0x80000000u);
  assert(aot::fp_demote_f64(1.5) == 1.5f && aot::fp_promote_f32(-2.25f) == -2.25);
  // A signalling single NaN keeps its payload through lfs/stfs.
  assert(aot::fp_single_to_double_bits(0x7F800001u) == 0x7FF0000020000000ull);
  assert(aot::fp_double_to_single_bits(0x7FF0000020000000ull) == 0x7F800001u);
}

void test_fprf_classification() {
  const double samples[] = {kQnan, kInf, -kInf, 1.0, -1.0, 0.0, -0.0,
                            std::numeric_limits<double>::denorm_min(),
                            -std::numeric_limits<double>::denorm_min()};
  for (const double d : samples)
    assert(fprf_class_of_bits(std::bit_cast<std::uint64_t>(d)) == aot::fprf_for(d));
}

// Informational: cost of a guest add + status update per tier.
void benchmark() {
  CpuState s = fresh();
  constexpr int kN = 20'000'000;
  const auto run = [&](const char* name) {
    double acc = 1.0, step = 1.0000001;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kN; ++i) {
      acc = aot::fp_mul(s, acc, step);
      aot::update_fpscr(s, acc);
      if (acc > 1e100) acc = 1.0;
    }
    const auto t1 = std::chrono::steady_clock::now();
    std::printf("  %-28s %.2f ns/op (acc=%g)\n", name,
                std::chrono::duration<double, std::nano>(t1 - t0).count() / kN, acc);
    s.fp_sync();
  };
  fp_legacy_status = true;  run("legacy (no status)");
  fp_legacy_status = false; run("tier A (lazy exact)");
  s.fpscr = fb::XE;         run("tier B (enable set)");
  s.fpscr = 0;
  aot::fp_strict_mode = true; run("strict");
  aot::fp_strict_mode = false;
}

}  // namespace

int main() {
  test_tier_a_status();
  test_tier_a_invalid();
  test_observation_points();
  test_tier_b();
  test_kill_switch();
  test_host_boundary();
  test_integer_conversions();
  test_fprf_classification();
  benchmark();
  std::puts("fp_lazy_status: ok");
  return 0;
}

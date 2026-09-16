#include <cassert>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <string_view>

#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/state.hpp"
#include "xenon/cpu/lifter.hpp"
#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/flat_memory.hpp"
#include "xenon/cpu/optimizer.hpp"
#include "xenon/cpu/aot_semantics.hpp"

using namespace xenon::cpu;

int main() {
  CpuState s{};
  assert(s.xer == 0);
  s.set_xer_ca(true);
  s.set_xer_overflow(true);
  assert(s.xer_ca() && s.xer_ov() && s.xer_so());
  s.set_xer_overflow(false);
  assert(!s.xer_ov() && s.xer_so()); // SO is sticky.
  s.set_xer_byte_count(0x55);
  assert(s.xer_byte_count() == 0x55);

  s.set_cr_field(0, 0xA);
  s.set_cr_field(7, 0x5);
  assert(s.cr_field(0) == 0xA && s.cr_field(7) == 0x5);
  assert((s.cr >> 28) == 0xA && (s.cr & 0xF) == 0x5);

  s.vscr = vscr_bits::NJ;
  s.set_vector_saturated();
  assert(s.vector_non_java() && s.vector_saturated());
  s.clear_vector_saturated();
  assert(s.vector_non_java() && !s.vector_saturated());

  const auto cat = Decoder::opcode_catalog();
  assert(cat.size() == 455);
  Decoder d;
  std::size_t mismatch = 0;
  for (const auto& op : cat) {
    auto got = d.decode(0x82000000u, op.pattern);
    if (!got.valid() || got.mnemonic() != op.mnemonic) {
      std::cerr << "decode mismatch pattern=0x" << std::hex << op.pattern
                << " expected=" << op.mnemonic << " got="
                << (got.valid() ? got.mnemonic() : std::string_view{"invalid"}) << '\n';
      ++mismatch;
    }
  }
  if (mismatch) {
    std::cerr << std::dec << mismatch << " canonical patterns failed self-decode\n";
    return 2;
  }

  // Fuzz only operand/unmasked bits. This catches masks that accidentally
  // freeze operand fields or leave opcode-extension bits unconstrained.
  std::mt19937 rng(0x58656E6Fu);
  for (const auto& op : cat) {
    for (unsigned n = 0; n < 64; ++n) {
      const std::uint32_t varied = (op.pattern & op.mask) | (rng() & ~op.mask);
      auto got = d.decode(0x82000000u, varied);
      if (!got.valid() || got.mnemonic() != op.mnemonic) {
        std::cerr << "operand-bit decode mismatch pattern=0x" << std::hex << op.pattern
                  << " varied=0x" << varied << " expected=" << op.mnemonic
                  << " got=" << (got.valid() ? got.mnemonic() : std::string_view{"invalid"})
                  << '\n';
        return 3;
      }
    }
  }

  // Spot checks.
  assert(d.decode(0x82000000u, 0x3860000Au).mnemonic() == "addi");
  assert(d.decode(0x82000000u, 0x80640000u).mnemonic() == "lwz");
  assert(d.decode(0x82000000u, 0x14000010u).mnemonic() == "vaddfp128");

  Lifter lifter;
  std::size_t lifted = 0;
  for (const auto& op : cat) {
    auto di = d.decode(0x82000000u, op.pattern);
    ir::Block block{0x82000000u,{}};
    ir::Builder builder(block);
    if (!lifter.lift(di,builder) || block.instructions.empty()) {
      std::cerr << "canonical lift failed: " << op.mnemonic << "\n";
      return 4;
    }
    ++lifted;

    // Also lift operand-bit variants to prove field values don't route into an
    // unimplemented path. This is compiler-time validation, not execution.
    for (unsigned n=0;n<16;++n) {
      const std::uint32_t varied=(op.pattern&op.mask)|(rng()&~op.mask);
      auto vi=d.decode(0x82001000u,varied);
      assert(vi.valid() && vi.mnemonic()==op.mnemonic);
      ir::Block vb{0x82001000u,{}}; ir::Builder vbuilder(vb);
      if (!lifter.lift(vi,vbuilder) || vb.instructions.empty()) {
        std::cerr << "operand-variant lift failed: " << op.mnemonic
                  << " word=0x" << std::hex << varied << "\n";
        return 5;
      }
    }
  }


  // Rc lives in different bit positions for normal X/VA forms, standard VMX
  // compare (VC), and Xbox VMX128 compare (VX128_R).
  {
    const auto* vc_info = [&]() -> const OpcodeInfo* { for (const auto& op:cat) if(op.mnemonic=="vcmpequw") return &op; return nullptr; }();
    const auto* vx128r_info = [&]() -> const OpcodeInfo* { for (const auto& op:cat) if(op.mnemonic=="vcmpequw128") return &op; return nullptr; }();
    assert(vc_info && vx128r_info);
    auto vc_rc=d.decode(0x82000000u,vc_info->pattern | (1u<<10));
    auto vx_rc=d.decode(0x82000000u,vx128r_info->pattern | (1u<<6));
    assert(vc_rc.valid() && vc_rc.rc());
    assert(vx_rc.valid() && vx_rc.rc());
  }

  // Tiny CPU-facing memory backend: endian access and reservation invalidation.
  {
    FlatMemory mem(0x1000u,0x100u);
    mem.write32_be(0x1010u,0x11223344u);
    assert(mem.read8(0x1010u)==0x11u);
    assert(mem.read32_be(0x1010u)==0x11223344u);
    assert(mem.read32_le(0x1010u)==0x44332211u);
    std::uint32_t observed{};
    auto token=mem.reserve32(0x1010u,observed);
    assert(observed==0x11223344u);
    assert(mem.store_conditional32(0x1010u,token,0xAABBCCDDu));
    token=mem.reserve32(0x1010u,observed);
    mem.write8(0x1020u,0x5Au); // conservative FlatMemory generation invalidates reservations.
    assert(!mem.store_conditional32(0x1010u,token,0x12345678u));
  }

  // A few architecture-state vector semantics independent of the host SIMD ISA.
  {
    CpuState vs{};
    Vector128 a{},b{};
    a.set_u32_be(0,0xFFFFFFFFu); b.set_u32_be(0,1u);
    auto carry=aot::execute_vector(aot::VectorSemantic::vaddcuw,0,vs,a,b);
    assert(carry.u32_be(0)==1u);
    auto noborrow=aot::execute_vector(aot::VectorSemantic::vsubcuw,0,vs,b,a);
    assert(noborrow.u32_be(0)==0u);
    Vector128 sat_a{},sat_b{}; sat_a.bytes[0]=0x7Fu; sat_b.bytes[0]=1u;
    (void)aot::execute_vector(aot::VectorSemantic::vaddsbs,0,vs,sat_a,sat_b);
    assert(vs.vector_saturated());
  }

  // FPSCR is full architectural state, including derived summaries. Guest FP
  // operations must also restore the host floating-point environment.
  {
    CpuState fp{};
    fp.fpscr = fpscr_bits::OX | fpscr_bits::OE;
    aot::recompute_fpscr_summaries(fp);
    assert((fp.fpscr & fpscr_bits::FEX) != 0);

    fp.fpscr = fpscr_bits::VXSNAN | fpscr_bits::VE;
    aot::recompute_fpscr_summaries(fp);
    assert((fp.fpscr & (fpscr_bits::VX | fpscr_bits::FEX)) ==
           (fpscr_bits::VX | fpscr_bits::FEX));

    // FPRF class/sign encoding covers every architected result class.
    assert(aot::fprf_for(std::numeric_limits<double>::quiet_NaN()) == 0x11u);
    assert(aot::fprf_for(-std::numeric_limits<double>::infinity()) == 0x09u);
    assert(aot::fprf_for(-1.0) == 0x08u);
    assert(aot::fprf_for(-std::numeric_limits<double>::denorm_min()) == 0x18u);
    assert(aot::fprf_for(-0.0) == 0x12u);
    assert(aot::fprf_for(+0.0) == 0x02u);
    assert(aot::fprf_for(std::numeric_limits<double>::denorm_min()) == 0x14u);
    assert(aot::fprf_for(1.0) == 0x04u);
    assert(aot::fprf_for(std::numeric_limits<double>::infinity()) == 0x05u);

    // Summary bits FEX and VX cannot be directly modified by mtfsb-style writes.
    const auto before = fp.fpscr;
    aot::set_fpscr_bit(fp, 1, false); // FEX
    aot::set_fpscr_bit(fp, 2, false); // VX
    assert(fp.fpscr == before);

    // mcrfs copies the selected field, clears its exception-status bits, then
    // recomputes the VX/FEX summaries.
    fp.fpscr = fpscr_bits::VXISI | fpscr_bits::VXIDI | fpscr_bits::VE;
    aot::recompute_fpscr_summaries(fp);
    assert((fp.fpscr & fpscr_bits::VX) != 0);
    aot::move_fpscr_field_to_cr(fp, 3, 2);
    assert((fp.fpscr & (fpscr_bits::VXISI | fpscr_bits::VXIDI)) == 0);
    assert((fp.fpscr & (fpscr_bits::VX | fpscr_bits::FEX)) == 0);

    // Sign/move helpers are bit operations: SNaN payloads are preserved and
    // no host floating-point exception is raised just by copying/changing sign.
    const double raw_snan=std::bit_cast<double>(0x7FF0000000001234ull);
    assert(std::bit_cast<std::uint64_t>(aot::fp_abs_bits(raw_snan))==0x7FF0000000001234ull);
    assert(std::bit_cast<std::uint64_t>(aot::fp_neg_bits(raw_snan))==0xFFF0000000001234ull);
    assert(aot::fp_select(-0.0,11.0,22.0)==11.0);
    assert(aot::fp_select(std::numeric_limits<double>::quiet_NaN(),11.0,22.0)==22.0);

    // Guest rounding mode is temporary and must not contaminate the host thread.
    const int host_round = std::fegetround();
    std::fesetround(FE_DOWNWARD);
    fp.fpscr = static_cast<std::uint32_t>(FpRoundingMode::TowardPositive);
    (void)aot::fp_add(fp, 1.0, 0x1p-53);
    aot::update_fpscr(fp, 1.0);
    assert(std::fegetround() == FE_DOWNWARD);
    std::fesetround(host_round);

    // Divide by zero and negative square root produce architected sticky state.
    fp.fpscr = 0;
    const double dz = aot::fp_div(fp, 1.0, 0.0);
    aot::update_fpscr(fp, dz);
    assert((fp.fpscr & (fpscr_bits::ZX | fpscr_bits::FX)) ==
           (fpscr_bits::ZX | fpscr_bits::FX));

    fp.fpscr = 0;
    const double sq = aot::fp_sqrt(fp, -1.0);
    aot::update_fpscr(fp, sq);
    assert((fp.fpscr & (fpscr_bits::VXSQRT | fpscr_bits::VX | fpscr_bits::FX)) ==
           (fpscr_bits::VXSQRT | fpscr_bits::VX | fpscr_bits::FX));

    // fcmpu and fcmpo differ for quiet/signalling NaNs.
    const double qnan = std::numeric_limits<double>::quiet_NaN();
    const double snan = std::bit_cast<double>(0x7FF0000000000001ull);
    fp.fpscr = 0;
    aot::update_fp_compare_status(fp, qnan, 1.0, false);
    assert((fp.fpscr & (fpscr_bits::VXSNAN | fpscr_bits::VXVC | fpscr_bits::VX)) == 0);
    assert(((fp.fpscr & fpscr_bits::FPRF_MASK) >> 12) == 1u);

    fp.fpscr = 0;
    aot::update_fp_compare_status(fp, snan, 1.0, false);
    assert((fp.fpscr & (fpscr_bits::VXSNAN | fpscr_bits::VX | fpscr_bits::FX)) ==
           (fpscr_bits::VXSNAN | fpscr_bits::VX | fpscr_bits::FX));
    assert((fp.fpscr & fpscr_bits::VXVC) == 0);

    fp.fpscr = 0;
    aot::update_fp_compare_status(fp, qnan, 1.0, true);
    assert((fp.fpscr & (fpscr_bits::VXVC | fpscr_bits::VX | fpscr_bits::FX)) ==
           (fpscr_bits::VXVC | fpscr_bits::VX | fpscr_bits::FX));

    fp.fpscr = fpscr_bits::VE;
    aot::update_fp_compare_status(fp, snan, 1.0, true);
    assert((fp.fpscr & fpscr_bits::VXSNAN) != 0);
    assert((fp.fpscr & fpscr_bits::VXVC) == 0);
    assert((fp.fpscr & (fpscr_bits::VX | fpscr_bits::FEX)) ==
           (fpscr_bits::VX | fpscr_bits::FEX));

    // Floating-to-integer conversion tracks inexact, FR/FI and invalid conversion.
    fp.fpscr = static_cast<std::uint32_t>(FpRoundingMode::Nearest);
    const auto cv = aot::fp_to_integer_bits(fp, 1.75, 32, false);
    assert(static_cast<std::int64_t>(cv) == 2);
    aot::update_fp_conversion_status(fp);
    assert((fp.fpscr & (fpscr_bits::FI | fpscr_bits::FR | fpscr_bits::XX | fpscr_bits::FX)) ==
           (fpscr_bits::FI | fpscr_bits::FR | fpscr_bits::XX | fpscr_bits::FX));

    fp.fpscr = 0;
    (void)aot::fp_to_integer_bits(fp, qnan, 64, false);
    aot::update_fp_conversion_status(fp);
    assert((fp.fpscr & (fpscr_bits::VXCVI | fpscr_bits::VX | fpscr_bits::FX)) ==
           (fpscr_bits::VXCVI | fpscr_bits::VX | fpscr_bits::FX));
  }

  // Xbox-era PowerPC MSR writes retain full state while honoring the mtmsr[d] L field.
  {
    CpuState ms{};
    constexpr auto bit = aot::msr_arch_bit;
    const auto EE=bit(48), PR=bit(49), ME=bit(51), IR=bit(58), DR=bit(59), RI=bit(62);
    const auto HV=bit(3), SF=bit(0), LE=bit(63);

    ms.msr = HV | ME | SF;
    aot::write_msr(ms, EE | RI | PR | LE, true, true);
    // L=1 changes only EE and RI.
    assert((ms.msr & (EE | RI)) == (EE | RI));
    assert((ms.msr & (HV | ME | SF)) == (HV | ME | SF));
    assert((ms.msr & (PR | LE)) == 0);

    // mtmsr L=0 changes only the architectural low half, preserves ME/high half,
    // and POWER4+ PR=1 forces EE/IR/DR.
    ms.msr = HV | ME | SF;
    aot::write_msr(ms, PR | LE, false, false);
    assert((ms.msr & (HV | ME | SF)) == (HV | ME | SF));
    assert((ms.msr & (PR | EE | IR | DR | LE)) == (PR | EE | IR | DR | LE));

    // mtmsrd L=0 can update the high half, but HV and ME remain unchanged.
    ms.msr = HV | ME;
    aot::write_msr(ms, bit(1) | PR | RI, false, true);
    assert((ms.msr & HV) == HV);
    assert((ms.msr & ME) == ME);
    assert((ms.msr & SF) == SF); // SF = RS[0] | RS[1].
    assert((ms.msr & (PR | EE | IR | DR | RI)) == (PR | EE | IR | DR | RI));
  }

  // Integer edge cases used by XER/CR-producing instructions.
  {
    assert(aot::carry_out(~std::uint64_t{0}, 0, true));
    assert(aot::signed_add_overflow(0x7FFFFFFFFFFFFFFFull, 1, false));
    assert(!aot::signed_add_overflow(1, 2, false));
    assert(aot::div_overflow(0x8000000000000000ull, ~std::uint64_t{0}, 64, false));
    assert(aot::div_overflow(123, 0, 64, true));
    assert(aot::ppc_shift("slwx", 1, 32) == 0);
    assert(aot::ppc_shift_carry("srawix", 0xFFFFFFFFu, 1));
  }

  // VMX state tests: signed average rounding, sticky saturation, compare CR6,
  // and an Xbox VMX128 rotate/insert lane mapping.
  {
    CpuState vs{};
    Vector128 a{}, b{};
    a.bytes[0] = 0xFFu; // -1
    b.bytes[0] = 0xFEu; // -2
    auto avg = aot::execute_vector(aot::VectorSemantic::vavgsb, 0, vs, a, b);
    assert(static_cast<std::int8_t>(avg.bytes[0]) == -1); // (-1 + -2 + 1) >> 1

    Vector128 maxv{}, one{};
    maxv.bytes[0] = 0x7Fu; one.bytes[0] = 1u;
    auto sat = aot::execute_vector(aot::VectorSemantic::vaddsbs, 0, vs, maxv, one);
    assert(sat.bytes[0] == 0x7Fu && vs.vector_saturated());
    (void)aot::execute_vector(aot::VectorSemantic::vaddubm, 0, vs, maxv, one);
    assert(vs.vector_saturated()); // SAT is sticky.

    Vector128 all_true{}; all_true.bytes.fill(0xFFu);
    aot::update_cr6_from_vector_compare(vs, all_true);
    assert(vs.cr_field(6) == 0x8u);
    Vector128 all_false{};
    aot::update_cr6_from_vector_compare(vs, all_false);
    assert(vs.cr_field(6) == 0x2u);

    Vector128 old{}, incoming{};
    for (unsigned lane=0; lane<4; ++lane) {
      old.set_u32_be(lane, 0x100u + lane);
      incoming.set_u32_be(lane, 0x200u + lane);
    }
    // VX128_4: IMM bits at 16..20, z rotation at 6..7. Select lanes 0 and 2,
    // rotate incoming left by one lane.
    const std::uint32_t word = (0xAu << 16) | (1u << 6);
    auto inserted = aot::execute_vector(aot::VectorSemantic::vrlimi128, word, vs, old, incoming);
    assert(inserted.u32_be(0) == incoming.u32_be(1));
    assert(inserted.u32_be(1) == old.u32_be(1));
    assert(inserted.u32_be(2) == incoming.u32_be(3));
    assert(inserted.u32_be(3) == old.u32_be(3));

    // Bounds compare defines NaN lanes as both out-of-bounds bits set.
    Vector128 fp_a{}, fp_b{};
    fp_a.set_u32_be(0, std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN()));
    fp_b.set_u32_be(0, std::bit_cast<std::uint32_t>(1.0f));
    auto bounds = aot::execute_vector(aot::VectorSemantic::vcmpbfp, 0, vs, fp_a, fp_b);
    assert(bounds.u32_be(0) == 0xC0000000u);

    // Saturating vector conversion must set sticky SAT for negative unsigned,
    // out-of-range and NaN inputs.
    vs.vscr = 0;
    Vector128 cv{};
    cv.set_u32_be(0, std::bit_cast<std::uint32_t>(-1.0f));
    cv.set_u32_be(1, std::bit_cast<std::uint32_t>(std::numeric_limits<float>::infinity()));
    cv.set_u32_be(2, std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN()));
    auto ucv = aot::execute_vector(aot::VectorSemantic::vctuxs, 0, vs, cv);
    assert(ucv.u32_be(0) == 0u);
    assert(ucv.u32_be(1) == 0xFFFFFFFFu);
    assert(ucv.u32_be(2) == 0u);
    assert(vs.vector_saturated());

    // vrfin is round-to-nearest-even and independent of the host's active mode.
    const int old_round=std::fegetround(); std::fesetround(FE_UPWARD);
    Vector128 rn{}; rn.set_u32_be(0,std::bit_cast<std::uint32_t>(2.5f));
    auto rounded=aot::execute_vector(aot::VectorSemantic::vrfin,0,vs,rn);
    assert(std::bit_cast<float>(rounded.u32_be(0))==2.0f);
    assert(std::fegetround()==FE_UPWARD);
    std::fesetround(old_round);
  }

  // Every canonical opcode must reach the portable native-AOT backend. This is
  // stronger than lift coverage: an IR operation without host lowering fails here.
  backend::CppAotBackend cpp_backend;
  std::size_t emitted=0;
  for(const auto& op:cat){
    auto di=d.decode(0x83000000u,op.pattern);
    ir::Block block{0x83000000u,{}}; ir::Builder bb(block);
    if(!lifter.lift(di,bb)){ std::cerr<<"backend pre-lift failed: "<<op.mnemonic<<"\n"; return 6; }
    try {
      auto source=cpp_backend.emit_translation_unit(block,"test_"+std::to_string(emitted));
      if(source.empty()){std::cerr<<"empty AOT source: "<<op.mnemonic<<"\n";return 7;}
    } catch(const std::exception& e){
      std::cerr<<"AOT backend coverage failed: "<<op.mnemonic<<": "<<e.what()<<"\n";
      return 8;
    }
    ++emitted;
  }

  std::cout << "xenon_cpu_tests: ok (" << cat.size() << " canonical opcodes, "
            << lifted << " strict lowerings, " << emitted << " AOT lowerings)\n";
  return 0;
}

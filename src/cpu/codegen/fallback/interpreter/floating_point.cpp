#include "cpu/codegen/fallback/fallback_interpreter.hpp"

namespace xenon::cpu::fallback {

// Scalar floating point (shared with the AOT semantic helpers).
[[nodiscard]] bool execute_floating_point(const DecodedInstruction& i,
                                          ExecutionContext& context) {
  auto& state = context.state;
  auto& mem = context.memory_access;
  const auto m = i.mnemonic();

  // Real AC6 repro: past the mulli fix above, the very next unsupported
  // instruction was "lfs" - this interpreter had NO floating-point support
  // at all (no loads/stores, no arithmetic), unlike the AOT-compiled path,
  // which lowers every one of these through aot_semantics.hpp's fp_add/
  // fp_sub/fp_mul/fp_div/fp_sqrt/fp_fma/update_fpscr/etc. Reusing those same
  // free functions here (rather than reimplementing FPSCR/exception
  // semantics a second time) keeps this interpreter bit-for-bit consistent
  // with the compiled path - the same guarantee dynamic_fallback_semantic_
  // tests already checks for the integer ops above.
  {
    const auto f64_at = [&](unsigned r) noexcept {
      return std::bit_cast<double>(state.fpr_bits[r]);
    };
    const auto store_f64 = [&](unsigned r, double v) noexcept {
      state.fpr_bits[r] = std::bit_cast<std::uint64_t>(v);
    };
    const auto update_cr1_if_rc = [&]() noexcept {
      if (i.rc()) state.update_cr1_from_fpscr();
    };

    if (m == "lfd" || m == "lfdu" || m == "lfdx" || m == "lfdux" || m == "lfs" ||
        m == "lfsu" || m == "lfsx" || m == "lfsux") {
      const bool single = m.starts_with("lfs");
      const ScalarAccess access{single ? 4u : 8u, false,
                                m == "lfdu" || m == "lfdux" || m == "lfsu" || m == "lfsux",
                                m.ends_with("x"), false};
      const auto ea = effective_address(i, state, access);
      if (single) {
        const auto raw = static_cast<std::uint32_t>(load_scalar(mem, ea, access));
        store_f64(i.frt(), static_cast<double>(std::bit_cast<float>(raw)));
      } else {
        state.fpr_bits[i.frt()] = load_scalar(mem, ea, access);
      }
      if (access.update) state.gpr[i.ra()] = ea;
      return true;
    }
    if (m == "stfd" || m == "stfdu" || m == "stfdx" || m == "stfdux" || m == "stfs" ||
        m == "stfsu" || m == "stfsx" || m == "stfsux") {
      const bool single = m.starts_with("stfs");
      const ScalarAccess access{single ? 4u : 8u, false,
                                m == "stfdu" || m == "stfdux" || m == "stfsu" || m == "stfsux",
                                m.ends_with("x"), false};
      const auto ea = effective_address(i, state, access);
      if (single) {
        const auto raw = std::bit_cast<std::uint32_t>(static_cast<float>(f64_at(i.frs())));
        store_scalar(mem, ea, access, raw);
      } else {
        store_scalar(mem, ea, access, state.fpr_bits[i.frs()]);
      }
      if (access.update) state.gpr[i.ra()] = ea;
      return true;
    }
    if (m == "fmrx" || m == "fnegx" || m == "fabsx" || m == "fnabsx") {
      auto v = f64_at(i.rb());
      if (m == "fabsx" || m == "fnabsx") v = aot::fp_abs_bits(v);
      if (m == "fnegx" || m == "fnabsx") v = aot::fp_neg_bits(v);
      store_f64(i.frt(), v);
      update_cr1_if_rc();
      return true;
    }
    if (m == "faddx" || m == "faddsx" || m == "fsubx" || m == "fsubsx" ||
        m == "fdivx" || m == "fdivsx") {
      const bool single = m.ends_with("sx");
      const auto a = f64_at(i.ra());
      const auto c = f64_at(i.rb());
      auto r = m.starts_with("fadd") ? aot::fp_add(state, a, c)
                                     : m.starts_with("fsub") ? aot::fp_sub(state, a, c)
                                                              : aot::fp_div(state, a, c);
      if (single) r = aot::fp_round_single(state, r);
      store_f64(i.frt(), r);
      aot::update_fpscr(state, r);
      update_cr1_if_rc();
      return true;
    }
    if (m == "fmulx" || m == "fmulsx") {
      const bool single = m == "fmulsx";
      const auto a = f64_at(i.ra());
      const auto c = f64_at(i.frc());
      auto r = aot::fp_mul(state, a, c);
      if (single) r = aot::fp_round_single(state, r);
      store_f64(i.frt(), r);
      aot::update_fpscr(state, r);
      update_cr1_if_rc();
      return true;
    }
    if (m == "fmaddx" || m == "fmaddsx" || m == "fmsubx" || m == "fmsubsx" ||
        m == "fnmaddx" || m == "fnmaddsx" || m == "fnmsubx" || m == "fnmsubsx") {
      const bool single = m.ends_with("sx");
      const bool subtract = m.starts_with("fmsub") || m.starts_with("fnmsub");
      const bool negate = m.starts_with("fnm");
      const auto a = f64_at(i.ra());
      const auto c = f64_at(i.frc());
      const auto addend = f64_at(i.rb());
      auto r = aot::fp_fma(state, a, c, subtract ? -addend : addend);
      if (negate) r = -r;
      if (single) r = aot::fp_round_single(state, r);
      store_f64(i.frt(), r);
      aot::update_fpscr(state, r);
      update_cr1_if_rc();
      return true;
    }
    if (m == "fsqrtx" || m == "fsqrtsx") {
      const bool single = m == "fsqrtsx";
      auto r = aot::fp_sqrt(state, f64_at(i.rb()));
      if (single) r = aot::fp_round_single(state, r);
      store_f64(i.frt(), r);
      aot::update_fpscr(state, r);
      update_cr1_if_rc();
      return true;
    }
    if (m == "fresx" || m == "frsqrtex") {
      const auto a = f64_at(i.rb());
      const auto r = m == "fresx" ? aot::fp_reciprocal(state, a) : aot::fp_rsqrt(state, a);
      store_f64(i.frt(), r);
      aot::update_fpscr(state, r);
      update_cr1_if_rc();
      return true;
    }
    if (m == "fselx") {
      const auto r = aot::fp_select(f64_at(i.ra()), f64_at(i.frc()), f64_at(i.rb()));
      store_f64(i.frt(), r);
      update_cr1_if_rc();  // fsel does not alter FPSCR.
      return true;
    }
    if (m == "frspx") {
      const auto r = aot::fp_round_single(state, f64_at(i.rb()));
      store_f64(i.frt(), r);
      aot::update_fpscr(state, r);
      update_cr1_if_rc();
      return true;
    }
    if (m == "fcfidx") {
      const auto r = aot::fp_from_i64(state, state.fpr_bits[i.rb()]);
      store_f64(i.frt(), r);
      aot::update_fpscr(state, r);
      update_cr1_if_rc();
      return true;
    }
    if (m == "fctidx" || m == "fctidzx" || m == "fctiwx" || m == "fctiwzx") {
      const bool to_zero = m == "fctidzx" || m == "fctiwzx";
      const unsigned width = (m == "fctiwx" || m == "fctiwzx") ? 32u : 64u;
      state.fpr_bits[i.frt()] =
          aot::fp_to_integer_bits(state, f64_at(i.rb()), width, to_zero);
      aot::update_fp_conversion_status(state);
      update_cr1_if_rc();
      return true;
    }
    if (m == "fcmpu" || m == "fcmpo") {
      const auto a = f64_at(i.ra());
      const auto c = f64_at(i.rb());
      state.set_cr_field(i.crfd(), aot::fp_compare_field(a, c));
      aot::update_fp_compare_status(state, a, c, m == "fcmpo");
      return true;
    }
    if (m == "mffsx") {
      state.fpr_bits[i.frt()] = state.fpscr;
      update_cr1_if_rc();
      return true;
    }
    // FPSCR writes and the FPSCR-to-CR move use the compiled path's own helpers so
    // the two can never disagree about what a sticky/exception bit does.
    if (m == "mcrfs") {
      aot::move_fpscr_field_to_cr(state, i.crfd(), i.crfs());
      return true;
    }
    if (m == "mtfsb0x" || m == "mtfsb1x") {
      aot::set_fpscr_bit(state, i.rt(), m == "mtfsb1x");
      update_cr1_if_rc();
      return true;
    }
    if (m == "mtfsfix") {
      aot::write_fpscr_field(state, i.crfd(), static_cast<std::uint8_t>((i.word >> 12u) & 0xFu));
      update_cr1_if_rc();
      return true;
    }
    if (m == "mtfsfx") {
      aot::write_fpscr_fields(state, static_cast<std::uint8_t>((i.word >> 17u) & 0xFFu),
                              static_cast<std::uint32_t>(state.fpr_bits[i.rb()]));
      update_cr1_if_rc();
      return true;
    }
    // stfiwx: store the low word of FRS, unconverted.
    if (m == "stfiwx") {
      const ScalarAccess access{4u, false, false, true, false};
      mem.write32_be(effective_address(i, state, access),
                     static_cast<std::uint32_t>(state.fpr_bits[i.frs()]));
      return true;
    }
  }
  return false;
}

}  // namespace xenon::cpu::fallback

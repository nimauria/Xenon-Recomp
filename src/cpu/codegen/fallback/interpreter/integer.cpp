#include "cpu/codegen/fallback/fallback_interpreter.hpp"

namespace xenon::cpu::fallback {

// Integer arithmetic, logical, multiply/divide, extend, compare, rotate, shift, count.
[[nodiscard]] bool execute_integer(const DecodedInstruction& i,
                                   ExecutionContext& context) {
  auto& state = context.state;
  const auto m = i.mnemonic();

  if (m == "addi" || m == "addis") {
    const auto base = i.ra() ? state.gpr[i.ra()] : 0u;
    std::int64_t imm = i.simm16();
    if (m == "addis") imm <<= 16;
    state.gpr[i.rt()] = base + static_cast<std::uint64_t>(imm);
    return true;
  }

  // Real AC6 repro: past the FPU fix above, the next unsupported instruction
  // was "addic." (this codebase's internal "addicx" name for the Rc-recording
  // form) - unlike "addi"/"addis" above, addic/addic. also set XER CA from
  // the addition's carry-out, which this interpreter had no case for at all.
  if (m == "addic" || m == "addicx") {
    const auto a = state.gpr[i.ra()];
    const auto c = static_cast<std::uint64_t>(static_cast<std::int64_t>(i.simm16()));
    const auto value = a + c;
    state.gpr[i.rt()] = value;
    // carry = sum <u a (valid for two-input unsigned addition) - matches
    // lifter_integer.cpp's emit_ca_from_add() exactly.
    state.set_xer_ca(value < a);
    if (m == "addicx") state.update_cr0_signed(value);
    return true;
  }

  if (m == "ori" || m == "oris" || m == "xori" || m == "xoris" ||
      m == "andix" || m == "andisx") {
    std::uint64_t imm = i.uimm16();
    if (m == "oris" || m == "xoris" || m == "andisx") imm <<= 16u;
    const auto src = state.gpr[i.rs()];
    std::uint64_t value = 0u;
    if (m == "ori" || m == "oris") value = src | imm;
    else if (m == "xori" || m == "xoris") value = src ^ imm;
    else value = src & imm;
    state.gpr[i.ra()] = value;
    if (m == "andix" || m == "andisx") state.update_cr0_signed(value);
    return true;
  }

  if (m == "addx" || m == "subfx") {
    const auto a = state.gpr[i.ra()];
    const auto b = state.gpr[i.rb()];
    const auto value = m == "addx" ? a + b : b - a;
    state.gpr[i.rt()] = value;
    if (i.oe()) {
      // Same overflow rules as the compiled path (lifter_integer.cpp).
      state.set_xer_overflow(m == "addx" ? aot::signed_add_overflow(a, b)
                                         : aot::signed_add_overflow(b, ~a, true));
    }
    if (i.rc()) state.update_cr0_signed(value);
    return true;
  }

  // Carry arithmetic: addc/adde/addme/addze and subfc/subfe/subfme/subfze/subfic.
  // XER.CA is the 64-bit carry out; the "extended" forms also add the incoming CA.
  // Identical semantics to lifter_integer.cpp (the compiled path) - this used to
  // be missing here, so any undiscovered function using them ended the run.
  if (m == "addcx" || m == "addex" || m == "addmex" || m == "addzex") {
    const auto a = state.gpr[i.ra()];
    const bool extended = m != "addcx";
    const bool carry_in = extended && state.xer_ca();
    const std::uint64_t b = m == "addcx" || m == "addex" ? state.gpr[i.rb()]
                            : m == "addmex"               ? ~std::uint64_t{0}
                                                          : std::uint64_t{0};
    const auto value = aot::add_carry(a, b, carry_in);
    state.gpr[i.rt()] = value;
    state.set_xer_ca(aot::carry_out(a, b, carry_in));
    if (i.oe()) state.set_xer_overflow(aot::signed_add_overflow(a, b, carry_in));
    if (i.rc()) state.update_cr0_signed(value);
    return true;
  }

  if (m == "subfcx") {
    const auto ra = state.gpr[i.ra()];
    const auto rb = state.gpr[i.rb()];
    const auto value = rb - ra;
    state.gpr[i.rt()] = value;
    state.set_xer_ca(rb >= ra);  // no borrow
    if (i.oe()) state.set_xer_overflow(aot::signed_add_overflow(rb, ~ra, true));
    if (i.rc()) state.update_cr0_signed(value);
    return true;
  }

  if (m == "subfex" || m == "subfmex" || m == "subfzex") {
    // subfe = rB + ~rA + CA; subfme = -1 + ~rA + CA; subfze = 0 + ~rA + CA.
    const auto not_ra = ~state.gpr[i.ra()];
    const bool carry_in = state.xer_ca();
    const std::uint64_t lhs = m == "subfex"    ? state.gpr[i.rb()]
                              : m == "subfmex" ? ~std::uint64_t{0}
                                               : std::uint64_t{0};
    const auto value = aot::add_carry(lhs, not_ra, carry_in);
    state.gpr[i.rt()] = value;
    state.set_xer_ca(aot::carry_out(lhs, not_ra, carry_in));
    if (i.oe()) state.set_xer_overflow(aot::signed_add_overflow(lhs, not_ra, carry_in));
    if (i.rc()) state.update_cr0_signed(value);
    return true;
  }

  if (m == "subficx") {
    const auto imm = static_cast<std::uint64_t>(static_cast<std::int64_t>(i.simm16()));
    const auto ra = state.gpr[i.ra()];
    state.gpr[i.rt()] = imm - ra;
    state.set_xer_ca(imm >= ra);
    return true;
  }

  if (m == "andx" || m == "andcx" || m == "orx" || m == "orcx" ||
      m == "xorx" || m == "eqvx" || m == "nandx" || m == "norx") {
    const auto a = state.gpr[i.rs()];
    const auto b = state.gpr[i.rb()];
    std::uint64_t value = 0u;
    if (m == "andx") value = a & b;
    else if (m == "andcx") value = a & ~b;
    else if (m == "orx") value = a | b;
    else if (m == "orcx") value = a | ~b;
    else if (m == "xorx") value = a ^ b;
    else if (m == "eqvx") value = ~(a ^ b);
    else if (m == "nandx") value = ~(a & b);
    else value = ~(a | b);
    state.gpr[i.ra()] = value;
    if (i.rc()) state.update_cr0_signed(value);
    return true;
  }

  if (m == "negx") {
    const auto value = std::uint64_t{0} - state.gpr[i.ra()];
    state.gpr[i.rt()] = value;
    if (i.rc()) state.update_cr0_signed(value);
    return true;
  }

  // Real AC6 repro: dynamic fallback hit "mulli" with no case here at all,
  // trapping with kFallbackUnsupportedDetail and killing guest execution a
  // few instructions after the very first guest thread's TLS setup - the
  // whole multiply/divide family was simply missing from this interpreter
  // (unlike the AOT-compiled path, which lowers them through the generic
  // Op::Mul/MulHigh*/Div* IR and so never needed a per-mnemonic case here).
  // XER SO/OV (the oe() bit) is intentionally not modeled, same as every
  // other oe()-capable op in this function (addx/subfx above) - a real,
  // pre-existing gap in this interpreter, not one introduced here.
  if (m == "mulli") {
    state.gpr[i.rt()] =
        state.gpr[i.ra()] *
        static_cast<std::uint64_t>(static_cast<std::int64_t>(i.simm16()));
    return true;
  }

  if (m == "mulldx" || m == "mullwx") {
    const unsigned width = m == "mullwx" ? 32u : 64u;
    auto a = state.gpr[i.ra()];
    auto c = state.gpr[i.rb()];
    if (width == 32) {
      a = static_cast<std::uint32_t>(a);
      c = static_cast<std::uint32_t>(c);
    }
    std::uint64_t value = a * c;
    if (width == 32)
      value = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
    state.gpr[i.rt()] = value;
    if (i.rc()) state.update_cr0_signed(value);
    return true;
  }

  if (m == "mulhdx" || m == "mulhdux" || m == "mulhwx" || m == "mulhwux") {
    const bool word = m == "mulhwx" || m == "mulhwux";
    const bool uns = m == "mulhdux" || m == "mulhwux";
    const unsigned width = word ? 32u : 64u;
    const auto a = state.gpr[i.ra()];
    const auto c = state.gpr[i.rb()];
    auto value = uns ? aot::mul_hi_unsigned_width(a, c, width)
                     : aot::mul_hi_signed_width(a, c, width);
    if (word)
      value = uns ? static_cast<std::uint64_t>(static_cast<std::uint32_t>(value))
                  : static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
    state.gpr[i.rt()] = value;
    if (i.rc()) state.update_cr0_signed(value);
    return true;
  }

  if (m == "divdx" || m == "divdux" || m == "divwx" || m == "divwux") {
    const bool word = m == "divwx" || m == "divwux";
    const bool uns = m == "divdux" || m == "divwux";
    const unsigned width = word ? 32u : 64u;
    const auto a = state.gpr[i.ra()];
    const auto c = state.gpr[i.rb()];
    auto value = uns ? aot::div_unsigned(a, c, width) : aot::div_signed(a, c, width);
    if (word)
      value = uns ? static_cast<std::uint64_t>(static_cast<std::uint32_t>(value))
                  : static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
    state.gpr[i.rt()] = value;
    if (i.rc()) state.update_cr0_signed(value);
    return true;
  }

  if (m == "extsbx" || m == "extshx" || m == "extswx") {
    const auto src = state.gpr[i.rs()];
    std::uint64_t value = 0u;
    if (m == "extsbx") value = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int8_t>(src)));
    else if (m == "extshx") value = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int16_t>(src)));
    else value = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(src)));
    state.gpr[i.ra()] = value;
    if (i.rc()) state.update_cr0_signed(value);
    return true;
  }

  if (m == "cmp" || m == "cmpl" || m == "cmpi" || m == "cmpli") {
    const bool logical = m == "cmpl" || m == "cmpli";
    const bool immediate = m == "cmpi" || m == "cmpli";
    const bool word = ((i.word >> 21u) & 1u) == 0u;
    const auto rhs = immediate
                         ? (logical ? std::uint64_t{i.uimm16()}
                                    : static_cast<std::uint64_t>(static_cast<std::int64_t>(i.simm16())))
                         : state.gpr[i.rb()];
    write_compare(state, i.crfd(), state.gpr[i.ra()], rhs, logical, word);
    return true;
  }

  if (m == "rlwinmx" || m == "rlwimix" || m == "rlwnmx") {
    const auto src = static_cast<std::uint32_t>(state.gpr[i.rs()]);
    const auto shift = m == "rlwnmx" ? static_cast<unsigned>(state.gpr[i.rb()] & 31u)
                                      : i.sh32();
    const auto rotated = std::rotl(src, static_cast<int>(shift));
    const auto mask = mask32(i.mb32(), i.me32());
    auto result = rotated & mask;
    if (m == "rlwimix")
      result = (static_cast<std::uint32_t>(state.gpr[i.ra()]) & ~mask) | result;
    state.gpr[i.ra()] = result;
    if (i.rc()) state.update_cr0_signed(result);
    return true;
  }

  // Doubleword rotate family (MD/MDS-form). The immediate forms take the shift
  // from sh64_md(); rldcl/rldcr take it from the low six bits of rB. The mask
  // field (mb64_md()) is MB for rldicl/rldic/rldimi/rldcl and ME for rldicr; rldic
  // and rldimi derive ME as 63 - SH.
  if (m == "rldiclx" || m == "rldicrx" || m == "rldicx" || m == "rldimix" ||
      m == "rldclx" || m == "rldcrx") {
    const bool by_register = m == "rldclx" || m == "rldcrx";
    const auto shift = by_register ? static_cast<unsigned>(state.gpr[i.rb()] & 63u) : i.sh64_md();
    const auto rotated = std::rotl(state.gpr[i.rs()], static_cast<int>(shift));
    const unsigned field = i.mb64_md();
    unsigned mb = field;
    unsigned me = 63u;
    if (m == "rldicrx" || m == "rldcrx") {
      mb = 0u;
      me = field;
    } else if (m == "rldicx" || m == "rldimix") {
      me = 63u - shift;
    }
    const auto mask = mask64(mb, me);
    auto result = rotated & mask;
    if (m == "rldimix") result = (state.gpr[i.ra()] & ~mask) | result;
    state.gpr[i.ra()] = result;
    if (i.rc()) state.update_cr0_signed(result);
    return true;
  }

  if (m == "slwx" || m == "srwx" || m == "srawx" || m == "sldx" ||
      m == "srdx" || m == "sradx") {
    const auto value = aot::ppc_shift(m, state.gpr[i.rs()], state.gpr[i.rb()]);
    state.gpr[i.ra()] = value;
    if (m == "srawx" || m == "sradx")
      state.set_xer_ca(aot::ppc_shift_carry(m, state.gpr[i.rs()], state.gpr[i.rb()]));
    if (i.rc()) state.update_cr0_signed(value);
    return true;
  }

  if (m == "srawix" || m == "sradix") {
    const auto shift = m == "srawix" ? i.sh32() : i.sh64_md();
    const auto value = aot::ppc_shift(m, state.gpr[i.rs()], shift);
    state.gpr[i.ra()] = value;
    state.set_xer_ca(aot::ppc_shift_carry(m, state.gpr[i.rs()], shift));
    if (i.rc()) state.update_cr0_signed(value);
    return true;
  }

  if (m == "cntlzwx" || m == "cntlzdx") {
    const auto src = state.gpr[i.rs()];
    const auto value = m == "cntlzwx"
                           ? static_cast<std::uint64_t>(
                                 std::countl_zero(static_cast<std::uint32_t>(src)))
                           : static_cast<std::uint64_t>(std::countl_zero(src));
    state.gpr[i.ra()] = value;
    if (i.rc()) state.update_cr0_signed(value);
    return true;
  }
  return false;
}

}  // namespace xenon::cpu::fallback

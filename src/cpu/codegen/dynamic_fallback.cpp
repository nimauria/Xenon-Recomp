#include "xenon/cpu/dynamic_fallback.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_map>

#include "xenon/cpu/aot_semantics.hpp"
#include "xenon/cpu/vector_semantic.hpp"

namespace xenon::cpu {
namespace {

constexpr std::uint32_t kGuestPageSize = 0x1000u;
constexpr GuestAddress kGuestPageMask = ~(kGuestPageSize - 1u);
constexpr std::uint32_t kFallbackUnsupportedDetail = 0x58470001u;  // "XG" Gen7
constexpr std::uint32_t kFallbackInvalidTargetDetail = 0x58470002u;
constexpr std::uint32_t kFallbackSourceChangedDetail = 0x58470003u;
constexpr std::uint32_t kFallbackInstructionLimitDetail = 0x58470004u;
constexpr std::uint32_t kFallbackCallDepthDetail = 0x58470005u;
constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

[[nodiscard]] std::uint64_t fingerprint_step(std::uint64_t hash,
                                             std::uint32_t word) noexcept {
  // Keep the first runtime fingerprint address-independent. Gen 9 can add
  // deeper normalization of PC-relative immediates; even this form already
  // survives a block moving wholesale between executable revisions.
  hash ^= word;
  hash *= kFnvPrime;
  return hash;
}

[[nodiscard]] bool branch_condition(const DecodedInstruction& i,
                                    CpuState& state, bool use_ctr) noexcept {
  const auto bo = i.bo();
  bool ctr_ok = true;
  bool cond_ok = true;
  if (use_ctr && (bo & 0b00100u) == 0u) {
    state.ctr -= 1u;
    const bool want_zero = (bo & 0b00010u) != 0u;
    ctr_ok = want_zero ? state.ctr == 0u : state.ctr != 0u;
  }
  if ((bo & 0b10000u) == 0u) {
    const bool want_true = (bo & 0b01000u) != 0u;
    cond_ok = state.cr_bit(i.bi()) == want_true;
  }
  return ctr_ok && cond_ok;
}

void write_compare(CpuState& state, unsigned field, std::uint64_t lhs,
                   std::uint64_t rhs, bool logical, bool word) noexcept {
  bool lt = false;
  bool gt = false;
  if (word) {
    if (logical) {
      const auto a = static_cast<std::uint32_t>(lhs);
      const auto b = static_cast<std::uint32_t>(rhs);
      lt = a < b;
      gt = a > b;
    } else {
      const auto a = static_cast<std::int32_t>(lhs);
      const auto b = static_cast<std::int32_t>(rhs);
      lt = a < b;
      gt = a > b;
    }
  } else if (logical) {
    lt = lhs < rhs;
    gt = lhs > rhs;
  } else {
    const auto a = static_cast<std::int64_t>(lhs);
    const auto b = static_cast<std::int64_t>(rhs);
    lt = a < b;
    gt = a > b;
  }
  const std::uint8_t nibble = static_cast<std::uint8_t>(
      (lt ? 0x8u : gt ? 0x4u : 0x2u) | (state.xer_so() ? 0x1u : 0u));
  state.set_cr_field(field, nibble);
}

// The 64-bit rotate mask of the doubleword rotate family (rldicl/rldicr/rldic/
// rldimi/rldcl/rldcr): ones from architectural bit `mb` through `me` (bit 0 is
// the MOST significant bit), wrapping when mb > me. Identical to the compiled
// path's mask64 in the PPC lifter, so the fallback and AOT agree bit for bit.
[[nodiscard]] std::uint64_t mask64(unsigned mb, unsigned me) noexcept {
  mb &= 63u;
  me &= 63u;
  std::uint64_t mask = 0u;
  const auto set_arch_bit = [&mask](unsigned bit) {
    mask |= std::uint64_t{1} << (63u - bit);
  };
  if (mb <= me) {
    for (unsigned bit = mb; bit <= me; ++bit) set_arch_bit(bit);
  } else {
    for (unsigned bit = mb; bit < 64u; ++bit) set_arch_bit(bit);
    for (unsigned bit = 0u; bit <= me; ++bit) set_arch_bit(bit);
  }
  return mask;
}

[[nodiscard]] std::uint32_t mask32(unsigned mb, unsigned me) noexcept {
  mb &= 31u;
  me &= 31u;
  std::uint32_t mask = 0u;
  const auto set_arch_bit = [&mask](unsigned bit) {
    mask |= std::uint32_t{1} << (31u - bit);
  };
  if (mb <= me) {
    for (unsigned bit = mb; bit <= me; ++bit) set_arch_bit(bit);
  } else {
    for (unsigned bit = mb; bit < 32u; ++bit) set_arch_bit(bit);
    for (unsigned bit = 0u; bit <= me; ++bit) set_arch_bit(bit);
  }
  return mask;
}

// VMX/VMX128 register-field extraction. Xbox 360 extends classic 32-register
// VMX with a 128-register VX128 encoding family that packs the extra address
// bits into different word positions per sub-format; this mirrors
// lifter_vector.cpp's own vd()/va()/vb()/vc() exactly (same real hardware
// field layout, independently duplicated here since the interpreter has no
// IR::Builder to share that code through).
bool vector_extended_format(InstructionFormat f) noexcept {
  return f == InstructionFormat::VX128 || f == InstructionFormat::VX128_1 ||
        f == InstructionFormat::VX128_2 || f == InstructionFormat::VX128_3 ||
        f == InstructionFormat::VX128_4 || f == InstructionFormat::VX128_5 ||
        f == InstructionFormat::VX128_R || f == InstructionFormat::VX128_P;
}
unsigned vector_vd(const DecodedInstruction& i) noexcept {
  return vector_extended_format(i.info->format) ? i.vx128_vd() : i.vd5();
}
unsigned vector_va(const DecodedInstruction& i) noexcept {
  switch (i.info->format) {
    case InstructionFormat::VX128: case InstructionFormat::VX128_2:
    case InstructionFormat::VX128_5: case InstructionFormat::VX128_R:
      return i.vx128_va();
    default: return i.va5();
  }
}
unsigned vector_vb(const DecodedInstruction& i) noexcept {
  switch (i.info->format) {
    case InstructionFormat::VX128: case InstructionFormat::VX128_2:
    case InstructionFormat::VX128_3: case InstructionFormat::VX128_4:
    case InstructionFormat::VX128_5: case InstructionFormat::VX128_R:
    case InstructionFormat::VX128_P:
      return i.vx128_vb();
    default: return i.vb5();
  }
}
unsigned vector_vc(const DecodedInstruction& i) noexcept {
  if (i.info->format == InstructionFormat::VX128_2) return (i.word >> 8) & 7u;
  return i.vc5();
}

// Maps a decoded guest mnemonic to the aot::VectorSemantic enumerator of the
// exact same name (see vector_semantic.hpp) so the interpreter can reuse
// aot::execute_vector() - the same runtime-dispatched semantic function the
// AOT-compiled path calls for every vector op it does not special-case for
// performance (see backend_cpp_aot.cpp's is_vector_compute() catch-all) -
// instead of reimplementing VMX/VMX128 arithmetic a second time.
std::optional<aot::VectorSemantic> vector_semantic_for(std::string_view m) noexcept {
  static const std::unordered_map<std::string_view, aot::VectorSemantic> table = {
#define V(name) {#name, aot::VectorSemantic::name}
      V(vaddcuw), V(vaddfp), V(vaddfp128), V(vaddsbs), V(vaddshs), V(vaddsws),
      V(vaddubm), V(vaddubs), V(vadduhm), V(vadduhs), V(vadduwm), V(vadduws),
      V(vand), V(vand128), V(vandc), V(vandc128), V(vavgsb), V(vavgsh),
      V(vavgsw), V(vavgub), V(vavguh), V(vavguw), V(vcfpsxws128),
      V(vcfpuxws128), V(vcfsx), V(vcfux), V(vcmpbfp), V(vcmpbfp128),
      V(vcmpeqfp), V(vcmpeqfp128), V(vcmpequb), V(vcmpequh), V(vcmpequw),
      V(vcmpequw128), V(vcmpgefp), V(vcmpgefp128), V(vcmpgtfp),
      V(vcmpgtfp128), V(vcmpgtsb), V(vcmpgtsh), V(vcmpgtsw), V(vcmpgtub),
      V(vcmpgtuh), V(vcmpgtuw), V(vcsxwfp128), V(vctsxs), V(vctuxs),
      V(vcuxwfp128), V(vexptefp), V(vexptefp128), V(vlogefp), V(vlogefp128),
      V(vmaddcfp128), V(vmaddfp), V(vmaddfp128), V(vmaxfp), V(vmaxfp128),
      V(vmaxsb), V(vmaxsh), V(vmaxsw), V(vmaxub), V(vmaxuh), V(vmaxuw),
      V(vmhaddshs), V(vmhraddshs), V(vminfp), V(vminfp128), V(vminsb),
      V(vminsh), V(vminsw), V(vminub), V(vminuh), V(vminuw), V(vmladduhm),
      V(vmrghb), V(vmrghh), V(vmrghw), V(vmrghw128), V(vmrglb), V(vmrglh),
      V(vmrglw), V(vmrglw128), V(vmsum3fp128), V(vmsum4fp128), V(vmsummbm),
      V(vmsumshm), V(vmsumshs), V(vmsumubm), V(vmsumuhm), V(vmsumuhs),
      V(vmulesb), V(vmulesh), V(vmuleub), V(vmuleuh), V(vmulfp128),
      V(vmulosb), V(vmulosh), V(vmuloub), V(vmulouh), V(vnmsubfp),
      V(vnmsubfp128), V(vnor), V(vnor128), V(vor), V(vor128), V(vperm),
      V(vperm128), V(vpermwi128), V(vpkd3d128), V(vpkpx), V(vpkshss),
      V(vpkshss128), V(vpkshus), V(vpkshus128), V(vpkswss), V(vpkswss128),
      V(vpkswus), V(vpkswus128), V(vpkuhum), V(vpkuhum128), V(vpkuhus),
      V(vpkuhus128), V(vpkuwum), V(vpkuwum128), V(vpkuwus), V(vpkuwus128),
      V(vrefp), V(vrefp128), V(vrfim), V(vrfim128), V(vrfin), V(vrfin128),
      V(vrfip), V(vrfip128), V(vrfiz), V(vrfiz128), V(vrlb), V(vrlh),
      V(vrlimi128), V(vrlw), V(vrlw128), V(vrsqrtefp), V(vrsqrtefp128),
      V(vsel), V(vsel128), V(vsl), V(vslb), V(vsldoi), V(vsldoi128), V(vslh),
      V(vslo), V(vslo128), V(vslw), V(vslw128), V(vspltb), V(vsplth),
      V(vspltisb), V(vspltish), V(vspltisw), V(vspltisw128), V(vspltw),
      V(vspltw128), V(vsr), V(vsrab), V(vsrah), V(vsraw), V(vsraw128),
      V(vsrb), V(vsrh), V(vsro), V(vsro128), V(vsrw), V(vsrw128), V(vsubcuw),
      V(vsubfp), V(vsubfp128), V(vsubsbs), V(vsubshs), V(vsubsws),
      V(vsububm), V(vsububs), V(vsubuhm), V(vsubuhs), V(vsubuwm),
      V(vsubuws), V(vsum2sws), V(vsum4sbs), V(vsum4shs), V(vsum4ubs),
      V(vsumsws), V(vupkd3d128), V(vupkhpx), V(vupkhsb), V(vupkhsb128),
      V(vupkhsh), V(vupklpx), V(vupklsb), V(vupklsb128), V(vupklsh),
      V(vxor), V(vxor128),
#undef V
  };
  const auto it = table.find(m);
  return it != table.end() ? std::optional(it->second) : std::nullopt;
}

struct ScalarAccess {
  unsigned bytes{};
  bool sign{};
  bool update{};
  bool indexed{};
  bool little_endian{};
};

[[nodiscard]] std::optional<ScalarAccess> load_access(std::string_view m) {
#define X(N,B,S,U,I) if (m == N) return ScalarAccess{B,S,U,I,false}
  X("lbz",1,false,false,false); X("lbzu",1,false,true,false);
  X("lbzx",1,false,false,true); X("lbzux",1,false,true,true);
  X("lha",2,true,false,false); X("lhau",2,true,true,false);
  X("lhax",2,true,false,true); X("lhaux",2,true,true,true);
  X("lhz",2,false,false,false); X("lhzu",2,false,true,false);
  X("lhzx",2,false,false,true); X("lhzux",2,false,true,true);
  X("lwa",4,true,false,false); X("lwax",4,true,false,true);
  X("lwaux",4,true,true,true); X("lwz",4,false,false,false);
  X("lwzu",4,false,true,false); X("lwzx",4,false,false,true);
  X("lwzux",4,false,true,true); X("ld",8,false,false,false);
  X("ldu",8,false,true,false); X("ldx",8,false,false,true);
  X("ldux",8,false,true,true);
#undef X
  if (m == "lhbrx") return ScalarAccess{2,false,false,true,true};
  if (m == "lwbrx") return ScalarAccess{4,false,false,true,true};
  if (m == "ldbrx") return ScalarAccess{8,false,false,true,true};
  return std::nullopt;
}

[[nodiscard]] std::optional<ScalarAccess> store_access(std::string_view m) {
#define X(N,B,U,I) if (m == N) return ScalarAccess{B,false,U,I,false}
  X("stb",1,false,false); X("stbu",1,true,false);
  X("stbx",1,false,true); X("stbux",1,true,true);
  X("sth",2,false,false); X("sthu",2,true,false);
  X("sthx",2,false,true); X("sthux",2,true,true);
  X("stw",4,false,false); X("stwu",4,true,false);
  X("stwx",4,false,true); X("stwux",4,true,true);
  X("std",8,false,false); X("stdu",8,true,false);
  X("stdx",8,false,true); X("stdux",8,true,true);
#undef X
  if (m == "sthbrx") return ScalarAccess{2,false,false,true,true};
  if (m == "stwbrx") return ScalarAccess{4,false,false,true,true};
  if (m == "stdbrx") return ScalarAccess{8,false,false,true,true};
  return std::nullopt;
}

[[nodiscard]] GuestAddress effective_address(const DecodedInstruction& i,
                                             const CpuState& state,
                                             const ScalarAccess& access) noexcept {
  std::uint64_t base = 0u;
  if (access.indexed) {
    if (access.update || i.ra() != 0u) base = state.gpr[i.ra()];
    return static_cast<GuestAddress>(base + state.gpr[i.rb()]);
  }
  if (access.update || i.ra() != 0u) base = state.gpr[i.ra()];
  const auto displacement = i.info && i.info->format == InstructionFormat::DS
                                ? static_cast<std::int64_t>(i.ds_displacement())
                                : static_cast<std::int64_t>(i.simm16());
  return static_cast<GuestAddress>(base + static_cast<std::uint64_t>(displacement));
}

[[nodiscard]] std::uint64_t load_scalar(MemoryAccessContext& memory,
                                        GuestAddress address,
                                        const ScalarAccess& access) {
  std::uint64_t value = 0u;
  switch (access.bytes) {
    case 1: value = memory.read8(address); break;
    case 2: value = access.little_endian ? memory.read16_le(address)
                                         : memory.read16_be(address); break;
    case 4: value = access.little_endian ? memory.read32_le(address)
                                         : memory.read32_be(address); break;
    case 8: value = access.little_endian ? memory.read64_le(address)
                                         : memory.read64_be(address); break;
    default: break;
  }
  if (!access.sign) return value;
  if (access.bytes == 2u)
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(
        static_cast<std::int16_t>(value)));
  if (access.bytes == 4u)
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(
        static_cast<std::int32_t>(value)));
  return value;
}

void store_scalar(MemoryAccessContext& memory, GuestAddress address,
                  const ScalarAccess& access, std::uint64_t value) {
  switch (access.bytes) {
    case 1: memory.write8(address, static_cast<std::uint8_t>(value)); break;
    case 2:
      if (access.little_endian) memory.write16_le(address, static_cast<std::uint16_t>(value));
      else memory.write16_be(address, static_cast<std::uint16_t>(value));
      break;
    case 4:
      if (access.little_endian) memory.write32_le(address, static_cast<std::uint32_t>(value));
      else memory.write32_be(address, static_cast<std::uint32_t>(value));
      break;
    case 8:
      if (access.little_endian) memory.write64_le(address, value);
      else memory.write64_be(address, value);
      break;
    default: break;
  }
}

[[nodiscard]] bool execute_simple(const DecodedInstruction& i,
                                  ExecutionContext& context) {
  auto& state = context.state;
  auto& mem = context.memory_access;
  const auto m = i.mnemonic();

  if (const auto access = load_access(m)) {
    const auto ea = effective_address(i, state, *access);
    state.gpr[i.rt()] = load_scalar(mem, ea, *access);
    if (access->update) state.gpr[i.ra()] = ea;
    return true;
  }
  if (const auto access = store_access(m)) {
    const auto ea = effective_address(i, state, *access);
    store_scalar(mem, ea, *access, state.gpr[i.rs()]);
    if (access->update) state.gpr[i.ra()] = ea;
    return true;
  }

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
  // Atomics (load-reserve / store-conditional). The reservation protocol is the
  // compiled path's exactly (backend_cpp_aot.cpp): a load-reserve replaces any
  // held reservation and records its token/width/address; a store-conditional
  // succeeds only if a reservation of the same width is still valid, always
  // clears it, and reports success in CR0.EQ (with XER.SO copied to CR0.SO).
  if (m == "lwarx" || m == "ldarx") {
    const bool word = m == "lwarx";
    const auto ea = static_cast<GuestAddress>((i.ra() != 0u ? state.gpr[i.ra()] : 0u) +
                                              state.gpr[i.rb()]);
    if (state.reservation.valid) mem.cancel_reservation(state.reservation.token);
    std::uint64_t observed = 0u;
    if (word) {
      std::uint32_t value = 0u;
      state.reservation.token = mem.reserve32(ea, value);
      observed = value;
    } else {
      std::uint64_t value = 0u;
      state.reservation.token = mem.reserve64(ea, value);
      observed = value;
    }
    state.reservation.valid = state.reservation.token != 0u;
    state.reservation.width = word ? 4u : 8u;
    state.reservation.address = ea;
    state.reservation.observed_value = observed;
    state.gpr[i.rt()] = observed;
    return true;
  }
  if (m == "stwcx" || m == "stdcx") {
    const bool word = m == "stwcx";
    const auto ea = static_cast<GuestAddress>((i.ra() != 0u ? state.gpr[i.ra()] : 0u) +
                                              state.gpr[i.rb()]);
    bool stored = false;
    if (state.reservation.valid) {
      if (state.reservation.width == (word ? 4u : 8u)) {
        stored = word ? mem.store_conditional32(ea, state.reservation.token,
                                                static_cast<std::uint32_t>(state.gpr[i.rs()]))
                      : mem.store_conditional64(ea, state.reservation.token, state.gpr[i.rs()]);
      } else {
        mem.cancel_reservation(state.reservation.token);
      }
    }
    state.reservation.clear();
    state.set_cr_field(0, static_cast<std::uint8_t>((stored ? 0x2u : 0u) | (state.xer_so() ? 0x1u : 0u)));
    return true;
  }
  // Load/store multiple word: rT..r31 from/to consecutive words at (rA|0) + d.
  if (m == "lmw" || m == "stmw") {
    const std::uint64_t base = i.ra() != 0u ? state.gpr[i.ra()] : 0u;
    for (unsigned r = i.rt(); r < 32u; ++r) {
      const auto ea = static_cast<GuestAddress>(
          base + static_cast<std::uint64_t>(static_cast<std::int64_t>(i.simm16())) + (r - i.rt()) * 4u);
      if (m == "lmw") state.gpr[r] = mem.read32_be(ea);
      else mem.write32_be(ea, static_cast<std::uint32_t>(state.gpr[r]));
    }
    return true;
  }
  // Load/store string: a byte count and register wrap defined by the architecture,
  // shared with the compiled path (aot::string_load/string_store).
  if (m == "lswi" || m == "lswx" || m == "stswi" || m == "stswx") {
    const bool indexed = m == "lswx" || m == "stswx";
    const std::uint64_t base = i.ra() != 0u ? state.gpr[i.ra()] : 0u;
    const auto ea = static_cast<GuestAddress>(indexed ? base + state.gpr[i.rb()] : base);
    const std::uint32_t count = indexed ? static_cast<std::uint32_t>(state.xer & 0x7Fu)
                                        : (i.rb() == 0u ? 32u : i.rb());
    if (m == "lswi" || m == "lswx") aot::string_load(state, mem, ea, count, i.rt());
    else aot::string_store(state, mem, ea, count, i.rt());
    return true;
  }
  // Machine state register.
  if (m == "mfmsr") {
    state.gpr[i.rt()] = state.msr;
    return true;
  }
  if (m == "mtmsr" || m == "mtmsrd") {
    aot::write_msr(state, state.gpr[i.rs()], ((i.word >> 16u) & 1u) != 0u, m == "mtmsrd");
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
  if (m == "crand" || m == "crandc" || m == "creqv" || m == "crnand" ||
      m == "crnor" || m == "cror" || m == "crorc" || m == "crxor") {
    const auto bt = (i.word >> 21u) & 31u;
    const auto ba = (i.word >> 16u) & 31u;
    const auto bb = (i.word >> 11u) & 31u;
    const bool a = state.cr_bit(ba);
    const bool b = state.cr_bit(bb);
    bool result = false;
    if (m == "crand") result = a && b;
    else if (m == "crandc") result = a && !b;
    else if (m == "creqv") result = a == b;
    else if (m == "crnand") result = !(a && b);
    else if (m == "crnor") result = !(a || b);
    else if (m == "cror") result = a || b;
    else if (m == "crorc") result = a || !b;
    else result = a != b;
    state.set_cr_bit(bt, result);
    return true;
  }
  if (m == "mcrf") {
    const auto bf = (i.word >> 23u) & 7u;
    const auto bfa = (i.word >> 18u) & 7u;
    state.set_cr_field(bf, state.cr_field(bfa));
    return true;
  }
  if (m == "mcrxr") {
    const auto field = static_cast<std::uint8_t>(
        (state.xer_so() ? 8u : 0u) | (state.xer_ov() ? 4u : 0u) |
        (state.xer_ca() ? 2u : 0u));
    state.set_cr_field(i.crfd(), field);
    state.xer &= ~(xer_bits::SO | xer_bits::OV | xer_bits::CA);
    return true;
  }
  if (m == "mfcr") {
    state.gpr[i.rt()] = state.cr;
    return true;
  }
  if (m == "mtcrf") {
    const auto fxm = (i.word >> 12u) & 0xFFu;
    const auto source = static_cast<std::uint32_t>(state.gpr[i.rs()]);
    for (unsigned field = 0; field < 8u; ++field) {
      if ((fxm & (0x80u >> field)) == 0u) continue;
      const auto shift = (7u - field) * 4u;
      state.cr = (state.cr & ~(0xFu << shift)) | (source & (0xFu << shift));
    }
    return true;
  }
  if (m == "mfspr" || m == "mftb") {
    const auto spr = i.spr();
    std::uint64_t value = 0u;
    if (spr == 1u) value = state.xer;
    else if (spr == 8u) value = state.lr;
    else if (spr == 9u) value = state.ctr;
    else if (spr == 256u) value = state.vrsave;
    else if (spr == 268u) value = context.runtime.read_time_base(state);
    else if (spr == 269u) value = context.runtime.read_time_base(state) >> 32u;
    else if (spr == 287u) value = state.pvr;
    else value = context.runtime.read_spr(spr, state);
    state.gpr[i.rt()] = value;
    return true;
  }
  if (m == "mtspr") {
    const auto spr = i.spr();
    const auto value = state.gpr[i.rs()];
    if (spr == 1u) state.xer = static_cast<std::uint32_t>(value);
    else if (spr == 8u) state.lr = value;
    else if (spr == 9u) state.ctr = value;
    else if (spr == 256u) state.vrsave = static_cast<std::uint32_t>(value);
    else context.runtime.write_spr(spr, value, state);
    return true;
  }
  if (m == "sync") {
    mem.barrier(BarrierKind::Sync);
    return true;
  }
  if (m == "eieio") {
    mem.barrier(BarrierKind::Eieio);
    return true;
  }
  if (m == "isync") {
    mem.barrier(BarrierKind::InstructionSync);
    return true;
  }
  if (m == "dcbz" || m == "dcbz128") {
    const auto ea = static_cast<GuestAddress>((i.ra() ? state.gpr[i.ra()] : 0u) + state.gpr[i.rb()]);
    mem.zero_cache_block(ea, m == "dcbz128" ? 128u : 32u);
    return true;
  }
  if (m == "icbi") {
    const auto ea = static_cast<GuestAddress>((i.ra() ? state.gpr[i.ra()] : 0u) + state.gpr[i.rb()]);
    mem.instruction_cache_invalidate(ea);
    return true;
  }
  // Cache hints have no architecturally visible data result in this fallback.
  if (m == "dcbf" || m == "dcbi" || m == "dcbst" || m == "dcbt" || m == "dcbtst")
    return true;

  // Real AC6 repro: the entire VMX/VMX128 vector unit - the largest single
  // gap this interpreter has ever had - had no case here at all, starting
  // with "lvx128" (a real load AC6's own compiled code executes constantly
  // for matrix/skinning math). Loads/stores are handled directly (mirroring
  // lifter_memory.cpp's own vector-memory cases exactly); every arithmetic/
  // logic/compare/permute/pack/splat/select/rotate/shift mnemonic reuses
  // aot::execute_vector() - the same runtime-dispatched semantic function
  // the AOT-compiled path calls for everything it does not special-case for
  // performance (see vector_semantic_for()'s own comment above).
  if (m == "lvx" || m == "lvxl" || m == "lvx128" || m == "lvxl128") {
    const auto base = i.ra() ? state.gpr[i.ra()] : 0u;
    const auto ea = static_cast<GuestAddress>((base + state.gpr[i.rb()]) & ~0xFull);
    state.vr[vector_vd(i)] = mem.read128(ea);
    return true;
  }
  if (m == "stvx" || m == "stvxl" || m == "stvx128" || m == "stvxl128") {
    const auto base = i.ra() ? state.gpr[i.ra()] : 0u;
    const auto ea = static_cast<GuestAddress>((base + state.gpr[i.rb()]) & ~0xFull);
    mem.write128(ea, state.vr[vector_vd(i)]);
    return true;
  }
  if (m == "lvebx" || m == "lvehx" || m == "lvewx" || m == "lvewx128") {
    const auto base = i.ra() ? state.gpr[i.ra()] : 0u;
    const auto ea = static_cast<GuestAddress>(base + state.gpr[i.rb()]);
    const unsigned width = m == "lvebx" ? 1u : (m == "lvehx" ? 2u : 4u);
    const auto reg = vector_vd(i);
    state.vr[reg] = aot::vector_load_element(state.vr[reg], mem, ea, width);
    return true;
  }
  if (m == "stvebx" || m == "stvehx" || m == "stvewx" || m == "stvewx128") {
    const auto base = i.ra() ? state.gpr[i.ra()] : 0u;
    const auto ea = static_cast<GuestAddress>(base + state.gpr[i.rb()]);
    const unsigned width = m == "stvebx" ? 1u : (m == "stvehx" ? 2u : 4u);
    aot::vector_store_element(state.vr[vector_vd(i)], mem, ea, width);
    return true;
  }
  if (m.starts_with("lvlx")) {
    const auto base = i.ra() ? state.gpr[i.ra()] : 0u;
    const auto ea = static_cast<GuestAddress>(base + state.gpr[i.rb()]);
    const auto reg = vector_vd(i);
    state.vr[reg] = aot::vector_load_left(state.vr[reg], mem, ea);
    return true;
  }
  if (m.starts_with("lvrx")) {
    const auto base = i.ra() ? state.gpr[i.ra()] : 0u;
    const auto ea = static_cast<GuestAddress>(base + state.gpr[i.rb()]);
    const auto reg = vector_vd(i);
    state.vr[reg] = aot::vector_load_right(state.vr[reg], mem, ea);
    return true;
  }
  if (m.starts_with("stvlx")) {
    const auto base = i.ra() ? state.gpr[i.ra()] : 0u;
    const auto ea = static_cast<GuestAddress>(base + state.gpr[i.rb()]);
    aot::vector_store_left(state.vr[vector_vd(i)], mem, ea);
    return true;
  }
  if (m.starts_with("stvrx")) {
    const auto base = i.ra() ? state.gpr[i.ra()] : 0u;
    const auto ea = static_cast<GuestAddress>(base + state.gpr[i.rb()]);
    aot::vector_store_right(state.vr[vector_vd(i)], mem, ea);
    return true;
  }
  if (m == "lvsl" || m == "lvsl128" || m == "lvsr" || m == "lvsr128") {
    const auto base = i.ra() ? state.gpr[i.ra()] : 0u;
    const auto ea = base + state.gpr[i.rb()];
    state.vr[vector_vd(i)] = m.starts_with("lvsl") ? aot::vector_load_shift_left(ea)
                                                   : aot::vector_load_shift_right(ea);
    return true;
  }
  if (m == "mfvscr") {
    Vector128 v{};
    v.set_u32_be(3, state.vscr);
    state.vr[i.vd5()] = v;
    return true;
  }
  if (m == "mtvscr") {
    state.vscr = state.vr[i.vb5()].u32_be(3);
    return true;
  }
  // A handful of mnemonic families take a genuinely different operand set on
  // the classic 3-register VA-form encoding than on Xbox's extended VX128
  // encodings - mirroring lifter_vector.cpp's own per-format branches exactly
  // (same real hardware operand wiring, independently duplicated here since
  // the interpreter has no IR::Builder to share that code through). Every
  // other mnemonic below uses the same (va, vb, vc) operand set regardless of
  // format, which the generic dispatch after this block handles uniformly.
  if (m.starts_with("vmaddfp") || m.starts_with("vmaddcfp") || m.starts_with("vnmsubfp")) {
    const auto semantic = vector_semantic_for(m);
    if (!semantic) return false;
    const auto reg = vector_vd(i);
    Vector128 a, b, c;
    if (i.info->format == InstructionFormat::VA) {
      a = state.vr[vector_va(i)];
      b = state.vr[vector_vb(i)];
      c = state.vr[vector_vc(i)];
    } else {
      a = state.vr[reg];
      b = state.vr[vector_va(i)];
      c = state.vr[vector_vb(i)];
    }
    state.vr[reg] = aot::execute_vector(*semantic, i.word, state, a, b, c, Vector128{});
    return true;
  }
  if (m.starts_with("vsel")) {
    const auto semantic = vector_semantic_for(m);
    if (!semantic) return false;
    const auto reg = vector_vd(i);
    Vector128 a, b, c;
    if (i.info->format == InstructionFormat::VA) {
      a = state.vr[vector_va(i)];
      b = state.vr[vector_vb(i)];
      c = state.vr[vector_vc(i)];
    } else {
      a = state.vr[reg];
      b = state.vr[vector_va(i)];
      c = state.vr[vector_vb(i)];
    }
    state.vr[reg] = aot::execute_vector(*semantic, i.word, state, a, b, c, Vector128{});
    return true;
  }
  if (m.starts_with("vsum") || m.starts_with("vmsum")) {
    const auto semantic = vector_semantic_for(m);
    if (!semantic) return false;
    const auto reg = vector_vd(i);
    const auto a = state.vr[vector_va(i)];
    const auto b = state.vr[vector_vb(i)];
    const auto c = i.info->format == InstructionFormat::VA ? state.vr[vector_vc(i)] : Vector128{};
    state.vr[reg] = aot::execute_vector(*semantic, i.word, state, a, b, c, Vector128{});
    return true;
  }
  if (m.starts_with("vpermwi")) {
    const auto semantic = vector_semantic_for(m);
    if (!semantic) return false;
    const auto reg = vector_vd(i);
    const auto b = state.vr[vector_vb(i)];
    state.vr[reg] =
        aot::execute_vector(*semantic, i.word, state, Vector128{}, b, Vector128{}, Vector128{});
    return true;
  }
  if (m.starts_with("vperm")) {
    const auto semantic = vector_semantic_for(m);
    if (!semantic) return false;
    const auto reg = vector_vd(i);
    if (i.info->format == InstructionFormat::VA || i.info->format == InstructionFormat::VX128_2) {
      const auto a = state.vr[vector_va(i)];
      const auto b = state.vr[vector_vb(i)];
      const auto c = state.vr[vector_vc(i)];
      state.vr[reg] = aot::execute_vector(*semantic, i.word, state, a, b, c, Vector128{});
    } else {
      const auto b = state.vr[vector_vb(i)];
      state.vr[reg] =
          aot::execute_vector(*semantic, i.word, state, Vector128{}, b, Vector128{}, Vector128{});
    }
    return true;
  }
  if (m.starts_with("vrlimi")) {
    const auto semantic = vector_semantic_for(m);
    if (!semantic) return false;
    const auto reg = vector_vd(i);
    const auto old = state.vr[reg];
    const auto b = state.vr[vector_vb(i)];
    state.vr[reg] =
        aot::execute_vector(*semantic, i.word, state, old, b, Vector128{}, Vector128{});
    return true;
  }
  if (const auto semantic = vector_semantic_for(m)) {
    const auto reg = vector_vd(i);
    const auto a = state.vr[vector_va(i)];
    const auto b = state.vr[vector_vb(i)];
    const auto c = state.vr[vector_vc(i)];
    const auto result = aot::execute_vector(*semantic, i.word, state, a, b, c, Vector128{});
    state.vr[reg] = result;
    if (m.starts_with("vcmp") && i.rc()) {
      aot::update_cr6_from_vector_compare(state, result);
    }
    return true;
  }

  return false;
}

}  // namespace

DynamicFallbackExecutor::DynamicFallbackExecutor(DynamicFallbackConfig config,
                                                 ObservationCallback observer)
    : config_(config), observer_(std::move(observer)) {
  config_.max_instructions_per_dispatch =
      std::max<std::uint32_t>(1u, config_.max_instructions_per_dispatch);
  config_.max_nested_calls = std::max<std::uint32_t>(1u, config_.max_nested_calls);
}

void DynamicFallbackExecutor::bind(ExecutionContext& context) noexcept {
  context.dynamic_fallback_executor = this;
  context.dynamic_fallback = &DynamicFallbackExecutor::callback;
}

DynamicFallbackResult DynamicFallbackExecutor::callback(
    void* executor, ExecutionContext& context, GuestAddress target,
    CompiledLookupKind kind) {
  if (!executor) return {};
  return static_cast<DynamicFallbackExecutor*>(executor)->try_execute(
      context, target, kind);
}

DynamicFallbackResult DynamicFallbackExecutor::try_execute(
    ExecutionContext& context, GuestAddress target, CompiledLookupKind kind) {
  const auto site = context.state.cia;
  auto run_result = run(context, target, kind, 0u);
  if (run_result.public_result.handled) {
    executed_blocks_.fetch_add(1u, std::memory_order_relaxed);
    executed_instructions_.fetch_add(run_result.instructions,
                                     std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(pc_hits_mutex_);
      ++pc_hits_[target];
    }
    if (observer_) {
      observer_(DynamicFallbackObservation{
          target, site, run_result.exit, kind, run_result.reason,
          run_result.instructions, run_result.fingerprint});
    }
  }
  return run_result.public_result;
}

std::size_t DynamicFallbackExecutor::fallback_unique_pc_count() const {
  std::lock_guard<std::mutex> lock(pc_hits_mutex_);
  return pc_hits_.size();
}

std::vector<std::pair<GuestAddress, std::uint64_t>>
DynamicFallbackExecutor::fallback_hot_pcs(std::size_t top_n) const {
  std::vector<std::pair<GuestAddress, std::uint64_t>> hits;
  {
    std::lock_guard<std::mutex> lock(pc_hits_mutex_);
    hits.reserve(pc_hits_.size());
    for (const auto& [pc, count] : pc_hits_) hits.emplace_back(pc, count);
  }
  std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) {
    if (a.second != b.second) return a.second > b.second;
    return a.first < b.first;  // Deterministic tie-break for reproducible reports.
  });
  if (hits.size() > top_n) hits.resize(top_n);
  return hits;
}

DynamicFallbackExecutor::RunResult DynamicFallbackExecutor::run(
    ExecutionContext& context, GuestAddress target, CompiledLookupKind kind,
    std::uint32_t depth) {
  RunResult out{};
  out.exit = target;
  out.fingerprint = kFnvOffset;

  if ((target & 3u) != 0u) {
    out.reason = DynamicFallbackStopReason::InvalidTarget;
    return out;
  }

  const auto first_stamp = context.memory.executable_page_stamp(target);
  if (!first_stamp.executable()) return out;  // Not ours; imports may handle it.

  // A recognized import thunk's bytes are loader-owned placeholder metadata
  // (ordinal/attributes/record-type), never real guest PPC, even though the
  // page containing them is marked executable (native XEX import records
  // commonly live inside the same executable section as real code). Decoding
  // them here would hit the exact "unsupported instruction" trap this
  // interpreter exists to avoid, for what is really a perfectly resolvable
  // import call - route straight to the runtime's own import dispatch
  // instead, the same way execute_call() below already falls through to
  // context.runtime.call() as its own last resort for a target this
  // interpreter cannot otherwise handle. Checked once here (covering both
  // try_execute()'s top-level entry and this same run() being re-entered for
  // a nested call below) rather than on every compiled-call hot path.
  if (context.runtime.is_recognized_import_thunk(target)) {
    out.public_result.handled = true;
    out.public_result.result = context.runtime.call(target, context.state, context.memory);
    return out;
  }

  out.public_result.handled = true;
  if (depth > config_.max_nested_calls) {
    out.reason = DynamicFallbackStopReason::CallDepthLimit;
    out.public_result.result = {FlowReason::Trap, target, kFallbackCallDepthDetail};
    return out;
  }
  const auto entry_return = static_cast<GuestAddress>(context.state.lr & ~3ull);
  std::unordered_map<GuestAddress, ExecutablePageStamp> page_stamps;
  Decoder decoder;
  GuestAddress pc = target;

  const auto trap_result = [&](DynamicFallbackStopReason reason,
                               std::uint32_t detail) {
    out.reason = reason;
    out.exit = pc;
    out.public_result.result = {FlowReason::Trap, pc, detail};
  };

  const auto validate_page = [&](GuestAddress address) -> bool {
    const auto page = address & kGuestPageMask;
    const auto current = context.memory.executable_page_stamp(address);
    if (!current.executable()) return false;
    const auto [it, inserted] = page_stamps.emplace(page, current);
    if (!inserted && it->second != current) {
      source_invalidations_.fetch_add(1u, std::memory_order_relaxed);
      trap_result(DynamicFallbackStopReason::SourceChanged,
                  kFallbackSourceChangedDetail);
      return false;
    }
    return true;
  };

  const auto finish_call = [&](const ExecutionResult& result,
                               GuestAddress expected_return,
                               GuestAddress& next_pc) -> std::optional<ExecutionResult> {
    if (result.reason == FlowReason::Return) {
      if (result.next_address == expected_return) {
        next_pc = expected_return;
        return std::nullopt;
      }
      return result;
    }
    if (result.reason == FlowReason::Fallthrough) {
      next_pc = expected_return;
      return std::nullopt;
    }
    return result;
  };

  const auto execute_call = [&](GuestAddress call_target,
                                GuestAddress expected_return,
                                GuestAddress& next_pc) -> std::optional<ExecutionResult> {
    if (auto* native = context.lookup_compiled(call_target, CompiledLookupKind::Call)) {
      const auto result = native(context);
      return finish_call(result, expected_return, next_pc);
    }
    if (context.memory.executable_page_stamp(call_target).executable()) {
      auto nested = run(context, call_target, CompiledLookupKind::Call, depth + 1u);
      out.instructions += nested.instructions;
      out.fingerprint ^= nested.fingerprint + 0x9E3779B97F4A7C15ull +
                         (out.fingerprint << 6u) + (out.fingerprint >> 2u);
      if (nested.public_result.handled) {
        // Nested dynamically discovered callees are first-class learned facts,
        // not hidden inside the outer observation. Instruction totals remain
        // accounted once by the outer dispatch; block counts/observations are
        // emitted here for each nested entry.
        executed_blocks_.fetch_add(1u, std::memory_order_relaxed);
        if (observer_) {
          observer_(DynamicFallbackObservation{
              call_target, pc, nested.exit, CompiledLookupKind::Call,
              nested.reason, nested.instructions, nested.fingerprint});
        }
        return finish_call(nested.public_result.result, expected_return, next_pc);
      }
    }
    const auto result = context.runtime.call(call_target, context.state, context.memory);
    return finish_call(result, expected_return, next_pc);
  };

  // An UNLINKED branch (bx/bcx/bclrx/bcctrx with LK=0 - a tail call/tail
  // branch, common compiler output for e.g. "if (x) return f();") never goes
  // through execute_call() above, so it never re-entered run() and never hit
  // the is_recognized_import_thunk() check at this function's own top for
  // the ORIGINAL entry target. Without this, a tail branch landing on an
  // import-thunk address (the exact same loader-owned placeholder bytes a
  // `bl` into the same address is already correctly routed around) would
  // fall through to `next_pc = branch_target` and get decoded as PPC on the
  // next loop iteration, producing a spurious "unsupported instruction"
  // trap for what is really a perfectly resolvable import call - real guest
  // code make no ABI distinction between reaching an import via `bl` or via
  // a tail branch, so neither should Xenon.
  const auto execute_tail_branch = [&](GuestAddress branch_target) -> bool {
    if (auto* native = context.lookup_compiled(branch_target, CompiledLookupKind::Branch)) {
      out.reason = DynamicFallbackStopReason::CompiledHandoff;
      out.exit = branch_target;
      out.public_result.result = native(context);
      return true;
    }
    if (context.runtime.is_recognized_import_thunk(branch_target)) {
      out.reason = DynamicFallbackStopReason::CompiledHandoff;
      out.exit = branch_target;
      out.public_result.handled = true;
      auto result = context.runtime.call(branch_target, context.state, context.memory);
      // XenonSession::call()'s Fallthrough convention ("the export ran;
      // continue at state.cia") is only meaningful to a caller that already
      // knows the real continuation address itself - a linked `bl` call site
      // does (its own expected_return, computed from pc+4 - see
      // execute_call()/finish_call() above), so it can safely ignore
      // Fallthrough's next_address field entirely. An UNLINKED tail branch
      // has no such fallback: state.cia at this point is still this bx
      // instruction's own address (nothing updates it before the call), so
      // forwarding Fallthrough as-is would make the OUTER dispatch loop
      // re-dispatch this exact same branch forever. A tail branch never
      // expects control back at all - it IS this function's own return, so
      // completing it means returning to the function's real caller via the
      // untouched LR, exactly as the equivalent real guest bytes
      // (`bl <import-thunk-copy>`/`blr`, or the loader-owned thunk's own
      // `mtctr`/`bctr`) would.
      if (result.reason == FlowReason::Fallthrough) {
        result = {FlowReason::Return, static_cast<GuestAddress>(context.state.lr & ~3ull), 0u};
      }
      out.public_result.result = result;
      return true;
    }
    return false;
  };

  for (std::uint32_t local_count = 0u;
       local_count < config_.max_instructions_per_dispatch; ++local_count) {
    if (!validate_page(pc)) {
      if (out.reason != DynamicFallbackStopReason::SourceChanged)
        trap_result(DynamicFallbackStopReason::InvalidTarget,
                    kFallbackInvalidTargetDetail);
      return out;
    }

    context.state.cia = pc;
    context.state.nia = pc + 4u;
    const auto word = context.memory.fetch32_be(pc);
    // Revalidate after the fetch as well. A host thread may rewrite/remap the
    // executable page between the pre-fetch stamp check and the actual read;
    // observations must never bless a mixed-generation block.
    if (!validate_page(pc)) {
      if (out.reason != DynamicFallbackStopReason::SourceChanged)
        trap_result(DynamicFallbackStopReason::InvalidTarget,
                    kFallbackInvalidTargetDetail);
      return out;
    }
    out.fingerprint = fingerprint_step(out.fingerprint, word);
    ++out.instructions;
    const auto insn = decoder.decode(pc, word);
    if (!insn.valid()) {
      unsupported_instructions_.fetch_add(1u, std::memory_order_relaxed);
      trap_result(DynamicFallbackStopReason::UnsupportedInstruction,
                  kFallbackUnsupportedDetail);
      return out;
    }

    const auto mnemonic = insn.mnemonic();
    GuestAddress next_pc = pc + 4u;

    if (mnemonic == "bx") {
      const auto branch_target = insn.direct_branch_target() & ~3u;
      if (insn.lk()) {
        context.state.lr = pc + 4u;
        if (const auto escaped = execute_call(branch_target, pc + 4u, next_pc)) {
          out.reason = escaped->reason == FlowReason::Trap
                           ? DynamicFallbackStopReason::Trap
                           : DynamicFallbackStopReason::CompiledHandoff;
          out.exit = escaped->next_address;
          out.public_result.result = *escaped;
          return out;
        }
      } else {
        if (execute_tail_branch(branch_target)) return out;
        next_pc = branch_target;
      }
    } else if (mnemonic == "bcx") {
      const bool taken = branch_condition(insn, context.state, true);
      if (insn.lk()) context.state.lr = pc + 4u;
      if (taken) {
        const auto branch_target = insn.direct_branch_target() & ~3u;
        if (insn.lk()) {
          if (const auto escaped = execute_call(branch_target, pc + 4u, next_pc)) {
            out.reason = escaped->reason == FlowReason::Trap
                             ? DynamicFallbackStopReason::Trap
                             : DynamicFallbackStopReason::CompiledHandoff;
            out.exit = escaped->next_address;
            out.public_result.result = *escaped;
            return out;
          }
        } else {
          if (execute_tail_branch(branch_target)) return out;
          next_pc = branch_target;
        }
      }
    } else if (mnemonic == "bclrx" || mnemonic == "bcctrx") {
      const bool use_ctr = mnemonic == "bclrx";
      const auto raw_target = mnemonic == "bclrx" ? context.state.lr : context.state.ctr;
      const auto branch_target = static_cast<GuestAddress>(raw_target & ~3ull);
      const bool taken = branch_condition(insn, context.state, use_ctr);
      if (insn.lk()) context.state.lr = pc + 4u;
      if (taken) {
        if (insn.lk()) {
          if (const auto escaped = execute_call(branch_target, pc + 4u, next_pc)) {
            out.reason = escaped->reason == FlowReason::Trap
                             ? DynamicFallbackStopReason::Trap
                             : DynamicFallbackStopReason::CompiledHandoff;
            out.exit = escaped->next_address;
            out.public_result.result = *escaped;
            return out;
          }
        } else if (mnemonic == "bclrx" && kind == CompiledLookupKind::Call &&
                   branch_target == entry_return) {
          // NIA is architecturally the taken return target. Gen 8 differential
          // verification caught the fallback returning the correct
          // ExecutionResult while leaving CpuState::nia at pc+4.
          context.state.nia = branch_target;
          out.reason = DynamicFallbackStopReason::Returned;
          out.exit = branch_target;
          out.public_result.result = {FlowReason::Return, branch_target, 0u};
          return out;
        } else if (mnemonic == "bclrx" &&
                   !context.memory.executable_page_stamp(branch_target).executable() &&
                   !context.lookup_compiled(branch_target, CompiledLookupKind::Branch) &&
                   !context.runtime.is_recognized_import_thunk(branch_target)) {
          // A return to an address that is not guest code at all (the thread-exit
          // sentinel, a host-owned return address) cannot be interpreted. This is
          // reached after an instruction-budget yield re-entered the function as a
          // Branch, where entry_return no longer identifies the real caller. Hand
          // the return back to the dispatcher, which owns those sentinels.
          context.state.nia = branch_target;
          out.reason = DynamicFallbackStopReason::Returned;
          out.exit = branch_target;
          out.public_result.result = {FlowReason::Return, branch_target, 0u};
          return out;
        } else {
          if (execute_tail_branch(branch_target)) return out;
          next_pc = branch_target;
        }
      }
    } else if (mnemonic == "sc") {
      const auto result = context.runtime.syscall((insn.word >> 5u) & 0x7Fu,
                                                  context.state, context.memory);
      out.reason = DynamicFallbackStopReason::Syscall;
      out.exit = result.next_address;
      out.public_result.result = result;
      return out;
    } else if (mnemonic == "td" || mnemonic == "tdi" || mnemonic == "tw" ||
               mnemonic == "twi") {
      const auto to = (insn.word >> 21u) & 31u;
      const auto ra = (insn.word >> 16u) & 31u;
      const auto rb = (insn.word >> 11u) & 31u;
      const bool word_form = mnemonic == "tw" || mnemonic == "twi";
      const bool immediate = mnemonic == "tdi" || mnemonic == "twi";
      const auto rhs = immediate
                           ? static_cast<std::uint64_t>(static_cast<std::int64_t>(
                                 static_cast<std::int16_t>(insn.word & 0xFFFFu)))
                           : context.state.gpr[rb];
      if (aot::trap_condition(to, context.state.gpr[ra], rhs, word_form)) {
        const auto result = context.runtime.trap(to, context.state, context.memory);
        out.reason = DynamicFallbackStopReason::Trap;
        out.exit = result.next_address;
        out.public_result.result = result;
        return out;
      }
    } else if (!execute_simple(insn, context)) {
      unsupported_instructions_.fetch_add(1u, std::memory_order_relaxed);
      trap_result(DynamicFallbackStopReason::UnsupportedInstruction,
                  kFallbackUnsupportedDetail);
      return out;
    }

    pc = next_pc;
  }

  // The per-dispatch budget bounds how long one host call runs, it is not a
  // fault. Real guest code legitimately runs far more than the budget in one
  // stretch (table initialisers, memcpy-style loops - Ace Combat 6's startup
  // walks an 80-entry constructor table right after entry), so yield at the
  // next instruction as an ordinary Branch: all architectural state is already
  // in CpuState/guest memory, and the dispatcher re-enters here (or in a
  // compiled function) at `pc`. A truly endless guest loop is still bounded by
  // the session's top-level dispatch limit.
  out.reason = DynamicFallbackStopReason::InstructionLimit;
  out.exit = pc;
  context.state.cia = pc;
  context.state.nia = pc;
  out.public_result.result = {FlowReason::Branch, pc, 0u};
  return out;
}

bool dynamic_fallback_supports(const DecodedInstruction& insn, MemoryPort& scratch_memory) {
  if (!insn.valid()) return false;
  const auto m = insn.mnemonic();
  // Control flow and traps the block loop handles inline (see the dispatch chain
  // in DynamicFallbackExecutor::run_block).
  if (m == "bx" || m == "bcx" || m == "bclrx" || m == "bcctrx" || m == "sc" || m == "td" ||
      m == "tdi" || m == "tw" || m == "twi") {
    return true;
  }
  NullRuntimeServices runtime;
  CpuState state{};
  ExecutionContext context(state, scratch_memory, runtime);
  state.cia = insn.address;
  state.nia = insn.address + 4u;
  try {
    return execute_simple(insn, context);
  } catch (...) {
    // The interpreter recognized the instruction and got as far as touching the
    // scratch memory / an operand it could not use.
    return true;
  }
}

}  // namespace xenon::cpu

#pragma once

// Private dynamic-fallback interpreter header shared by the executor
// (dynamic_fallback.cpp) and the per-class instruction files under
// interpreter/. The helpers keep internal linkage in every including file,
// as they had in the original single translation unit. Private to xenon_cpu.

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

[[nodiscard]] inline std::uint64_t fingerprint_step(std::uint64_t hash,
                                             std::uint32_t word) noexcept {
  // Keep the first runtime fingerprint address-independent. Gen 9 can add
  // deeper normalization of PC-relative immediates; even this form already
  // survives a block moving wholesale between executable revisions.
  hash ^= word;
  hash *= kFnvPrime;
  return hash;
}

[[nodiscard]] inline bool branch_condition(const DecodedInstruction& i,
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

inline void write_compare(CpuState& state, unsigned field, std::uint64_t lhs,
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
[[nodiscard]] inline std::uint64_t mask64(unsigned mb, unsigned me) noexcept {
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

[[nodiscard]] inline std::uint32_t mask32(unsigned mb, unsigned me) noexcept {
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
inline bool vector_extended_format(InstructionFormat f) noexcept {
  return f == InstructionFormat::VX128 || f == InstructionFormat::VX128_1 ||
        f == InstructionFormat::VX128_2 || f == InstructionFormat::VX128_3 ||
        f == InstructionFormat::VX128_4 || f == InstructionFormat::VX128_5 ||
        f == InstructionFormat::VX128_R || f == InstructionFormat::VX128_P;
}

inline unsigned vector_vd(const DecodedInstruction& i) noexcept {
  return vector_extended_format(i.info->format) ? i.vx128_vd() : i.vd5();
}

inline unsigned vector_va(const DecodedInstruction& i) noexcept {
  switch (i.info->format) {
    case InstructionFormat::VX128: case InstructionFormat::VX128_2:
    case InstructionFormat::VX128_5: case InstructionFormat::VX128_R:
      return i.vx128_va();
    default: return i.va5();
  }
}

inline unsigned vector_vb(const DecodedInstruction& i) noexcept {
  switch (i.info->format) {
    case InstructionFormat::VX128: case InstructionFormat::VX128_2:
    case InstructionFormat::VX128_3: case InstructionFormat::VX128_4:
    case InstructionFormat::VX128_5: case InstructionFormat::VX128_R:
    case InstructionFormat::VX128_P:
      return i.vx128_vb();
    default: return i.vb5();
  }
}

inline unsigned vector_vc(const DecodedInstruction& i) noexcept {
  if (i.info->format == InstructionFormat::VX128_2) return (i.word >> 8) & 7u;
  return i.vc5();
}

// Maps a decoded guest mnemonic to the aot::VectorSemantic enumerator of the
// exact same name (see vector_semantic.hpp) so the interpreter can reuse
// aot::execute_vector() - the same runtime-dispatched semantic function the
// AOT-compiled path calls for every vector op it does not special-case for
// performance (see backend_cpp_aot.cpp's is_vector_compute() catch-all) -
// instead of reimplementing VMX/VMX128 arithmetic a second time.
inline std::optional<aot::VectorSemantic> vector_semantic_for(std::string_view m) noexcept {
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

[[nodiscard]] inline std::optional<ScalarAccess> load_access(std::string_view m) {
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

[[nodiscard]] inline std::optional<ScalarAccess> store_access(std::string_view m) {
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

[[nodiscard]] inline GuestAddress effective_address(const DecodedInstruction& i,
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

[[nodiscard]] inline std::uint64_t load_scalar(MemoryAccessContext& memory,
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

inline void store_scalar(MemoryAccessContext& memory, GuestAddress address,
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

}  // namespace

namespace fallback {

// Instruction classes executed by the interpreter (interpreter/*.cpp). Each
// returns false when the mnemonic is not one of its own.
[[nodiscard]] bool execute_scalar_memory(const DecodedInstruction& i, ExecutionContext& context);
[[nodiscard]] bool execute_integer(const DecodedInstruction& i, ExecutionContext& context);
[[nodiscard]] bool execute_system(const DecodedInstruction& i, ExecutionContext& context);
[[nodiscard]] bool execute_floating_point(const DecodedInstruction& i, ExecutionContext& context);
[[nodiscard]] bool execute_vector(const DecodedInstruction& i, ExecutionContext& context);

}  // namespace fallback
}  // namespace xenon::cpu

#pragma once

#include <cstdint>
#include <string_view>

#include "xenon/cpu/types.hpp"

namespace xenon::cpu {

enum class InstructionFormat : std::uint8_t {
  SC, D, DS, B, I, X, XL, XFX, XFL, XS, XO, A, M, MD, MDS, DCBZ,
  VX, VC, VA, VX128, VX128_1, VX128_2, VX128_3, VX128_4, VX128_5,
  VX128_R, VX128_P,
};

enum class InstructionGroup : std::uint8_t {
  Branch,
  Control,
  Memory,
  Integer,
  FloatingPoint,
  Vector,
};

enum class InstructionType : std::uint8_t {
  General,
  Synchronizing,
};

struct OpcodeId {
  std::uint32_t value{};

  [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
  explicit constexpr operator bool() const noexcept { return valid(); }
  friend constexpr bool operator==(OpcodeId, OpcodeId) = default;
};

inline constexpr OpcodeId kInvalidOpcodeId{};

struct OpcodeInfo {
  OpcodeId id{};
  std::uint32_t pattern{};
  std::uint32_t mask{};
  std::string_view mnemonic{};
  InstructionFormat format{};
  InstructionGroup group{};
  InstructionType type{};
};

struct DecodedInstruction {
  GuestAddress address{};
  std::uint32_t word{};
  const OpcodeInfo* info{};

  [[nodiscard]] bool valid() const noexcept { return info != nullptr; }
  [[nodiscard]] OpcodeId opcode_id() const noexcept {
    return info ? info->id : kInvalidOpcodeId;
  }
  [[nodiscard]] std::string_view mnemonic() const noexcept {
    return info ? info->mnemonic : std::string_view{"invalid"};
  }

  [[nodiscard]] std::uint32_t primary() const noexcept { return word >> 26; }
  [[nodiscard]] std::uint32_t rt() const noexcept { return (word >> 21) & 31u; }
  [[nodiscard]] std::uint32_t rs() const noexcept { return rt(); }
  [[nodiscard]] std::uint32_t rd() const noexcept { return rt(); }
  [[nodiscard]] std::uint32_t frt() const noexcept { return rt(); }
  [[nodiscard]] std::uint32_t frs() const noexcept { return rt(); }
  [[nodiscard]] std::uint32_t ra() const noexcept { return (word >> 16) & 31u; }
  [[nodiscard]] std::uint32_t rb() const noexcept { return (word >> 11) & 31u; }
  [[nodiscard]] std::uint32_t frc() const noexcept { return (word >> 6) & 31u; }
  [[nodiscard]] std::uint32_t vd5() const noexcept { return rt(); }
  [[nodiscard]] std::uint32_t va5() const noexcept { return ra(); }
  [[nodiscard]] std::uint32_t vb5() const noexcept { return rb(); }
  [[nodiscard]] std::uint32_t vc5() const noexcept { return frc(); }

  [[nodiscard]] std::int16_t simm16() const noexcept {
    return static_cast<std::int16_t>(word & 0xFFFFu);
  }
  [[nodiscard]] std::uint16_t uimm16() const noexcept {
    return static_cast<std::uint16_t>(word & 0xFFFFu);
  }
  [[nodiscard]] bool rc() const noexcept {
    if (!info) return false;
    switch (info->format) {
      case InstructionFormat::VC: return ((word >> 10) & 1u) != 0;
      case InstructionFormat::VX128_R: return ((word >> 6) & 1u) != 0;
      default: return (word & 1u) != 0;
    }
  }
  [[nodiscard]] bool oe() const noexcept { return ((word >> 10) & 1u) != 0; }

  [[nodiscard]] std::uint32_t crfd() const noexcept { return (word >> 23) & 7u; }
  [[nodiscard]] std::uint32_t crfs() const noexcept { return (word >> 18) & 7u; }

  [[nodiscard]] std::uint32_t bo() const noexcept { return (word >> 21) & 31u; }
  [[nodiscard]] std::uint32_t bi() const noexcept { return (word >> 16) & 31u; }
  [[nodiscard]] bool aa() const noexcept { return ((word >> 1) & 1u) != 0; }
  [[nodiscard]] bool lk() const noexcept { return (word & 1u) != 0; }

  [[nodiscard]] std::int32_t branch_i_displacement() const noexcept {
    const std::uint32_t li = word & 0x03FFFFFCu;
    return static_cast<std::int32_t>(sign_extend<26>(li));
  }
  [[nodiscard]] std::int32_t branch_b_displacement() const noexcept {
    const std::uint32_t bd = word & 0x0000FFFCu;
    return static_cast<std::int32_t>(sign_extend<16>(bd));
  }


  [[nodiscard]] std::int32_t ds_displacement() const noexcept {
    return static_cast<std::int32_t>(sign_extend<16>(word & 0xFFFCu));
  }

  [[nodiscard]] std::uint32_t spr() const noexcept {
    const std::uint32_t enc = (word >> 11) & 0x3FFu;
    return ((enc & 0x1Fu) << 5) | ((enc >> 5) & 0x1Fu);
  }

  [[nodiscard]] std::uint32_t sh32() const noexcept { return (word >> 11) & 31u; }
  [[nodiscard]] std::uint32_t mb32() const noexcept { return (word >> 6) & 31u; }
  [[nodiscard]] std::uint32_t me32() const noexcept { return (word >> 1) & 31u; }

  [[nodiscard]] std::uint32_t sh64_md() const noexcept {
    return ((word >> 11) & 31u) | ((word & 0x2u) << 4);
  }
  [[nodiscard]] std::uint32_t mb64_md() const noexcept {
    return ((word >> 6) & 31u) | ((word & 0x20u));
  }

  // VMX128 register reconstruction based on the Xbox-specific format.
  [[nodiscard]] std::uint32_t vx128_vd() const noexcept {
    return vd5() | ((word & 0xCu) << 3);
  }
  [[nodiscard]] std::uint32_t vx128_va() const noexcept {
    return va5() | ((word & 0x20u)) | ((word & 0x400u) >> 4);
  }
  [[nodiscard]] std::uint32_t vx128_vb() const noexcept {
    return vb5() | ((word & 0x3u) << 5);
  }


  [[nodiscard]] std::uint32_t vx128_3_imm() const noexcept { return (word >> 16) & 31u; }
  [[nodiscard]] std::uint32_t vx128_4_imm() const noexcept { return (word >> 16) & 31u; }
  [[nodiscard]] std::uint32_t vx128_4_z() const noexcept { return (word >> 6) & 3u; }
  [[nodiscard]] std::uint32_t vx128_5_sh() const noexcept { return (word >> 6) & 15u; }
  [[nodiscard]] std::uint32_t vx128_p_perm() const noexcept {
    return ((word >> 16) & 31u) | (((word >> 6) & 7u) << 5);
  }

  [[nodiscard]] GuestAddress direct_branch_target() const noexcept;
};

[[nodiscard]] constexpr std::uint32_t mask_for_format(InstructionFormat f) noexcept {
  switch (f) {
    case InstructionFormat::D:
    case InstructionFormat::B:
    case InstructionFormat::I:
    case InstructionFormat::M:
      return 0xFC000000u;
    case InstructionFormat::DS:
      return 0xFC000003u;
    case InstructionFormat::X:
    case InstructionFormat::XL:
    case InstructionFormat::XFX:
    case InstructionFormat::XFL:
      return 0xFC0007FEu;
    case InstructionFormat::XS:
      return 0xFC0007FCu;
    case InstructionFormat::XO:
      return 0xFC0003FEu;
    case InstructionFormat::A:
      return 0xFC00003Eu;
    case InstructionFormat::MD:
      return 0xFC00001Cu;
    case InstructionFormat::MDS:
      return 0xFC00001Eu;
    case InstructionFormat::DCBZ:
      return 0xFC2007FEu;
    case InstructionFormat::VX:
      return 0xFC0007FFu;
    case InstructionFormat::VC:
      return 0xFC0003FFu;
    case InstructionFormat::VA:
      return 0xFC00003Fu;
    case InstructionFormat::VX128:
      return 0xFC0003D0u;
    case InstructionFormat::VX128_1:
      return 0xFC0007F3u;
    case InstructionFormat::VX128_2:
      return 0xFC000210u;
    case InstructionFormat::VX128_3:
      return 0xFC0007F0u;
    case InstructionFormat::VX128_4:
      return 0xFC000730u;
    case InstructionFormat::VX128_5:
      return 0xFC000010u;
    case InstructionFormat::VX128_R:
      return 0xFC000390u;
    case InstructionFormat::VX128_P:
      return 0xFC000630u;
    case InstructionFormat::SC:
      // LEV is variable; primary and architecturally fixed low bits identify sc.
      return 0xFC000003u;
  }
  return 0xFFFFFFFFu;
}

}  // namespace xenon::cpu

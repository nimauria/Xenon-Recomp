#pragma once

#include <array>
#include <cstdint>
#include <cstring>

#include "xenon/cpu/types.hpp"

namespace xenon::cpu {

// XER is kept as the complete architecturally visible 32-bit register.
// PowerPC bit numbering is MSB-first; these masks use the conventional
// integer representation used when XER is moved through mfspr/mtspr.
namespace xer_bits {
inline constexpr std::uint32_t SO = 0x80000000u;
inline constexpr std::uint32_t OV = 0x40000000u;
inline constexpr std::uint32_t CA = 0x20000000u;
inline constexpr std::uint32_t BYTE_COUNT = 0x0000007Fu;
}  // namespace xer_bits

namespace fpscr_bits {
inline constexpr std::uint32_t FX     = 0x80000000u;
inline constexpr std::uint32_t FEX    = 0x40000000u;
inline constexpr std::uint32_t VX     = 0x20000000u;
inline constexpr std::uint32_t OX     = 0x10000000u;
inline constexpr std::uint32_t UX     = 0x08000000u;
inline constexpr std::uint32_t ZX     = 0x04000000u;
inline constexpr std::uint32_t XX     = 0x02000000u;
inline constexpr std::uint32_t VXSNAN = 0x01000000u;
inline constexpr std::uint32_t VXISI  = 0x00800000u;
inline constexpr std::uint32_t VXIDI  = 0x00400000u;
inline constexpr std::uint32_t VXZDZ  = 0x00200000u;
inline constexpr std::uint32_t VXIMZ  = 0x00100000u;
inline constexpr std::uint32_t VXVC   = 0x00080000u;
inline constexpr std::uint32_t FR     = 0x00040000u;
inline constexpr std::uint32_t FI     = 0x00020000u;
inline constexpr std::uint32_t FPRF_MASK = 0x0001F000u;
inline constexpr std::uint32_t VXSOFT = 0x00000400u;
inline constexpr std::uint32_t VXSQRT = 0x00000200u;
inline constexpr std::uint32_t VXCVI  = 0x00000100u;
inline constexpr std::uint32_t VE     = 0x00000080u;
inline constexpr std::uint32_t OE     = 0x00000040u;
inline constexpr std::uint32_t UE     = 0x00000020u;
inline constexpr std::uint32_t ZE     = 0x00000010u;
inline constexpr std::uint32_t XE     = 0x00000008u;
inline constexpr std::uint32_t NI     = 0x00000004u;
inline constexpr std::uint32_t RN_MASK = 0x00000003u;
}  // namespace fpscr_bits

namespace vscr_bits {
// In the architected 32-bit VSCR, NJ is bit 15 and SAT is bit 31 using
// PowerPC MSB-first numbering. Integer masks therefore appear as below.
inline constexpr std::uint32_t NJ  = 0x00010000u;
inline constexpr std::uint32_t SAT = 0x00000001u;
}  // namespace vscr_bits

enum class FpRoundingMode : std::uint8_t {
  Nearest = 0,
  TowardZero = 1,
  TowardPositive = 2,
  TowardNegative = 3,
};

struct ReservationState {
  bool valid{};
  std::uint8_t width{};  // 4 or 8 bytes for lwarx/ldarx.
  GuestAddress address{};
  std::uint64_t observed_value{};
  std::uint64_t token{}; // Memory implementation may use a generation token.

  void clear() noexcept { *this = {}; }
};

struct alignas(64) CpuState {
  std::array<std::uint64_t, 32> gpr{};
  std::array<std::uint64_t, 32> fpr_bits{};
  std::array<Vector128, 128> vr{};

  std::uint64_t lr{};
  std::uint64_t ctr{};

  std::uint32_t cr{};
  std::uint32_t xer{};
  std::uint32_t fpscr{};
  std::uint32_t vscr{};

  std::uint64_t msr{};
  std::uint32_t vrsave{};
  std::uint32_t pvr{0x710700u};

  std::uint64_t time_base{};
  GuestAddress cia{};
  GuestAddress nia{};

  ReservationState reservation{};

  [[nodiscard]] double fpr(std::size_t i) const noexcept {
    double v{};
    std::memcpy(&v, &fpr_bits[i], sizeof(v));
    return v;
  }
  void set_fpr(std::size_t i, double v) noexcept {
    std::memcpy(&fpr_bits[i], &v, sizeof(v));
  }

  [[nodiscard]] std::uint8_t cr_field(unsigned field) const noexcept;
  void set_cr_field(unsigned field, std::uint8_t value) noexcept;
  [[nodiscard]] bool cr_bit(unsigned bit) const noexcept;
  void set_cr_bit(unsigned bit, bool value) noexcept;

  [[nodiscard]] bool xer_ca() const noexcept { return (xer & xer_bits::CA) != 0; }
  [[nodiscard]] bool xer_ov() const noexcept { return (xer & xer_bits::OV) != 0; }
  [[nodiscard]] bool xer_so() const noexcept { return (xer & xer_bits::SO) != 0; }
  void set_xer_ca(bool v) noexcept;
  void set_xer_ov(bool v) noexcept;
  void set_xer_so(bool v) noexcept;
  void set_xer_overflow(bool overflow) noexcept;
  [[nodiscard]] std::uint8_t xer_byte_count() const noexcept {
    return static_cast<std::uint8_t>(xer & xer_bits::BYTE_COUNT);
  }
  void set_xer_byte_count(std::uint8_t n) noexcept {
    xer = (xer & ~xer_bits::BYTE_COUNT) | (n & 0x7Fu);
  }

  [[nodiscard]] FpRoundingMode fp_rounding_mode() const noexcept {
    return static_cast<FpRoundingMode>(fpscr & fpscr_bits::RN_MASK);
  }
  [[nodiscard]] bool vector_non_java() const noexcept { return (vscr & vscr_bits::NJ) != 0; }
  [[nodiscard]] bool vector_saturated() const noexcept { return (vscr & vscr_bits::SAT) != 0; }
  void set_vector_saturated() noexcept { vscr |= vscr_bits::SAT; }
  void clear_vector_saturated() noexcept { vscr &= ~vscr_bits::SAT; }

  void update_cr0_signed(std::uint64_t result) noexcept;
  void update_cr1_from_fpscr() noexcept;
};

static_assert(alignof(CpuState) >= 64);

}  // namespace xenon::cpu

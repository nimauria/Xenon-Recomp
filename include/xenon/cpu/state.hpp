#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#else
#include <cfenv>
#endif

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

// XENON_FP_EXACT_STATUS=0 restores the previous behaviour in which the default
// native floating-point path never updates FPSCR status (see aot_semantics.hpp).
// Read once when the module loads.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)  // getenv: read once, never written
#endif
inline bool fp_legacy_status = [] {
  const char* value = std::getenv("XENON_FP_EXACT_STATUS");
  return value && *value == '0';
}();
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// FPSCR[FPRF] (C, FL, FG, FE, FU) for a result, classified from its bits so
// the lazy status path needs no floating-point instruction.
[[nodiscard]] inline std::uint32_t fprf_class_of_bits(std::uint64_t bits) noexcept {
  const bool neg = (bits >> 63) != 0;
  const std::uint32_t exp = static_cast<std::uint32_t>((bits >> 52) & 0x7FFu);
  const std::uint64_t frac = bits & 0x000FFFFFFFFFFFFFull;
  if (exp == 0x7FFu) return frac ? 0x11u : (neg ? 0x09u : 0x05u);
  if (exp == 0u) return frac ? (neg ? 0x18u : 0x14u) : (neg ? 0x12u : 0x02u);
  return neg ? 0x08u : 0x04u;
}

// The FPSCR summary bits are derived: VX from the VX* detail bits, FEX from an
// enabled sticky exception.
inline void fpscr_recompute_summaries(std::uint32_t& f) noexcept {
  constexpr std::uint32_t detail = fpscr_bits::VXSNAN | fpscr_bits::VXISI | fpscr_bits::VXIDI |
      fpscr_bits::VXZDZ | fpscr_bits::VXIMZ | fpscr_bits::VXVC | fpscr_bits::VXSOFT |
      fpscr_bits::VXSQRT | fpscr_bits::VXCVI;
  if (f & detail) f |= fpscr_bits::VX; else f &= ~fpscr_bits::VX;
  const bool enabled = ((f & fpscr_bits::VX) && (f & fpscr_bits::VE)) ||
                       ((f & fpscr_bits::OX) && (f & fpscr_bits::OE)) ||
                       ((f & fpscr_bits::UX) && (f & fpscr_bits::UE)) ||
                       ((f & fpscr_bits::ZX) && (f & fpscr_bits::ZE)) ||
                       ((f & fpscr_bits::XX) && (f & fpscr_bits::XE));
  if (enabled) f |= fpscr_bits::FEX; else f &= ~fpscr_bits::FEX;
}

struct ReservationState {
  bool valid{};
  std::uint8_t width{};  // 4 or 8 bytes for lwarx/ldarx.
  GuestAddress address{};
  std::uint64_t observed_value{};
  std::uint64_t token{}; // Memory implementation may use a generation token.

  void clear() noexcept { *this = {}; }
};

#if defined(_MSC_VER)
#pragma warning(push)
// The tail padding is intentional: each CPU state begins on a cache line.
#pragma warning(disable : 4324)
#endif
struct alignas(64) CpuState {
  std::array<std::uint64_t, 32> gpr{};
  std::array<std::uint64_t, 32> fpr_bits{};
  std::array<Vector128, 128> vr{};

  std::uint64_t lr{};
  std::uint64_t ctr{};

  std::uint32_t cr{};
  std::uint32_t xer{};
  std::uint32_t fpscr{};
  // The Xbox 360's VMX unit runs in non-Java mode: denormal inputs and
  // results flush to zero (xenia and rexglue flush every VMX float op).
  std::uint32_t vscr{vscr_bits::NJ};

  std::uint64_t msr{};
  std::uint32_t vrsave{};
  std::uint32_t pvr{0x710700u};

  std::uint64_t time_base{};
  GuestAddress cia{};
  GuestAddress nia{};

  ReservationState reservation{};

  // Lazy FPSCR status (the exact, cheap floating-point tier; see
  // aot_semantics.hpp). While the guest's FPSCR control bits (RN, NI and the
  // five enables) are all zero, arithmetic runs as plain host instructions and
  // only leaves two traces:
  //   * fp_last_bits/fp_pending: the last FPRF-updating result, classified by
  //     fp_sync(), so FPRF/FR/FI cost one store per operation;
  //   * the host's MXCSR sticky flags (ZE/OE/UE/PE), which accumulate the
  //     sticky ZX/OX/UX/XX bits for this thread.
  // Invariant I1: FPSCR as the guest observes it is fpscr after fp_sync();
  // every reader and every writer of fpscr goes through fp_sync() first
  // (observe_fpscr()/write_fpscr()/the aot FPSCR helpers). Invariant I2: the
  // MXCSR flags of a thread belong to the guest code it runs; host code that
  // runs on the thread (FpHostBoundary) is bracketed so its own flags never
  // reach the guest.
  std::uint64_t fp_last_bits{};
  std::uint32_t fp_pending{};

  // Classify the pending result into FPRF; FR/FI are not tracked by Tier A
  // and read as zero.
  void fp_commit_fprf() noexcept {
    if (!fp_pending) return;
    fpscr = (fpscr & ~(fpscr_bits::FPRF_MASK | fpscr_bits::FR | fpscr_bits::FI)) |
            (fprf_class_of_bits(fp_last_bits) << 12);
    fp_pending = 0;
  }

  // Fold the pending status into fpscr (see the invariants above).
  void fp_sync() noexcept {
    std::uint32_t bits = 0;
#if defined(_M_X64) || defined(__x86_64__)
    const unsigned csr = _mm_getcsr();
    if (csr & 0x3Cu) {
      if (csr & 0x04u) bits |= fpscr_bits::ZX;
      if (csr & 0x08u) bits |= fpscr_bits::OX;
      if (csr & 0x10u) bits |= fpscr_bits::UX;
      if (csr & 0x20u) bits |= fpscr_bits::XX;
    }
    // IE (invalid) is not taken from the host: its VX* detail is recorded
    // exactly where the NaN result appears. DE is not an FPSCR bit.
    if (csr & 0x3Fu) _mm_setcsr(csr & ~0x3Fu);
#else
    const int ex = std::fetestexcept(FE_DIVBYZERO | FE_OVERFLOW | FE_UNDERFLOW | FE_INEXACT);
    if (ex) {
      if (ex & FE_DIVBYZERO) bits |= fpscr_bits::ZX;
      if (ex & FE_OVERFLOW) bits |= fpscr_bits::OX;
      if (ex & FE_UNDERFLOW) bits |= fpscr_bits::UX;
      if (ex & FE_INEXACT) bits |= fpscr_bits::XX;
    }
    std::feclearexcept(FE_ALL_EXCEPT);
#endif
    if (bits && !fp_legacy_status) {
      if (bits & ~fpscr) fpscr |= fpscr_bits::FX;
      fpscr |= bits;
    }
    fp_commit_fprf();
    fpscr_recompute_summaries(fpscr);
  }
  [[nodiscard]] std::uint32_t observe_fpscr() noexcept { fp_sync(); return fpscr; }
  // A whole-register write replaces whatever was pending.
  void write_fpscr(std::uint32_t value) noexcept { fp_sync(); fpscr = value; }
  // A VX* invalid-operation sticky bit, set exactly where it is detected.
  void raise_fp_invalid(std::uint32_t detail) noexcept {
    if (detail & ~fpscr) fpscr |= fpscr_bits::FX;
    fpscr |= detail;
    fpscr_recompute_summaries(fpscr);
  }

  [[nodiscard]] double fpr(std::size_t i) const noexcept {
    double v{};
    std::memcpy(&v, &fpr_bits[i], sizeof(v));
    return v;
  }
  void set_fpr(std::size_t i, double v) noexcept {
    std::memcpy(&fpr_bits[i], &v, sizeof(v));
  }

  // Inline: generated code tests a CR bit at every conditional branch, and
  // the definitions in state.cpp made each test a call across modules.
  [[nodiscard]] std::uint8_t cr_field(unsigned field) const noexcept {
    const unsigned shift = (7u - (field & 7u)) * 4u;
    return static_cast<std::uint8_t>((cr >> shift) & 0xFu);
  }
  void set_cr_field(unsigned field, std::uint8_t value) noexcept {
    const unsigned shift = (7u - (field & 7u)) * 4u;
    cr = (cr & ~(0xFu << shift)) |
         ((static_cast<std::uint32_t>(value) & 0xFu) << shift);
  }
  [[nodiscard]] bool cr_bit(unsigned bit) const noexcept {
    return (cr & (0x80000000u >> (bit & 31u))) != 0;
  }
  void set_cr_bit(unsigned bit, bool value) noexcept {
    const auto mask = 0x80000000u >> (bit & 31u);
    cr = value ? (cr | mask) : (cr & ~mask);
  }

  [[nodiscard]] bool xer_ca() const noexcept { return (xer & xer_bits::CA) != 0; }
  [[nodiscard]] bool xer_ov() const noexcept { return (xer & xer_bits::OV) != 0; }
  [[nodiscard]] bool xer_so() const noexcept { return (xer & xer_bits::SO) != 0; }
  void set_xer_ca(bool v) noexcept {
    xer = v ? (xer | xer_bits::CA) : (xer & ~xer_bits::CA);
  }
  void set_xer_ov(bool v) noexcept {
    xer = v ? (xer | xer_bits::OV) : (xer & ~xer_bits::OV);
  }
  void set_xer_so(bool v) noexcept {
    xer = v ? (xer | xer_bits::SO) : (xer & ~xer_bits::SO);
  }
  void set_xer_overflow(bool overflow) noexcept {
    set_xer_ov(overflow);
    if (overflow) set_xer_so(true);
  }
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

  // Record forms ("add.", "addic.", "rlwinm.", ...) compare the low 32 bits of
  // the result with zero: titles run with 32-bit mode CR0 semantics, as xenia's
  // UpdateCR (Truncate to INT32) and XenonRecomp's compare<int32_t> both do.
  // Code relies on it: a word loaded with lwz is zero-extended, so 0xFFFFFFFF
  // decremented by "addic. -1" must read as negative, not as 0x0_FFFFFFFE.
  void update_cr0_signed(std::uint64_t result) noexcept {
    const auto signed_result = static_cast<std::int32_t>(static_cast<std::uint32_t>(result));
    const std::uint8_t field = static_cast<std::uint8_t>(
        (signed_result < 0 ? 0x8u : signed_result > 0 ? 0x4u : 0x2u) |
        (xer_so() ? 0x1u : 0u));
    set_cr_field(0, field);
  }
  void update_cr1_from_fpscr() noexcept;
};

// Brackets host code that runs on a guest thread (kernel imports, runtime
// services): the guest's pending status is folded before it, and whatever
// sticky flags the host code raised are discarded after it (invariant I2).
class FpHostBoundary {
 public:
  explicit FpHostBoundary(CpuState& state) noexcept { state.fp_sync(); }
  FpHostBoundary(const FpHostBoundary&) = delete;
  FpHostBoundary& operator=(const FpHostBoundary&) = delete;
  ~FpHostBoundary() {
#if defined(_M_X64) || defined(__x86_64__)
    const unsigned csr = _mm_getcsr();
    if (csr & 0x3Fu) _mm_setcsr(csr & ~0x3Fu);
#else
    std::feclearexcept(FE_ALL_EXCEPT);
#endif
  }
};

static_assert(alignof(CpuState) >= 64);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace xenon::cpu

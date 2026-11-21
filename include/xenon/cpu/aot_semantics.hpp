#pragma once

#include <algorithm>
#include <bit>
#include <cfenv>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>

#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

#if defined(__AVX2__) || defined(__FMA__)
#include <immintrin.h>
#endif
#include "xenon/cpu/runtime.hpp"
#include "xenon/cpu/vector_semantic.hpp"

namespace xenon::cpu::aot {

// Generated functions with promoted registers (CppAotBackend) write them
// back to CpuState on every exit - returns and exceptions unwinding through
// the function alike - by running `flush` here.
template <typename Flush>
struct FlushOnExit {
  Flush& flush;
  ~FlushOnExit() { flush(); }
};

// One-entry cache of an indirect call or branch site (`bctr`, `bctrl`,
// `bclr`-through-register, vtable and function-pointer calls): the guest
// target it last resolved to and the native function that resolved to. A
// generated site declares one `static thread_local IndirectSite` and resolves
// through lookup_cached().
struct IndirectSite {
  GuestAddress target{};
  NativeCompiledEntry entry{};
  std::uint64_t generation{};
  CompiledLookupCallback lookup{};
  void* registry{};
};

// context.lookup_compiled(target, kind), answered from the site's cache when
// the same target was resolved by the same lookup (callback and registry) and
// the host's dispatch generation has not moved since. Only a native function
// is cached - a miss always goes through the lookup, so its miss observer and
// the caller's dynamic-fallback path still run. A context with no dispatch
// generation (tests, tools, a module's own wrapper context) always looks up.
inline NativeCompiledEntry lookup_cached(IndirectSite& site, ExecutionContext& context,
                                         GuestAddress target, CompiledLookupKind kind) {
  const auto* counter = context.dispatch_generation;
  if (!counter) [[unlikely]] return context.lookup_compiled(target, kind);
  // Read before the lookup: a bump racing it leaves the entry stamped with the
  // older generation, so the next call looks up again.
  const auto generation = counter->load(std::memory_order_acquire);
  if (site.entry && site.target == target && site.generation == generation &&
      site.lookup == context.compiled_lookup && site.registry == context.compiled_registry)
      [[likely]]
    return site.entry;
  auto* entry = context.lookup_compiled(target, kind);
  if (entry) site = {target, entry, generation, context.compiled_lookup, context.compiled_registry};
  return entry;
}

// The guest lowered its hardware thread's priority inside a spin-wait
// (`or 1,1,1` / `or 6,6,6` / `or 31,31,31`): tell the host core the same, so
// its sibling thread gets the pipeline while this one polls.
inline void spin_hint() noexcept {
#if defined(_M_X64) || defined(__x86_64__)
  _mm_pause();
#elif defined(_M_ARM64)
  __yield();
#elif defined(__aarch64__)
  __asm__ __volatile__("yield");
#endif
}

template <VectorSemantic S>
[[nodiscard]] inline Vector128 vector_logic(const Vector128& a,
                                            const Vector128& b) noexcept {
  constexpr bool is_and = S == VectorSemantic::vand || S == VectorSemantic::vand128;
  constexpr bool is_andc = S == VectorSemantic::vandc || S == VectorSemantic::vandc128;
  constexpr bool is_or = S == VectorSemantic::vor || S == VectorSemantic::vor128;
  constexpr bool is_xor = S == VectorSemantic::vxor || S == VectorSemantic::vxor128;
  constexpr bool is_nor = S == VectorSemantic::vnor || S == VectorSemantic::vnor128;
  static_assert(is_and || is_andc || is_or || is_xor || is_nor);
  const auto lhs = std::bit_cast<std::array<std::uint64_t, 2>>(a);
  const auto rhs = std::bit_cast<std::array<std::uint64_t, 2>>(b);
  std::array<std::uint64_t, 2> result{};
  for (unsigned lane = 0; lane < 2; ++lane) {
    if constexpr (is_and) result[lane] = lhs[lane] & rhs[lane];
    else if constexpr (is_andc) result[lane] = lhs[lane] & ~rhs[lane];
    else if constexpr (is_or) result[lane] = lhs[lane] | rhs[lane];
    else if constexpr (is_xor) result[lane] = lhs[lane] ^ rhs[lane];
    else result[lane] = ~(lhs[lane] | rhs[lane]);
  }
  return std::bit_cast<Vector128>(result);
}

[[nodiscard]] inline Vector128 vector_select(const Vector128& a,
                                              const Vector128& b,
                                              const Vector128& mask) noexcept {
  const auto lhs = std::bit_cast<std::array<std::uint64_t, 2>>(a);
  const auto rhs = std::bit_cast<std::array<std::uint64_t, 2>>(b);
  const auto bits = std::bit_cast<std::array<std::uint64_t, 2>>(mask);
  std::array<std::uint64_t, 2> result{};
  for (unsigned lane = 0; lane < 2; ++lane)
    result[lane] = (lhs[lane] & ~bits[lane]) | (rhs[lane] & bits[lane]);
  return std::bit_cast<Vector128>(result);
}

// Common vector splat operations - inline rather than runtime dispatch
template <VectorSemantic S>
[[nodiscard]] inline Vector128 vector_splat_byte(const Vector128& src, unsigned imm) noexcept {
  Vector128 r{};
  const std::uint8_t value = src.bytes[imm & 15u];
  std::fill(r.bytes.begin(), r.bytes.end(), value);
  return r;
}

template <VectorSemantic S>
[[nodiscard]] inline Vector128 vector_splat_halfword(const Vector128& src, unsigned imm) noexcept {
  Vector128 r{};
  const std::uint16_t value = src.u16_be(imm & 7u);
  for (unsigned i = 0; i < 8; ++i) r.set_u16_be(i, value);
  return r;
}

template <VectorSemantic S>
[[nodiscard]] inline Vector128 vector_splat_word(const Vector128& src, unsigned imm) noexcept {
  Vector128 r{};
  const std::uint32_t value = src.u32_be(imm & 3u);
  for (unsigned i = 0; i < 4; ++i) r.set_u32_be(i, value);
  return r;
}

template <VectorSemantic S>
[[nodiscard]] inline Vector128 vector_splat_immediate_byte(unsigned imm) noexcept {
  Vector128 r{};
  const auto value = static_cast<std::uint8_t>(static_cast<std::int8_t>((imm & 31u) << 3) >> 3);
  std::fill(r.bytes.begin(), r.bytes.end(), value);
  return r;
}

template <VectorSemantic S>
[[nodiscard]] inline Vector128 vector_splat_immediate_halfword(unsigned imm) noexcept {
  Vector128 r{};
  const auto value = static_cast<std::uint16_t>(static_cast<std::int16_t>((imm & 31u) << 11) >> 11);
  for (unsigned i = 0; i < 8; ++i) r.set_u16_be(i, value);
  return r;
}

template <VectorSemantic S>
[[nodiscard]] inline Vector128 vector_splat_immediate_word(unsigned imm) noexcept {
  Vector128 r{};
  const auto value = static_cast<std::uint32_t>(static_cast<std::int32_t>((imm & 31u) << 27) >> 27);
  for (unsigned i = 0; i < 4; ++i) r.set_u32_be(i, value);
  return r;
}

// Modular integer lane arithmetic - inline per-lane ops
template <typename T>
[[nodiscard]] inline Vector128 vector_add_modular(const Vector128& a, const Vector128& b) noexcept {
  Vector128 r{};
  constexpr unsigned n = 16u / sizeof(T);
  for (unsigned i = 0; i < n; ++i) {
    T x, y;
    if constexpr (sizeof(T) == 1) { x = a.bytes[i]; y = b.bytes[i]; }
    else if constexpr (sizeof(T) == 2) { x = std::bit_cast<T>(a.u16_be(i)); y = std::bit_cast<T>(b.u16_be(i)); }
    else { x = std::bit_cast<T>(a.u32_be(i)); y = std::bit_cast<T>(b.u32_be(i)); }
    const T result = static_cast<T>(x + y);
    if constexpr (sizeof(T) == 1) r.bytes[i] = result;
    else if constexpr (sizeof(T) == 2) r.set_u16_be(i, std::bit_cast<std::uint16_t>(result));
    else r.set_u32_be(i, std::bit_cast<std::uint32_t>(result));
  }
  return r;
}

template <typename T>
[[nodiscard]] inline Vector128 vector_sub_modular(const Vector128& a, const Vector128& b) noexcept {
  Vector128 r{};
  constexpr unsigned n = 16u / sizeof(T);
  for (unsigned i = 0; i < n; ++i) {
    T x, y;
    if constexpr (sizeof(T) == 1) { x = a.bytes[i]; y = b.bytes[i]; }
    else if constexpr (sizeof(T) == 2) { x = std::bit_cast<T>(a.u16_be(i)); y = std::bit_cast<T>(b.u16_be(i)); }
    else { x = std::bit_cast<T>(a.u32_be(i)); y = std::bit_cast<T>(b.u32_be(i)); }
    const T result = static_cast<T>(x - y);
    if constexpr (sizeof(T) == 1) r.bytes[i] = result;
    else if constexpr (sizeof(T) == 2) r.set_u16_be(i, std::bit_cast<std::uint16_t>(result));
    else r.set_u32_be(i, std::bit_cast<std::uint32_t>(result));
  }
  return r;
}

template <typename T>
[[nodiscard]] constexpr T add_wrap(T a, T b) noexcept {
  using U = std::make_unsigned_t<T>;
  return static_cast<T>(static_cast<U>(a) + static_cast<U>(b));
}

[[nodiscard]] inline std::uint64_t add_carry(std::uint64_t a, std::uint64_t b,
                                             bool carry) noexcept {
  return a + b + static_cast<std::uint64_t>(carry);
}
// The add/subtract family's XER.CA and XER.OV follow 32-bit mode, as on the
// console: CA is the carry out of the low word and OV the signed overflow of
// the low word (xenia's AddDidCarry/SubDidCarry truncate to INT32, XenonRecomp
// compares u32). The register still receives the full 64-bit sum.
[[nodiscard]] inline bool carry_out(std::uint64_t a, std::uint64_t b,
                                    bool carry) noexcept {
  const std::uint64_t sum = std::uint64_t{static_cast<std::uint32_t>(a)} +
                            static_cast<std::uint32_t>(b) + static_cast<std::uint64_t>(carry);
  return (sum >> 32) != 0;
}

[[nodiscard]] inline bool signed_add_overflow(std::uint64_t a, std::uint64_t b,
                                              bool carry = false) noexcept {
  const auto r = static_cast<std::uint32_t>(add_carry(a, b, carry));
  const auto a32 = static_cast<std::uint32_t>(a);
  const auto b32 = static_cast<std::uint32_t>(b);
  return ((a32 ^ r) & (b32 ^ r) & 0x80000000u) != 0;
}

[[nodiscard]] inline std::uint64_t mul_hi_unsigned(std::uint64_t a,
                                                   std::uint64_t b) noexcept {
  const std::uint64_t a0 = static_cast<std::uint32_t>(a), a1 = a >> 32;
  const std::uint64_t b0 = static_cast<std::uint32_t>(b), b1 = b >> 32;
  const std::uint64_t p00 = a0*b0;
  const std::uint64_t p01 = a0*b1;
  const std::uint64_t p10 = a1*b0;
  const std::uint64_t p11 = a1*b1;
  const std::uint64_t middle = (p00>>32) + static_cast<std::uint32_t>(p01) +
                               static_cast<std::uint32_t>(p10);
  return p11 + (p01>>32) + (p10>>32) + (middle>>32);
}

[[nodiscard]] inline std::uint64_t mul_hi_signed(std::int64_t a,
                                                 std::int64_t b) noexcept {
  const auto ua = static_cast<std::uint64_t>(a);
  const auto ub = static_cast<std::uint64_t>(b);
  std::uint64_t hi = mul_hi_unsigned(ua, ub);
  if (a < 0) hi -= ub;
  if (b < 0) hi -= ua;
  return hi;
}

[[nodiscard]] inline std::uint64_t div_signed(std::uint64_t a, std::uint64_t b,
                                              unsigned width) noexcept {
  if (width == 32) {
    const auto x = static_cast<std::int32_t>(a);
    const auto y = static_cast<std::int32_t>(b);
    if (y == 0 || (x == std::numeric_limits<std::int32_t>::min() && y == -1)) return 0;
    return static_cast<std::uint32_t>(x / y);
  }
  const auto x = static_cast<std::int64_t>(a);
  const auto y = static_cast<std::int64_t>(b);
  if (y == 0 || (x == std::numeric_limits<std::int64_t>::min() && y == -1)) return 0;
  return static_cast<std::uint64_t>(x / y);
}

[[nodiscard]] inline std::uint64_t div_unsigned(std::uint64_t a, std::uint64_t b,
                                                unsigned width) noexcept {
  if (width == 32) {
    const auto x = static_cast<std::uint32_t>(a), y = static_cast<std::uint32_t>(b);
    return y ? static_cast<std::uint32_t>(x / y) : 0;
  }
  return b ? a / b : 0;
}

[[nodiscard]] inline bool div_overflow(std::uint64_t a, std::uint64_t b,
                                       unsigned width, bool uns) noexcept {
  if (uns) return width == 32 ? static_cast<std::uint32_t>(b)==0 : b==0;
  if (width == 32) {
    const auto x=static_cast<std::int32_t>(a), y=static_cast<std::int32_t>(b);
    return y==0 || (x==std::numeric_limits<std::int32_t>::min() && y==-1);
  }
  const auto x=static_cast<std::int64_t>(a), y=static_cast<std::int64_t>(b);
  return y==0 || (x==std::numeric_limits<std::int64_t>::min() && y==-1);
}

[[nodiscard]] inline std::uint64_t ppc_shift(std::string_view mnemonic,
                                             std::uint64_t value,
                                             std::uint64_t shift) noexcept {
  if (mnemonic == "slwx") {
    const unsigned sh = unsigned(shift & 0x3Fu);
    return sh >= 32 ? 0 : std::uint64_t(std::uint32_t(value) << sh);
  }
  if (mnemonic == "srwx") {
    const unsigned sh = unsigned(shift & 0x3Fu);
    return sh >= 32 ? 0 : std::uint64_t(std::uint32_t(value) >> sh);
  }
  if (mnemonic == "srawx" || mnemonic == "srawix") {
    const unsigned sh = unsigned(shift & 0x3Fu);
    const auto v = static_cast<std::int32_t>(value);
    const auto r = sh >= 32 ? (v < 0 ? -1 : 0) : (v >> sh);
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(r));
  }
  if (mnemonic == "sldx") {
    const unsigned sh = unsigned(shift & 0x7Fu);
    return sh >= 64 ? 0 : value << sh;
  }
  if (mnemonic == "srdx") {
    const unsigned sh = unsigned(shift & 0x7Fu);
    return sh >= 64 ? 0 : value >> sh;
  }
  if (mnemonic == "sradx" || mnemonic == "sradix") {
    const unsigned sh = unsigned(shift & 0x7Fu);
    const auto v = static_cast<std::int64_t>(value);
    return static_cast<std::uint64_t>(sh >= 64 ? (v < 0 ? -1 : 0) : (v >> sh));
  }
  return value;
}

[[nodiscard]] inline bool ppc_shift_carry(std::string_view mnemonic,
                                          std::uint64_t value,
                                          std::uint64_t shift) noexcept {
  if (mnemonic == "srawx" || mnemonic == "srawix") {
    const unsigned sh = unsigned(shift & 0x3Fu);
    const auto v = static_cast<std::int32_t>(value);
    if (v >= 0 || sh == 0) return false;
    if (sh >= 32) return true;
    const std::uint32_t mask=(std::uint32_t{1}<<sh)-1u;
    return (static_cast<std::uint32_t>(v)&mask)!=0;
  }
  if (mnemonic == "sradx" || mnemonic == "sradix") {
    const unsigned sh = unsigned(shift & 0x7Fu);
    const auto v = static_cast<std::int64_t>(value);
    if (v >= 0 || sh == 0) return false;
    if (sh >= 64) return true;
    const std::uint64_t mask=(std::uint64_t{1}<<sh)-1u;
    return (static_cast<std::uint64_t>(v)&mask)!=0;
  }
  return false;
}

inline void write_cr_compare(CpuState& state, unsigned field, std::int64_t a,
                             std::int64_t b, bool is_signed) noexcept {
  std::uint8_t nibble{};
  if (is_signed) nibble = a < b ? 0x8u : (a > b ? 0x4u : 0x2u);
  else {
    const auto ua=static_cast<std::uint64_t>(a), ub=static_cast<std::uint64_t>(b);
    nibble=ua<ub?0x8u:(ua>ub?0x4u:0x2u);
  }
  if(state.xer_so()) nibble|=1u;
  state.set_cr_field(field,nibble);
}

inline void update_cr6_from_vector_compare(CpuState& state, const Vector128& v) noexcept {
  bool all_true=true, all_false=true;
  for(auto b:v.bytes){ all_true &= b==0xFFu; all_false &= b==0; }
  std::uint8_t cr6=0;
  if(all_true) cr6|=0x8u;
  if(all_false) cr6|=0x2u;
  state.set_cr_field(6,cr6);
}


inline constexpr std::uint32_t fpscr_invalid_detail_mask =
    fpscr_bits::VXSNAN | fpscr_bits::VXISI | fpscr_bits::VXIDI |
    fpscr_bits::VXZDZ | fpscr_bits::VXIMZ | fpscr_bits::VXVC |
    fpscr_bits::VXSOFT | fpscr_bits::VXSQRT | fpscr_bits::VXCVI;

inline void recompute_fpscr_summaries(CpuState& state) noexcept {
  if (state.fpscr & fpscr_invalid_detail_mask) state.fpscr |= fpscr_bits::VX;
  else state.fpscr &= ~fpscr_bits::VX;

  const bool enabled =
      ((state.fpscr & fpscr_bits::VX) && (state.fpscr & fpscr_bits::VE)) ||
      ((state.fpscr & fpscr_bits::OX) && (state.fpscr & fpscr_bits::OE)) ||
      ((state.fpscr & fpscr_bits::UX) && (state.fpscr & fpscr_bits::UE)) ||
      ((state.fpscr & fpscr_bits::ZX) && (state.fpscr & fpscr_bits::ZE)) ||
      ((state.fpscr & fpscr_bits::XX) && (state.fpscr & fpscr_bits::XE));
  if (enabled) state.fpscr |= fpscr_bits::FEX;
  else state.fpscr &= ~fpscr_bits::FEX;
}

[[nodiscard]] inline bool is_signaling_nan(double v) noexcept {
  const auto bits = std::bit_cast<std::uint64_t>(v);
  const auto exp = bits & 0x7FF0000000000000ull;
  const auto frac = bits & 0x000FFFFFFFFFFFFFull;
  const auto quiet = bits & 0x0008000000000000ull;
  return exp == 0x7FF0000000000000ull && frac != 0 && quiet == 0;
}


[[nodiscard]] inline double fp_abs_bits(double v) noexcept {
  return std::bit_cast<double>(std::bit_cast<std::uint64_t>(v) & 0x7FFFFFFFFFFFFFFFull);
}
[[nodiscard]] inline double fp_neg_bits(double v) noexcept {
  return std::bit_cast<double>(std::bit_cast<std::uint64_t>(v) ^ 0x8000000000000000ull);
}
[[nodiscard]] inline double fp_select(double condition, double nonnegative,
                                      double negative_or_nan) noexcept {
  const auto bits=std::bit_cast<std::uint64_t>(condition);
  const auto mag=bits & 0x7FFFFFFFFFFFFFFFull;
  const bool nan=((mag & 0x7FF0000000000000ull)==0x7FF0000000000000ull) &&
                 ((mag & 0x000FFFFFFFFFFFFFull)!=0);
  if(nan) return negative_or_nan;
  if(mag==0) return nonnegative; // both +0 and -0 compare equal to zero.
  return (bits>>63)==0 ? nonnegative : negative_or_nan;
}

inline int host_rounding(FpRoundingMode m) noexcept;

inline thread_local int last_fp_exceptions = 0;
inline thread_local std::uint32_t last_fp_invalid_detail = 0;
inline thread_local bool last_fp_result_incremented = false;

// Each guest FP operation runs with the guest's rounding mode and clear
// exception flags, then hands the host its own state back. On x64 all scalar
// double/float arithmetic is SSE, so that state is exactly MXCSR: reading and
// writing it directly costs a few cycles, where fegetenv/fesetenv also save
// and reload the x87 environment and made FP-heavy guest code (AC6's audio
// mixer) spend half its time in the CRT.
#if defined(_M_X64) || defined(__x86_64__)
inline thread_local unsigned saved_host_mxcsr = 0;

[[nodiscard]] inline unsigned mxcsr_rounding(FpRoundingMode m) noexcept {
  switch (m) {
    case FpRoundingMode::Nearest: return 0x0000u;
    case FpRoundingMode::TowardNegative: return 0x2000u;
    case FpRoundingMode::TowardPositive: return 0x4000u;
    case FpRoundingMode::TowardZero: return 0x6000u;
  }
  return 0x0000u;
}

inline void begin_fp_operation(CpuState& state) noexcept {
  last_fp_exceptions = 0;
  last_fp_invalid_detail = 0;
  last_fp_result_incremented = false;
  saved_host_mxcsr = _mm_getcsr();
  // Clear the six exception flags and replace the rounding control; the
  // host's masks, flush-to-zero and denormals-are-zero settings are kept.
  // NI (non-IEEE mode) flushes denormal inputs and results to zero.
  _mm_setcsr((saved_host_mxcsr & ~(0x003Fu | 0x6000u)) | ((state.fpscr & fpscr_bits::NI) ? 0x8040u : 0u) |
             mxcsr_rounding(state.fp_rounding_mode()));
}

inline void finish_fp_operation() noexcept {
  const unsigned flags = _mm_getcsr();
  _mm_setcsr(saved_host_mxcsr);
  last_fp_exceptions = ((flags & 0x01u) ? FE_INVALID : 0) | ((flags & 0x04u) ? FE_DIVBYZERO : 0) |
                       ((flags & 0x08u) ? FE_OVERFLOW : 0) | ((flags & 0x10u) ? FE_UNDERFLOW : 0) |
                       ((flags & 0x20u) ? FE_INEXACT : 0);
}
#else
inline thread_local fenv_t saved_host_fp_environment{};
inline thread_local bool saved_host_fp_environment_valid = false;

inline void begin_fp_operation(CpuState& state) noexcept {
  last_fp_exceptions = 0;
  last_fp_invalid_detail = 0;
  last_fp_result_incremented = false;
  saved_host_fp_environment_valid = std::fegetenv(&saved_host_fp_environment) == 0;
  std::feclearexcept(FE_ALL_EXCEPT);
  // Set every time: finish_fp_operation() restores the host's rounding.
  std::fesetround(host_rounding(state.fp_rounding_mode()));
}

inline void finish_fp_operation() noexcept {
  last_fp_exceptions = std::fetestexcept(FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW | FE_UNDERFLOW | FE_INEXACT);
  if (saved_host_fp_environment_valid) {
    std::fesetenv(&saved_host_fp_environment);
    saved_host_fp_environment_valid = false;
  }
}
#endif

// Exact floating-point status in two tiers.
//
// Tier A (the default whenever the guest's FPSCR control bits RN, NI and the
// five exception enables are all zero - what every title runs with): each
// helper below is the plain host instruction plus a guard on the result.
//   * ZX/OX/UX/XX are not tracked per operation. The host's sticky MXCSR flags
//     accumulate them and CpuState::fp_sync() folds them into FPSCR at every
//     observation point (mffs, mcrfs, mtfsf, mtfsb*, Rc=1 CR1 updates, the host
//     boundary, thread/context code). Host and guest agree on these exactly:
//     both raise them for the same operation under round-to-nearest.
//   * VX* detail needs to know why a NaN appeared, so a NaN result takes a
//     cold path (fp_note_nan_result) that classifies it from the operands.
//   * FPRF costs one store: update_fpscr() remembers the result bits and the
//     fold classifies them.
// Tier B (an enabled exception, a non-nearest rounding mode, NI, or
// XENON_FP_STRICT=1) is the exact per-operation path: the host's exception
// flags are read around every operation, FPSCR is updated immediately, and
// NI maps to flush-to-zero/denormals-are-zero. Entering Tier B first folds
// any pending Tier A status so the two never interleave.
//
// XENON_FP_EXACT_STATUS=0 restores the previous behaviour: the default native
// path never updates FPSCR status (only a non-nearest rounding mode leaves it).
// The flags are read when each module loads, so the runtime and a game module
// always agree; a test may assign them before running guest code.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)  // getenv: read once, never written
#endif
inline bool fp_strict_mode = [] {
  const char* value = std::getenv("XENON_FP_STRICT");
  return value && *value && *value != '0';
}();
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// Tier A?
[[nodiscard]] inline bool fp_native(const CpuState& state) noexcept {
  return !fp_strict_mode && (state.fpscr & (fp_legacy_status ? 0x3u : 0xFFu)) == 0;
}

// The strict path reads the host's exception flags around one operation, so
// that operation has to happen at run time. Once a helper is inlined with
// constant operands the compiler can fold "a + b" away, raising no flag (a
// strict-mode test lost FE_INEXACT that way); a volatile read of each operand
// keeps it a real instruction. The result store is volatile for the same reason.
template <typename T>
[[nodiscard]] inline T fp_operand(T value) noexcept {
  volatile T held = value;
  return held;
}

enum class FpOpKind : std::uint8_t { AddSub, Mul, Div, Sqrt, Fma, Rsqrt };

// Cold path: an operation returned a NaN. Records VXSNAN when an operand was
// signalling, nothing when a quiet NaN was merely propagated, and the
// operation's own VX* bit when the NaN was created from non-NaN operands.
inline void fp_note_nan_result(CpuState& state, FpOpKind kind, double a, double b = 0.0,
                               double c = 0.0) noexcept {
  if (fp_legacy_status) return;
  const bool has_c = kind == FpOpKind::Fma;
  if (is_signaling_nan(a) || is_signaling_nan(b) || (has_c && is_signaling_nan(c))) {
    state.raise_fp_invalid(fpscr_bits::VXSNAN);
    return;
  }
  if (std::isnan(a) || std::isnan(b) || (has_c && std::isnan(c))) return;
  std::uint32_t detail = fpscr_bits::VXSOFT;
  switch (kind) {
    case FpOpKind::AddSub: detail = fpscr_bits::VXISI; break;
    case FpOpKind::Mul: detail = fpscr_bits::VXIMZ; break;
    case FpOpKind::Div:
      if (a == 0.0 && b == 0.0) detail = fpscr_bits::VXZDZ;
      else if (std::isinf(a) && std::isinf(b)) detail = fpscr_bits::VXIDI;
      break;
    case FpOpKind::Sqrt:
    case FpOpKind::Rsqrt:
      detail = a < 0.0 ? fpscr_bits::VXSQRT : fpscr_bits::VXSOFT;
      break;
    case FpOpKind::Fma:
      detail = ((a == 0.0 && std::isinf(b)) || (b == 0.0 && std::isinf(a)))
                   ? fpscr_bits::VXIMZ : fpscr_bits::VXISI;
      break;
  }
  state.raise_fp_invalid(detail);
}

// Tier A result guard: a NaN is the only way an arithmetic helper creates or
// forwards invalid-operation status.
#define XENON_FP_GUARD(STATE, KIND, R, ...)                                   \
  do { if (R != R) [[unlikely]] fp_note_nan_result(STATE, KIND, __VA_ARGS__); } while (0)

[[nodiscard]] inline double fp_add(CpuState& state, double a, double b) noexcept {
  if (fp_native(state)) {
    const double r = a + b;
    XENON_FP_GUARD(state, FpOpKind::AddSub, r, a, b);
    return r;
  }
  state.fp_sync();
  begin_fp_operation(state);
  if (is_signaling_nan(a) || is_signaling_nan(b)) last_fp_invalid_detail |= fpscr_bits::VXSNAN;
  volatile double r = fp_operand(a) + fp_operand(b);
  finish_fp_operation();
  if ((last_fp_exceptions & FE_INVALID) && !last_fp_invalid_detail) last_fp_invalid_detail |= fpscr_bits::VXISI;
  return r;
}
[[nodiscard]] inline double fp_sub(CpuState& state, double a, double b) noexcept {
  if (fp_native(state)) {
    const double r = a - b;
    XENON_FP_GUARD(state, FpOpKind::AddSub, r, a, b);
    return r;
  }
  state.fp_sync();
  begin_fp_operation(state);
  if (is_signaling_nan(a) || is_signaling_nan(b)) last_fp_invalid_detail |= fpscr_bits::VXSNAN;
  volatile double r = fp_operand(a) - fp_operand(b);
  finish_fp_operation();
  if ((last_fp_exceptions & FE_INVALID) && !last_fp_invalid_detail) last_fp_invalid_detail |= fpscr_bits::VXISI;
  return r;
}
[[nodiscard]] inline double fp_mul(CpuState& state, double a, double b) noexcept {
  if (fp_native(state)) {
    const double r = a * b;
    XENON_FP_GUARD(state, FpOpKind::Mul, r, a, b);
    return r;
  }
  state.fp_sync();
  begin_fp_operation(state);
  if (is_signaling_nan(a) || is_signaling_nan(b)) last_fp_invalid_detail |= fpscr_bits::VXSNAN;
  volatile double r = fp_operand(a) * fp_operand(b);
  finish_fp_operation();
  if ((last_fp_exceptions & FE_INVALID) && !last_fp_invalid_detail) last_fp_invalid_detail |= fpscr_bits::VXIMZ;
  return r;
}
[[nodiscard]] inline double fp_div(CpuState& state, double a, double b) noexcept {
  if (fp_native(state)) {
    const double r = a / b;
    XENON_FP_GUARD(state, FpOpKind::Div, r, a, b);
    return r;
  }
  state.fp_sync();
  begin_fp_operation(state);
  if (is_signaling_nan(a) || is_signaling_nan(b)) last_fp_invalid_detail |= fpscr_bits::VXSNAN;
  volatile double r = fp_operand(a) / fp_operand(b);
  finish_fp_operation();
  if ((last_fp_exceptions & FE_INVALID) && !last_fp_invalid_detail) {
    if (a == 0.0 && b == 0.0) last_fp_invalid_detail |= fpscr_bits::VXZDZ;
    else if (std::isinf(a) && std::isinf(b)) last_fp_invalid_detail |= fpscr_bits::VXIDI;
    else last_fp_invalid_detail |= fpscr_bits::VXSOFT;
  }
  return r;
}
[[nodiscard]] inline double fp_sqrt(CpuState& state, double a) noexcept {
  if (fp_native(state)) {
    const double r = std::sqrt(a);
    XENON_FP_GUARD(state, FpOpKind::Sqrt, r, a);
    return r;
  }
  state.fp_sync();
  begin_fp_operation(state);
  if (is_signaling_nan(a)) last_fp_invalid_detail |= fpscr_bits::VXSNAN;
  volatile double r = std::sqrt(fp_operand(a));
  finish_fp_operation();
  if ((last_fp_exceptions & FE_INVALID) && !last_fp_invalid_detail) last_fp_invalid_detail |= fpscr_bits::VXSQRT;
  return r;
}
[[nodiscard]] inline double fp_fma(CpuState& state, double a, double b, double c) noexcept {
  if (fp_native(state)) {
#if defined(__AVX2__) || defined(__FMA__)
    // The FMA instruction itself: std::fma is a call into the C runtime here,
    // in AC6's audio mixer and every other title's float-heavy loops. One
    // rounding either way, so the result is the same.
    const double r = _mm_cvtsd_f64(_mm_fmadd_sd(_mm_set_sd(a), _mm_set_sd(b), _mm_set_sd(c)));
#else
    const double r = std::fma(a, b, c);
#endif
    XENON_FP_GUARD(state, FpOpKind::Fma, r, a, b, c);
    return r;
  }
  state.fp_sync();
  begin_fp_operation(state);
  if (is_signaling_nan(a) || is_signaling_nan(b) || is_signaling_nan(c)) last_fp_invalid_detail |= fpscr_bits::VXSNAN;
  volatile double r = std::fma(fp_operand(a), fp_operand(b), fp_operand(c));
  finish_fp_operation();
  if ((last_fp_exceptions & FE_INVALID) && !last_fp_invalid_detail) {
    if ((a == 0.0 && std::isinf(b)) || (b == 0.0 && std::isinf(a))) last_fp_invalid_detail |= fpscr_bits::VXIMZ;
    else last_fp_invalid_detail |= fpscr_bits::VXISI;
  }
  return r;
}
[[nodiscard]] inline double fp_reciprocal(CpuState& state, double a) noexcept {
  return fp_div(state, 1.0, a);
}
[[nodiscard]] inline double fp_rsqrt(CpuState& state, double a) noexcept {
  if (fp_native(state)) {
    const double r = 1.0 / std::sqrt(a);
    XENON_FP_GUARD(state, FpOpKind::Rsqrt, r, a);
    return r;
  }
  state.fp_sync();
  begin_fp_operation(state);
  if (is_signaling_nan(a)) last_fp_invalid_detail |= fpscr_bits::VXSNAN;
  volatile double root = std::sqrt(fp_operand(a));
  volatile double r = 1.0 / root;
  finish_fp_operation();
  if ((last_fp_exceptions & FE_INVALID) && !last_fp_invalid_detail) {
    last_fp_invalid_detail |= a < 0.0 ? fpscr_bits::VXSQRT : fpscr_bits::VXSOFT;
  }
  return r;
}
[[nodiscard]] inline double fp_round_single(CpuState& state, double a) noexcept {
  if (fp_native(state)) {
    // cvtsd2ss raises the host's OE/UE/PE flags exactly as frsp does.
    const double r = static_cast<double>(static_cast<float>(a));
    if (a != a) [[unlikely]] {
      if (!fp_legacy_status && is_signaling_nan(a)) state.raise_fp_invalid(fpscr_bits::VXSNAN);
    }
    return r;
  }
  // This helper is used both by frsp and as the final rounding step of the
  // single-precision arithmetic instructions. Preserve status captured by a
  // preceding arithmetic helper in the same guest instruction and merge the
  // exceptions raised by the narrowing conversion.
  const int pending_exceptions = last_fp_exceptions;
  const std::uint32_t pending_invalid = last_fp_invalid_detail;
  const bool pending_incremented = last_fp_result_incremented;

  state.fp_sync();
  begin_fp_operation(state);
  if (is_signaling_nan(a)) last_fp_invalid_detail |= fpscr_bits::VXSNAN;
  volatile float f = static_cast<float>(fp_operand(a));
  volatile double r = static_cast<double>(f);
  finish_fp_operation();

  if (std::isfinite(a) && static_cast<double>(r) != a) {
    last_fp_exceptions |= FE_INEXACT;
    last_fp_result_incremented = static_cast<double>(r) > a;
  }
  last_fp_exceptions |= pending_exceptions;
  last_fp_invalid_detail |= pending_invalid;
  last_fp_result_incremented = last_fp_result_incremented || pending_incremented;
  return r;
}
[[nodiscard]] inline double fp_from_i64(CpuState& state, std::uint64_t bits) noexcept {
  if (fp_native(state)) return static_cast<double>(static_cast<std::int64_t>(bits));
  state.fp_sync();
  begin_fp_operation(state);
  volatile double r = static_cast<double>(static_cast<std::int64_t>(fp_operand(bits)));
  finish_fp_operation();
  return r;
}

// lfs/stfs conversions are pure integer operations: a single-precision value
// moving to or from memory raises no floating-point status on the guest, and
// must not leave host flags that would reach FPSCR. Widening is exact and keeps
// a NaN's payload and quiet bit; narrowing rounds to nearest-even like frsp's
// result (denormal and overflowing values included) without touching MXCSR.
[[nodiscard]] inline std::uint64_t fp_single_to_double_bits(std::uint32_t w) noexcept {
  const std::uint64_t sign = static_cast<std::uint64_t>(w >> 31) << 63;
  std::uint32_t exp = (w >> 23) & 0xFFu;
  std::uint64_t frac = w & 0x7FFFFFu;
  if (exp == 0xFFu) return sign | 0x7FF0000000000000ull | (frac << 29);
  if (exp == 0u) {
    if (frac == 0) return sign;
    // A single denormal is a normal double: shift the leading one up.
    const int lead = 31 - std::countl_zero(static_cast<std::uint32_t>(frac));
    const int shift = 23 - lead;
    frac = (frac << shift) & 0x7FFFFFu;
    return sign | (static_cast<std::uint64_t>(1023 - 126 - shift) << 52) | (frac << 29);
  }
  return sign | (static_cast<std::uint64_t>(exp + 896u) << 52) | (frac << 29);
}
[[nodiscard]] inline std::uint32_t fp_double_to_single_bits(std::uint64_t b) noexcept {
  const std::uint32_t sign = static_cast<std::uint32_t>(b >> 63) << 31;
  const std::int32_t exp = static_cast<std::int32_t>((b >> 52) & 0x7FFu);
  const std::uint64_t frac = b & 0x000FFFFFFFFFFFFFull;
  if (exp == 0x7FF) {
    if (frac == 0) return sign | 0x7F800000u;
    std::uint32_t payload = static_cast<std::uint32_t>(frac >> 29);
    if (payload == 0) payload = 0x400000u;  // never turn a NaN into infinity
    return sign | 0x7F800000u | payload;
  }
  if (exp == 0) return sign;  // zero, and double denormals far below single range
  const std::int32_t e = exp - 1023;
  if (e > 127) return sign | 0x7F800000u;
  if (e >= -126) {
    std::uint32_t m = static_cast<std::uint32_t>(frac >> 29);
    const std::uint64_t low = frac & 0x1FFFFFFFull;
    if (low > 0x10000000ull || (low == 0x10000000ull && (m & 1u))) ++m;
    // A carry out of the mantissa bumps the exponent (and may reach infinity).
    return sign | (static_cast<std::uint32_t>(e + 127) << 23) + m;
  }
  if (e >= -150) {
    const std::uint64_t M = (1ull << 52) | frac;
    const unsigned shift = static_cast<unsigned>(29 + (-126 - e));
    std::uint32_t m = static_cast<std::uint32_t>(M >> shift);
    const std::uint64_t rem = M & ((1ull << shift) - 1u);
    const std::uint64_t half = 1ull << (shift - 1u);
    if (rem > half || (rem == half && (m & 1u))) ++m;
    return sign | m;
  }
  return sign;
}
[[nodiscard]] inline double fp_promote_f32(float f) noexcept {
  return std::bit_cast<double>(fp_single_to_double_bits(std::bit_cast<std::uint32_t>(f)));
}
[[nodiscard]] inline float fp_demote_f64(double d) noexcept {
  return std::bit_cast<float>(fp_double_to_single_bits(std::bit_cast<std::uint64_t>(d)));
}

inline std::uint8_t fprf_for(double v) noexcept {
  // PowerPC FPSCR[FPRF] (C, FL, FG, FE, FU) result classification.
  // Keep denormalized results distinct from normalized results; games may
  // observe this through mffs/mcrfs even when host FP arithmetic is native.
  if (std::isnan(v)) return 0x11u;          // 1 0 0 0 1 : quiet NaN
  if (std::isinf(v)) return std::signbit(v) ? 0x09u : 0x05u;
  if (v == 0.0) return std::signbit(v) ? 0x12u : 0x02u;
  if (std::fpclassify(v) == FP_SUBNORMAL)
    return std::signbit(v) ? 0x18u : 0x14u;
  return std::signbit(v) ? 0x08u : 0x04u;
}

inline void update_fpscr(CpuState& state, double result) noexcept {
  if (fp_native(state)) {
    // Tier A: remember the result; fp_sync() classifies it (FPRF) on observation.
    if (fp_legacy_status) return;
    state.fp_last_bits = std::bit_cast<std::uint64_t>(result);
    state.fp_pending = 1;
    return;
  }
  if (fp_legacy_status && !fp_strict_mode) return;
  state.fp_pending = 0;
  state.fpscr = (state.fpscr & ~fpscr_bits::FPRF_MASK) |
                (std::uint32_t(fprf_for(result)&0x1Fu) << 12);
  state.fpscr &= ~(fpscr_bits::FR | fpscr_bits::FI);
  const int ex = last_fp_exceptions;
  if (last_fp_invalid_detail) {
    state.fpscr |= last_fp_invalid_detail | fpscr_bits::FX;
  }
  if(ex & FE_DIVBYZERO) state.fpscr |= fpscr_bits::ZX | fpscr_bits::FX;
  if(ex & FE_OVERFLOW) state.fpscr |= fpscr_bits::OX | fpscr_bits::FX;
  if(ex & FE_UNDERFLOW) state.fpscr |= fpscr_bits::UX | fpscr_bits::FX;
  if(ex & FE_INEXACT) state.fpscr |= fpscr_bits::XX | fpscr_bits::FI | fpscr_bits::FX;
  if(last_fp_result_incremented) state.fpscr |= fpscr_bits::FR;
  recompute_fpscr_summaries(state);
  last_fp_exceptions = 0;
  last_fp_invalid_detail = 0;
  last_fp_result_incremented = false;
}


inline int host_rounding(FpRoundingMode m) noexcept {
  switch(m){
    case FpRoundingMode::Nearest:return FE_TONEAREST;
    case FpRoundingMode::TowardZero:return FE_TOWARDZERO;
    case FpRoundingMode::TowardPositive:return FE_UPWARD;
    case FpRoundingMode::TowardNegative:return FE_DOWNWARD;
  }
  return FE_TONEAREST;
}

inline bool trap_condition(std::uint32_t to, std::uint64_t a, std::uint64_t b,
                           bool word) noexcept {
  if(word){ a=static_cast<std::uint32_t>(a); b=static_cast<std::uint32_t>(b); }
  const auto sa=word?static_cast<std::int64_t>(static_cast<std::int32_t>(a)):static_cast<std::int64_t>(a);
  const auto sb=word?static_cast<std::int64_t>(static_cast<std::int32_t>(b)):static_cast<std::int64_t>(b);
  if((to&0x10u)&&sa<sb) return true;
  if((to&0x08u)&&sa>sb) return true;
  if((to&0x04u)&&a==b) return true;
  if((to&0x02u)&&a<b) return true;
  if((to&0x01u)&&a>b) return true;
  return false;
}



[[nodiscard]] inline std::uint8_t fp_compare_field(double a, double b) noexcept {
  if (std::isnan(a) || std::isnan(b)) return 0x1u;
  if (a < b) return 0x8u;
  if (a > b) return 0x4u;
  return 0x2u;
}

[[nodiscard]] inline std::uint64_t fp_to_integer_bits(CpuState& state,
                                                       double value,
                                                       unsigned width,
                                                       bool toward_zero) noexcept {
  // fctiw/fctid place a signed integer bit-pattern in the FPR while recording
  // invalid-conversion and inexact status for the following FUpdateStatus IR.
  last_fp_exceptions = 0;
  last_fp_invalid_detail = 0;
  last_fp_result_incremented = false;

  const bool snan = is_signaling_nan(value);
  if (snan) last_fp_invalid_detail |= fpscr_bits::VXSNAN;

  const long double lo = width == 32
      ? static_cast<long double>(std::numeric_limits<std::int32_t>::min())
      : static_cast<long double>(std::numeric_limits<std::int64_t>::min());
  const long double hi = width == 32
      ? static_cast<long double>(std::numeric_limits<std::int32_t>::max())
      : static_cast<long double>(std::numeric_limits<std::int64_t>::max());

  if (std::isnan(value)) {
    last_fp_invalid_detail |= fpscr_bits::VXCVI;
    return width == 32 ? 0xFFFFFFFF80000000ull : 0x8000000000000000ull;
  }
  if (static_cast<long double>(value) > hi) {
    last_fp_invalid_detail |= fpscr_bits::VXCVI;
    return width == 32 ? 0x000000007FFFFFFFull
                       : static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  }
  if (static_cast<long double>(value) < lo) {
    last_fp_invalid_detail |= fpscr_bits::VXCVI;
    return width == 32 ? 0xFFFFFFFF80000000ull : 0x8000000000000000ull;
  }

  const long double input = static_cast<long double>(value);
  long double rounded{};
  if (toward_zero) {
    // Truncation does not depend on the rounding mode.
    rounded = std::trunc(input);
  } else if (fp_native(state)) {
    // The host already rounds to nearest, as the guest asked.
    rounded = std::nearbyint(input);
  } else {
    const int old_round = std::fegetround();
    std::fesetround(host_rounding(state.fp_rounding_mode()));
    rounded = std::nearbyint(input);
    std::fesetround(old_round);
  }

  if (rounded != input) {
    last_fp_exceptions |= FE_INEXACT;
    // FR is set when rounding increments the infinitely precise result. In the
    // signed conversion case this corresponds to the rounded result being
    // numerically greater than the input.
    last_fp_result_incremented = rounded > input;
  }

  if (width == 32) {
    return static_cast<std::uint64_t>(
        static_cast<std::int64_t>(static_cast<std::int32_t>(rounded)));
  }
  return static_cast<std::uint64_t>(static_cast<std::int64_t>(rounded));
}

// Compare status is separate from arithmetic status: fcmpu only raises
// VXSNAN for an SNaN, while fcmpo additionally raises VXVC for unordered
// ordered comparisons (QNaN always; SNaN when invalid exceptions are masked).
inline void update_fp_compare_status(CpuState& state, double a, double b,
                                     bool ordered) noexcept {
  if (fp_legacy_status && !fp_strict_mode) return;
  state.fp_pending = 0;  // the compare code replaces FPRF outright
  const auto fpcc = fp_compare_field(a, b);
  state.fpscr = (state.fpscr & ~fpscr_bits::FPRF_MASK) |
                (std::uint32_t(fpcc & 0xFu) << 12);

  const bool any_nan = std::isnan(a) || std::isnan(b);
  const bool any_snan = is_signaling_nan(a) || is_signaling_nan(b);
  if (any_snan) {
    state.fpscr |= fpscr_bits::VXSNAN | fpscr_bits::FX;
  }
  if (ordered && any_nan) {
    const bool invalid_enabled = (state.fpscr & fpscr_bits::VE) != 0;
    const bool quiet_nan_present = any_nan && !any_snan;
    if (quiet_nan_present || !invalid_enabled) {
      state.fpscr |= fpscr_bits::VXVC | fpscr_bits::FX;
    }
  }
  recompute_fpscr_summaries(state);
}

// Status update for floating-to-integer conversions. The FPRF field is
// architecturally undefined for fctid/fctidz, so preserve it rather than
// manufacturing a classification from the source operand. fctiw likewise has
// undefined class bits; the sticky/rounding status is the observable contract.
inline void update_fp_conversion_status(CpuState& state) noexcept {
  if (fp_legacy_status && !fp_strict_mode) return;
  state.fp_commit_fprf();
  state.fpscr &= ~(fpscr_bits::FR | fpscr_bits::FI);
  if (last_fp_invalid_detail) {
    state.fpscr |= last_fp_invalid_detail | fpscr_bits::FX;
  }
  if (last_fp_exceptions & FE_INEXACT) {
    state.fpscr |= fpscr_bits::XX | fpscr_bits::FI | fpscr_bits::FX;
  }
  if (last_fp_result_incremented) state.fpscr |= fpscr_bits::FR;
  recompute_fpscr_summaries(state);
  last_fp_exceptions = 0;
  last_fp_invalid_detail = 0;
  last_fp_result_incremented = false;
}

inline void set_fpscr_bit(CpuState& state, unsigned architectural_bit, bool value) noexcept {
  state.fp_sync();
  architectural_bit &= 31u;
  const std::uint32_t mask = 0x80000000u >> architectural_bit;
  // FEX and VX are derived summaries, not directly writable via mtfsb.
  if (mask == fpscr_bits::FEX || mask == fpscr_bits::VX) return;
  if (value) state.fpscr |= mask; else state.fpscr &= ~mask;
  recompute_fpscr_summaries(state);
}

inline void write_fpscr_field(CpuState& state, unsigned field, std::uint8_t value) noexcept {
  state.fp_sync();
  field &= 7u;
  const unsigned shift = (7u - field) * 4u;
  const std::uint32_t mask = 0xFu << shift;
  std::uint32_t writable = mask;
  // FEX and VX are summary bits (architectural bits 1 and 2) and are not
  // directly writable by mtfsb/mtfsfi/mtfsf.
  writable &= ~(fpscr_bits::FEX | fpscr_bits::VX);
  const std::uint32_t incoming = (std::uint32_t(value) & 0xFu) << shift;
  state.fpscr = (state.fpscr & ~writable) | (incoming & writable);
  recompute_fpscr_summaries(state);
}

inline void write_fpscr_fields(CpuState& state, std::uint8_t field_mask,
                               std::uint32_t source) noexcept {
  state.fp_sync();
  for (unsigned field = 0; field < 8; ++field) {
    const bool selected = (field_mask & (0x80u >> field)) != 0;
    if (!selected) continue;
    const unsigned shift = (7u - field) * 4u;
    write_fpscr_field(state, field, std::uint8_t((source >> shift) & 0xFu));
  }
}

inline void move_fpscr_field_to_cr(CpuState& state, unsigned cr_field,
                                   unsigned fpscr_field) noexcept {
  state.fp_sync();
  fpscr_field &= 7u;
  const unsigned shift = (7u - fpscr_field) * 4u;
  state.set_cr_field(cr_field, std::uint8_t((state.fpscr >> shift) & 0xFu));
  // mcrfs clears the exception status bits contained in fields 0..3. FEX and VX
  // are summaries and are recomputed by software-visible status producers.
  if (fpscr_field <= 3) {
    const std::uint32_t clear_masks[4] = {
      fpscr_bits::FX | fpscr_bits::OX,
      fpscr_bits::UX | fpscr_bits::ZX | fpscr_bits::XX | fpscr_bits::VXSNAN,
      fpscr_bits::VXISI | fpscr_bits::VXIDI | fpscr_bits::VXZDZ | fpscr_bits::VXIMZ,
      fpscr_bits::VXVC,
    };
    state.fpscr &= ~clear_masks[fpscr_field];
    recompute_fpscr_summaries(state);
  }
}

[[nodiscard]] inline std::uint64_t mul_hi_signed_width(std::uint64_t a, std::uint64_t b,
                                                        unsigned width) noexcept {
  if (width == 32) {
    const std::int64_t p = std::int64_t(static_cast<std::int32_t>(a)) *
                           std::int64_t(static_cast<std::int32_t>(b));
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(p) >> 32);
  }
  return mul_hi_signed(static_cast<std::int64_t>(a), static_cast<std::int64_t>(b));
}
[[nodiscard]] inline std::uint64_t mul_hi_unsigned_width(std::uint64_t a, std::uint64_t b,
                                                          unsigned width) noexcept {
  if (width == 32) {
    return static_cast<std::uint32_t>((std::uint64_t(static_cast<std::uint32_t>(a)) *
                                       std::uint64_t(static_cast<std::uint32_t>(b))) >> 32);
  }
  return mul_hi_unsigned(a,b);
}
[[nodiscard]] inline bool mul_overflow_signed(std::uint64_t a, std::uint64_t b,
                                               unsigned width) noexcept {
  if (width == 32) {
    const std::int64_t p = std::int64_t(static_cast<std::int32_t>(a)) *
                           std::int64_t(static_cast<std::int32_t>(b));
    return p > std::numeric_limits<std::int32_t>::max() || p < std::numeric_limits<std::int32_t>::min();
  }
  const auto x=static_cast<std::int64_t>(a), y=static_cast<std::int64_t>(b);
  if (x == 0 || y == 0) return false;
  if (x == -1) return y == std::numeric_limits<std::int64_t>::min();
  if (y == -1) return x == std::numeric_limits<std::int64_t>::min();
  if (x > 0) {
    if (y > 0) return x > std::numeric_limits<std::int64_t>::max()/y;
    return y < std::numeric_limits<std::int64_t>::min()/x;
  }
  if (y > 0) return x < std::numeric_limits<std::int64_t>::min()/y;
  return x != 0 && y < std::numeric_limits<std::int64_t>::max()/x;
}


// PowerPC v2.02 MSR write semantics used by Xbox-era Xenon code. PowerPC
// architectural bit numbers are MSB-first, so bit N maps to integer bit 63-N.
[[nodiscard]] constexpr std::uint64_t msr_arch_bit(unsigned n) noexcept {
  return std::uint64_t{1} << (63u - n);
}
[[nodiscard]] constexpr std::uint64_t msr_arch_range(unsigned first, unsigned last) noexcept {
  std::uint64_t mask = 0;
  for (unsigned n = first; n <= last; ++n) mask |= msr_arch_bit(n);
  return mask;
}

inline void write_msr(CpuState& state, std::uint64_t source, bool limited,
                      bool doubleword) noexcept {
  constexpr auto EE = msr_arch_bit(48);
  constexpr auto PR = msr_arch_bit(49);
  constexpr auto IR = msr_arch_bit(58);
  constexpr auto DR = msr_arch_bit(59);
  constexpr auto RI = msr_arch_bit(62);

  // L=1 is identical for mtmsr and mtmsrd: only EE and RI are replaced.
  if (limited) {
    constexpr std::uint64_t mask = EE | RI;
    state.msr = (state.msr & ~mask) | (source & mask);
    return;
  }

  if (doubleword) {
    // PowerPC OEA v2.02 mtmsrd L=0. HV (architectural bit 3) and ME (51)
    // are intentionally preserved. SF (0) is RS[0] | RS[1].
    constexpr std::uint64_t copy_mask =
        msr_arch_range(1, 2) | msr_arch_range(4, 47) |
        msr_arch_range(49, 50) | msr_arch_range(52, 57) |
        msr_arch_range(60, 63);
    state.msr = (state.msr & ~copy_mask) | (source & copy_mask);
    const bool sf = (source & (msr_arch_bit(0) | msr_arch_bit(1))) != 0;
    if (sf) state.msr |= msr_arch_bit(0); else state.msr &= ~msr_arch_bit(0);
  } else {
    // 32-bit mtmsr L=0 only changes the low architectural half and preserves
    // ME. Xenon is POWER4+-era, so PR participates in EE/IR/DR derivation.
    constexpr std::uint64_t copy_mask =
        msr_arch_range(32, 47) | msr_arch_range(49, 50) |
        msr_arch_range(52, 57) | msr_arch_range(60, 63);
    state.msr = (state.msr & ~copy_mask) | (source & copy_mask);
  }

  // POWER4+ OEA rule: setting PR also forces EE, IR and DR. Otherwise these
  // three bits take RS-bit OR PR as specified by mtmsr[d].
  const bool pr = (source & PR) != 0;
  const auto set_from_source_or_pr = [&](std::uint64_t bit) {
    if ((source & bit) != 0 || pr) state.msr |= bit;
    else state.msr &= ~bit;
  };
  set_from_source_or_pr(EE);
  set_from_source_or_pr(IR);
  set_from_source_or_pr(DR);
}

// Vector semantic entry point used by statically-generated code. The argument
// is an internal semantic enum chosen by the compiler, not a guest opcode. It
// therefore cannot decode or dispatch arbitrary PPC at runtime.
Vector128 execute_vector(VectorSemantic semantic, std::uint32_t guest_word,
                         CpuState& state, const Vector128& a,
                         const Vector128& b = {}, const Vector128& c = {},
                         const Vector128& d = {});

// VMX memory/permute helpers shared by the AOT backend and tests.
Vector128 vector_load_shift_left(std::uint64_t ea) noexcept;
Vector128 vector_load_shift_right(std::uint64_t ea) noexcept;
Vector128 vector_load_element(const Vector128& old, MemoryAccessContext& memory,
                              GuestAddress ea, unsigned width);
void vector_store_element(const Vector128& value, MemoryAccessContext& memory,
                          GuestAddress ea, unsigned width);

// lvlx/lvrx/stvlx/stvrx (and the 128 forms): the part of the aligned 16-byte
// block holding ea from ea up (left) or below ea (right), against the first
// or last bytes of the register. Inline over MemoryAccessContext's
// quadword-part accessors: AC6's audio mixer stores its unaligned output
// with stvlx/stvrx pairs, and the general byte-range path cost it 8% of its
// thread.
inline Vector128 vector_load_left(const Vector128& old, MemoryAccessContext& memory,
                                  GuestAddress ea) {
  (void)old;
  return memory.read_vector_left(ea);
}
inline Vector128 vector_load_right(const Vector128& old, MemoryAccessContext& memory,
                                   GuestAddress ea) {
  (void)old;
  return memory.read_vector_right(ea);
}
inline void vector_store_left(const Vector128& value, MemoryAccessContext& memory,
                              GuestAddress ea) {
  const auto n = static_cast<unsigned>(ea & 15u);
  memory.write_quadword_part(ea, std::as_bytes(std::span(value.bytes)).first(16u - n));
}
inline void vector_store_right(const Vector128& value, MemoryAccessContext& memory,
                               GuestAddress ea) {
  const auto n = static_cast<unsigned>(ea & 15u);
  if (n)
    memory.write_quadword_part(GuestAddress(ea & ~15u),
                               std::as_bytes(std::span(value.bytes)).subspan(16u - n, n));
}

void string_load(CpuState& state, MemoryAccessContext& memory, GuestAddress ea,
                 std::uint32_t count, unsigned first_reg);
void string_store(CpuState& state, MemoryAccessContext& memory, GuestAddress ea,
                  std::uint32_t count, unsigned first_reg);

}  // namespace xenon::cpu::aot

#include "xenon/cpu/aot_vector_inline.hpp"


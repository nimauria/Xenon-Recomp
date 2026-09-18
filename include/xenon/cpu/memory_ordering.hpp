#pragma once

#include <atomic>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace xenon::cpu {

enum class BarrierKind : std::uint8_t {
  Sync,
  LightweightSync,
  Eieio,
  InstructionSync,
};

enum class OrderedAccess : std::uint8_t {
  Load,
  Store,
};

enum class MemoryOrderingDomain : std::uint8_t {
  Normal,
  Device,
  WriteCombined,
  CacheInhibited,
};

struct BarrierSemantics {
  bool load_before_load{};
  bool load_before_store{};
  bool store_before_load{};
  bool store_before_store{};
  bool device_only{};
  bool instruction_sync{};
};

// Canonical PPC/Xenon ordering contract. This describes guest-visible ordering,
// not the host instruction chosen to implement it.
[[nodiscard]] constexpr BarrierSemantics barrier_semantics(
    BarrierKind kind) noexcept {
  switch (kind) {
    case BarrierKind::Sync:
      return {true, true, true, true, false, false};
    case BarrierKind::LightweightSync:
      // POWER lwsync orders Load->Load, Load->Store and Store->Store, but not
      // Store->Load. Keeping that distinction matters on weakly ordered ARM64.
      return {true, true, false, true, false, false};
    case BarrierKind::Eieio:
      // eieio is an I/O/cache-inhibited ordering primitive. It is not a general
      // cached-RAM barrier. Xenon Memory currently conservatively orders all
      // device-side load/store pairs when this barrier is requested.
      return {true, true, true, true, true, false};
    case BarrierKind::InstructionSync:
      return {false, false, false, false, false, true};
  }
  return {};
}

[[nodiscard]] constexpr bool barrier_orders(
    BarrierKind kind, OrderedAccess before, OrderedAccess after,
    MemoryOrderingDomain domain = MemoryOrderingDomain::Normal) noexcept {
  const auto semantics = barrier_semantics(kind);
  if (semantics.device_only && domain == MemoryOrderingDomain::Normal) {
    return false;
  }
  if (before == OrderedAccess::Load && after == OrderedAccess::Load) {
    return semantics.load_before_load;
  }
  if (before == OrderedAccess::Load && after == OrderedAccess::Store) {
    return semantics.load_before_store;
  }
  if (before == OrderedAccess::Store && after == OrderedAccess::Load) {
    return semantics.store_before_load;
  }
  return semantics.store_before_store;
}

namespace host_memory_ordering {

inline void compiler_barrier() noexcept {
  std::atomic_signal_fence(std::memory_order_seq_cst);
}

// Translate the canonical Xenon/PPC barrier to the cheapest host primitive we
// can use without weakening guest semantics. Common RAM accesses are relaxed
// atomic_ref operations, so these barriers provide the cross-access ordering.
inline void apply(BarrierKind kind) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
  switch (kind) {
    case BarrierKind::Sync:
      // x86 TSO still permits Store->Load reordering; heavyweight sync does not.
      _mm_mfence();
      compiler_barrier();
      return;
    case BarrierKind::LightweightSync:
      // x86 TSO already provides exactly the data-order pairs required by
      // lwsync (LL, LS, SS) while still allowing Store->Load. Only prevent the
      // compiler from moving guest accesses across the boundary.
      compiler_barrier();
      return;
    case BarrierKind::Eieio:
      // Xenon's eieio is used for I/O/cache-inhibited ordering. MFENCE is
      // conservative for host MMIO / WC mappings and avoids relying on normal
      // WB-memory TSO guarantees for future direct device mappings.
      _mm_mfence();
      compiler_barrier();
      return;
    case BarrierKind::InstructionSync:
      // Guest instructions are recompiled code, not fetched directly from guest
      // RAM. The guest execution boundary is retained here; executable-page/code
      // generation synchronization is owned by the executable-memory subsystem.
      compiler_barrier();
      return;
  }
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
  switch (kind) {
    case BarrierKind::Sync:
      // PPC sync is a heavyweight completion/order point. DSB is intentionally
      // used instead of DMB so prior memory accesses have completed before the
      // following guest operations proceed.
      __asm__ __volatile__("dsb ish" ::: "memory");
      return;
    case BarrierKind::LightweightSync:
      // Preserve the important PPC distinction that Store->Load is *not*
      // ordered by lwsync. ISHST supplies SS; ISHLD supplies LL and LS.
      __asm__ __volatile__("dmb ishst\n\tdmb ishld" ::: "memory");
      return;
    case BarrierKind::Eieio:
      // Device/cache-inhibited accesses may live outside the inner-shareable
      // domain, so use an outer-shareable device ordering point.
      __asm__ __volatile__("dmb osh" ::: "memory");
      return;
    case BarrierKind::InstructionSync:
      __asm__ __volatile__("isb" ::: "memory");
      return;
  }
#else
  // Portable fallback for host architectures without a tuned mapping yet.
  // It may be stronger than the guest primitive, but never weaker.
  switch (kind) {
    case BarrierKind::Sync:
      std::atomic_thread_fence(std::memory_order_seq_cst);
      return;
    case BarrierKind::LightweightSync:
      std::atomic_thread_fence(std::memory_order_acq_rel);
      return;
    case BarrierKind::Eieio:
      std::atomic_thread_fence(std::memory_order_seq_cst);
      return;
    case BarrierKind::InstructionSync:
      std::atomic_thread_fence(std::memory_order_acquire);
      compiler_barrier();
      return;
  }
#endif
}

}  // namespace host_memory_ordering
}  // namespace xenon::cpu

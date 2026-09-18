#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

#include "xenon/cpu/types.hpp"

namespace xenon::cpu {

enum class BarrierKind : std::uint8_t {
  Sync,
  LightweightSync,
  Eieio,
  InstructionSync,
};

class MemoryPort;

// Compact page-entry contract published by the production Xenon memory system.
// The CPU only consumes this representation; AddressSpace remains the authority
// that creates mappings, permissions, aliases and MMIO overlays.
namespace fast_memory {
inline constexpr std::uint64_t kPhysicalPageMask = (1ull << 17u) - 1u;
inline constexpr std::uint64_t kMapped = 1ull << 17u;
inline constexpr std::uint64_t kRead = 1ull << 18u;
inline constexpr std::uint64_t kWrite = 1ull << 19u;
inline constexpr std::uint64_t kExecute = 1ull << 20u;
inline constexpr std::uint64_t kSlow = 1ull << 21u;
inline constexpr std::uint64_t kNoCache = 1ull << 22u;
inline constexpr std::uint64_t kWriteCombine = 1ull << 23u;

[[nodiscard]] constexpr std::uint64_t encode_page(
    std::uint32_t physical_page, bool read, bool write, bool execute,
    bool slow = false, bool no_cache = false,
    bool write_combine = false) noexcept {
  return (std::uint64_t{physical_page} & kPhysicalPageMask) | kMapped |
         (read ? kRead : 0) | (write ? kWrite : 0) |
         (execute ? kExecute : 0) | (slow ? kSlow : 0) |
         (no_cache ? kNoCache : 0) |
         (write_combine ? kWriteCombine : 0);
}
}  // namespace fast_memory

// Data needed by the generated-memory fast path. All pointers refer to storage
// whose lifetime is owned by the concrete MemoryPort implementation.
struct FastMemoryView {
  std::byte* physical_base{};
  const std::atomic<std::uint64_t>* page_table{};
  std::uint32_t page_count{};
  std::uint32_t page_shift{12};
  std::uint32_t physical_size{};

  std::atomic<std::uint32_t>* reservation_versions{};
  std::uint32_t reservation_granule_size{};
  std::uint32_t reservation_granule_count{};

  std::atomic<std::uint64_t>* physical_page_epochs{};
  std::uint32_t physical_page_count{};
  std::atomic<std::uint64_t>* global_write_epoch{};
  std::atomic<std::uint32_t>* active_coherency_writers{};

  // Read-side quiescence for mapping reclamation. A context holds one reader
  // reference for its lifetime. Cold mapping code may unpublish a hot entry
  // immediately, but physical pages retired by that change are not recycled
  // until no context that could have observed the old entry remains alive.
  std::atomic<std::uint32_t>* active_fast_readers{};
};

// Concrete, compiler-inlinable access facade used by generated PPC. Common RAM
// accesses use the compact atomic page table directly. MMIO, faults, unusual
// cross-page accesses and test/reference ports fall back to MemoryPort.
class MemoryAccessContext {
 public:
  explicit MemoryAccessContext(MemoryPort& slow,
                               FastMemoryView fast = {}) noexcept
      : slow_(&slow), fast_(fast) {
    acquire_read_guard();
  }
  ~MemoryAccessContext() noexcept { release_read_guard(); }
  MemoryAccessContext(const MemoryAccessContext&) = delete;
  MemoryAccessContext& operator=(const MemoryAccessContext&) = delete;
  MemoryAccessContext(MemoryAccessContext&& other) noexcept
      : slow_(std::exchange(other.slow_, nullptr)),
        fast_(other.fast_),
        read_guard_active_(std::exchange(other.read_guard_active_, false)) {}
  MemoryAccessContext& operator=(MemoryAccessContext&& other) noexcept {
    if (this == &other) return *this;
    release_read_guard();
    slow_ = std::exchange(other.slow_, nullptr);
    fast_ = other.fast_;
    read_guard_active_ = std::exchange(other.read_guard_active_, false);
    return *this;
  }

  [[nodiscard]] std::uint8_t read8(GuestAddress address);
  [[nodiscard]] std::uint16_t read16_be(GuestAddress address);
  [[nodiscard]] std::uint32_t read32_be(GuestAddress address);
  [[nodiscard]] std::uint64_t read64_be(GuestAddress address);
  [[nodiscard]] Vector128 read128(GuestAddress address);

  void write8(GuestAddress address, std::uint8_t value);
  void write16_be(GuestAddress address, std::uint16_t value);
  void write32_be(GuestAddress address, std::uint32_t value);
  void write64_be(GuestAddress address, std::uint64_t value);
  void write128(GuestAddress address, const Vector128& value);

  [[nodiscard]] std::uint16_t read16_le(GuestAddress address);
  [[nodiscard]] std::uint32_t read32_le(GuestAddress address);
  [[nodiscard]] std::uint64_t read64_le(GuestAddress address);
  void write16_le(GuestAddress address, std::uint16_t value);
  void write32_le(GuestAddress address, std::uint32_t value);
  void write64_le(GuestAddress address, std::uint64_t value);

  [[nodiscard]] bool has_fast_path() const noexcept {
    return fast_.physical_base && fast_.page_table && fast_.page_count;
  }

 private:
  struct Resolved {
    std::byte* ptr{};
    std::uint32_t physical_address{};
  };

  [[nodiscard]] bool resolve_fast(GuestAddress address, std::size_t width,
                                  bool write, std::size_t alignment,
                                  Resolved& out) const noexcept;
  void note_write(std::uint32_t physical_address,
                  std::uint32_t width) const noexcept;

  template <typename T>
  [[nodiscard]] T read_integer(GuestAddress address, bool little_endian);
  template <typename T>
  void write_integer(GuestAddress address, T value, bool little_endian);

  template <typename T>
  [[nodiscard]] static T byteswap_if(T value, bool swap) noexcept;

  void acquire_read_guard() noexcept {
    if (!fast_.active_fast_readers || !has_fast_path()) return;
    fast_.active_fast_readers->fetch_add(1u, std::memory_order_acq_rel);
    read_guard_active_ = true;
  }
  void release_read_guard() noexcept {
    if (!read_guard_active_ || !fast_.active_fast_readers) return;
    fast_.active_fast_readers->fetch_sub(1u, std::memory_order_release);
    read_guard_active_ = false;
  }

  MemoryPort* slow_{};
  FastMemoryView fast_{};
  bool read_guard_active_{};
};

// Deliberately tiny CPU-facing contract. This is NOT the RAM subsystem.
class MemoryPort {
 public:
  virtual ~MemoryPort() = default;

  // One acquisition per generated function replaces virtual dispatch on each
  // ordinary load/store. Test fixtures use the default slow-only context;
  // production AddressSpace overrides this with the compact Memory V2 view.
  [[nodiscard]] virtual MemoryAccessContext access_context() noexcept {
    return MemoryAccessContext(*this);
  }

  virtual std::uint8_t read8(GuestAddress address) = 0;
  virtual std::uint16_t read16_be(GuestAddress address) = 0;
  virtual std::uint32_t read32_be(GuestAddress address) = 0;
  virtual std::uint64_t read64_be(GuestAddress address) = 0;
  virtual Vector128 read128(GuestAddress address) = 0;

  virtual void write8(GuestAddress address, std::uint8_t value) = 0;
  virtual void write16_be(GuestAddress address, std::uint16_t value) = 0;
  virtual void write32_be(GuestAddress address, std::uint32_t value) = 0;
  virtual void write64_be(GuestAddress address, std::uint64_t value) = 0;
  virtual void write128(GuestAddress address, const Vector128& value) = 0;

  // Byte-reversed PPC instructions are explicitly little-endian relative to
  // normal big-endian guest accesses.
  virtual std::uint16_t read16_le(GuestAddress address) = 0;
  virtual std::uint32_t read32_le(GuestAddress address) = 0;
  virtual std::uint64_t read64_le(GuestAddress address) = 0;
  virtual void write16_le(GuestAddress address, std::uint16_t value) = 0;
  virtual void write32_le(GuestAddress address, std::uint32_t value) = 0;
  virtual void write64_le(GuestAddress address, std::uint64_t value) = 0;

  // Reservation operations return a memory-generation token. The production
  // memory system owns invalidation across aliases and hardware threads.
  virtual std::uint64_t reserve32(GuestAddress address, std::uint32_t& value) = 0;
  virtual std::uint64_t reserve64(GuestAddress address, std::uint64_t& value) = 0;
  virtual bool store_conditional32(GuestAddress address, std::uint64_t token,
                                   std::uint32_t value) = 0;
  virtual bool store_conditional64(GuestAddress address, std::uint64_t token,
                                   std::uint64_t value) = 0;

  virtual void barrier(BarrierKind kind) = 0;
  virtual void zero_cache_block(GuestAddress address, std::uint32_t bytes) = 0;
  virtual void instruction_cache_invalidate(GuestAddress address) = 0;
};

namespace detail {

template <typename T>
[[nodiscard]] inline T atomic_load_relaxed(std::byte* ptr) noexcept {
  auto& object = *reinterpret_cast<T*>(ptr);
  std::atomic_ref<T> atomic(object);
  return atomic.load(std::memory_order_relaxed);
}

template <typename T>
inline void atomic_store_relaxed(std::byte* ptr, T value) noexcept {
  auto& object = *reinterpret_cast<T*>(ptr);
  std::atomic_ref<T> atomic(object);
  atomic.store(value, std::memory_order_relaxed);
}

}  // namespace detail

inline bool MemoryAccessContext::resolve_fast(GuestAddress address,
                                              std::size_t width, bool write,
                                              std::size_t alignment,
                                              Resolved& out) const noexcept {
  if (!has_fast_path() || !width) return false;
  const auto last64 = std::uint64_t{address} + width - 1u;
  if (last64 > std::numeric_limits<GuestAddress>::max()) return false;
  const auto last = static_cast<GuestAddress>(last64);
  const auto first_page = address >> fast_.page_shift;
  if (first_page >= fast_.page_count ||
      first_page != (last >> fast_.page_shift)) {
    return false;
  }

  const auto entry = fast_.page_table[first_page].load(std::memory_order_acquire);
  const auto permission = write ? fast_memory::kWrite : fast_memory::kRead;
  if ((entry & (fast_memory::kMapped | permission)) !=
          (fast_memory::kMapped | permission) ||
      (entry & fast_memory::kSlow)) {
    return false;
  }

  const auto page_size = std::uint32_t{1} << fast_.page_shift;
  const auto physical_page = static_cast<std::uint32_t>(
      entry & fast_memory::kPhysicalPageMask);
  const auto physical64 = std::uint64_t{physical_page} * page_size +
                          (address & (page_size - 1u));
  if (physical64 + width > fast_.physical_size) return false;
  const auto physical = static_cast<std::uint32_t>(physical64);
  auto* ptr = fast_.physical_base + physical;
  if (alignment > 1u &&
      (reinterpret_cast<std::uintptr_t>(ptr) & (alignment - 1u)) != 0) {
    return false;
  }
  out = {ptr, physical};
  return true;
}

inline void MemoryAccessContext::note_write(
    std::uint32_t physical_address, std::uint32_t width) const noexcept {
  if (!width || physical_address >= fast_.physical_size) return;

  if (fast_.reservation_versions && fast_.reservation_granule_size &&
      fast_.reservation_granule_count) {
    const auto first = physical_address / fast_.reservation_granule_size;
    const auto last = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        (std::uint64_t{physical_address} + width - 1u) /
            fast_.reservation_granule_size,
        fast_.reservation_granule_count - 1u));
    for (std::uint32_t i = first; i <= last; ++i) {
      const auto next = fast_.reservation_versions[i].fetch_add(
                            1u, std::memory_order_acq_rel) +
                        1u;
      if (next == 0u) {
        fast_.reservation_versions[i].store(1u, std::memory_order_release);
      }
    }
  }

  if (fast_.global_write_epoch && fast_.physical_page_epochs &&
      fast_.physical_page_count && fast_.active_coherency_writers) {
    fast_.active_coherency_writers->fetch_add(1u, std::memory_order_acq_rel);
    auto epoch = fast_.global_write_epoch->fetch_add(
                     1u, std::memory_order_acq_rel) +
                 1u;
    if (epoch == 0u) {
      fast_.global_write_epoch->store(1u, std::memory_order_release);
      epoch = 1u;
    }
    constexpr std::uint32_t kPhysicalPageShift = 12u;
    const auto first = physical_address >> kPhysicalPageShift;
    const auto last = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        (std::uint64_t{physical_address} + width - 1u) >>
            kPhysicalPageShift,
        fast_.physical_page_count - 1u));
    for (std::uint32_t i = first; i <= last; ++i) {
      fast_.physical_page_epochs[i].store(epoch, std::memory_order_release);
    }
    fast_.active_coherency_writers->fetch_sub(1u, std::memory_order_release);
  }

}

template <typename T>
inline T MemoryAccessContext::byteswap_if(T value, bool swap) noexcept {
  if (!swap || sizeof(T) == 1u) return value;
  if constexpr (sizeof(T) == 2u) {
    const auto v = static_cast<std::uint16_t>(value);
    return static_cast<T>((v << 8u) | (v >> 8u));
  } else if constexpr (sizeof(T) == 4u) {
    const auto v = static_cast<std::uint32_t>(value);
    return static_cast<T>(((v & 0x000000FFu) << 24u) |
                          ((v & 0x0000FF00u) << 8u) |
                          ((v & 0x00FF0000u) >> 8u) |
                          ((v & 0xFF000000u) >> 24u));
  } else if constexpr (sizeof(T) == 8u) {
    const auto v = static_cast<std::uint64_t>(value);
    return static_cast<T>(((v & 0x00000000000000FFull) << 56u) |
                          ((v & 0x000000000000FF00ull) << 40u) |
                          ((v & 0x0000000000FF0000ull) << 24u) |
                          ((v & 0x00000000FF000000ull) << 8u) |
                          ((v & 0x000000FF00000000ull) >> 8u) |
                          ((v & 0x0000FF0000000000ull) >> 24u) |
                          ((v & 0x00FF000000000000ull) >> 40u) |
                          ((v & 0xFF00000000000000ull) >> 56u));
  }
  return value;
}

template <typename T>
inline T MemoryAccessContext::read_integer(GuestAddress address,
                                           bool little_endian) {
  Resolved resolved{};
  if (!resolve_fast(address, sizeof(T), false, alignof(T), resolved)) {
    if constexpr (sizeof(T) == 2u) {
      return little_endian ? static_cast<T>(slow_->read16_le(address))
                           : static_cast<T>(slow_->read16_be(address));
    } else if constexpr (sizeof(T) == 4u) {
      return little_endian ? static_cast<T>(slow_->read32_le(address))
                           : static_cast<T>(slow_->read32_be(address));
    } else {
      return little_endian ? static_cast<T>(slow_->read64_le(address))
                           : static_cast<T>(slow_->read64_be(address));
    }
  }
  const auto raw = detail::atomic_load_relaxed<T>(resolved.ptr);
  const bool host_little = std::endian::native == std::endian::little;
  const bool swap = little_endian ? !host_little : host_little;
  return byteswap_if(raw, swap);
}

template <typename T>
inline void MemoryAccessContext::write_integer(GuestAddress address, T value,
                                               bool little_endian) {
  Resolved resolved{};
  if (!resolve_fast(address, sizeof(T), true, alignof(T), resolved)) {
    if constexpr (sizeof(T) == 2u) {
      if (little_endian) slow_->write16_le(address, value);
      else slow_->write16_be(address, value);
    } else if constexpr (sizeof(T) == 4u) {
      if (little_endian) slow_->write32_le(address, value);
      else slow_->write32_be(address, value);
    } else {
      if (little_endian) slow_->write64_le(address, value);
      else slow_->write64_be(address, value);
    }
    return;
  }
  const bool host_little = std::endian::native == std::endian::little;
  const bool swap = little_endian ? !host_little : host_little;
  detail::atomic_store_relaxed<T>(resolved.ptr, byteswap_if(value, swap));
  note_write(resolved.physical_address, sizeof(T));
}

inline std::uint8_t MemoryAccessContext::read8(GuestAddress address) {
  Resolved resolved{};
  if (!resolve_fast(address, 1, false, 1, resolved)) return slow_->read8(address);
  return detail::atomic_load_relaxed<std::uint8_t>(resolved.ptr);
}
inline std::uint16_t MemoryAccessContext::read16_be(GuestAddress address) {
  return read_integer<std::uint16_t>(address, false);
}
inline std::uint32_t MemoryAccessContext::read32_be(GuestAddress address) {
  return read_integer<std::uint32_t>(address, false);
}
inline std::uint64_t MemoryAccessContext::read64_be(GuestAddress address) {
  return read_integer<std::uint64_t>(address, false);
}
inline Vector128 MemoryAccessContext::read128(GuestAddress address) {
  Resolved resolved{};
  if (!resolve_fast(address, 16, false, alignof(std::uint64_t), resolved)) {
    return slow_->read128(address);
  }
  Vector128 value{};
  const auto lo = detail::atomic_load_relaxed<std::uint64_t>(resolved.ptr);
  const auto hi = detail::atomic_load_relaxed<std::uint64_t>(resolved.ptr + 8u);
  std::memcpy(value.bytes.data(), &lo, sizeof(lo));
  std::memcpy(value.bytes.data() + 8u, &hi, sizeof(hi));
  return value;
}

inline void MemoryAccessContext::write8(GuestAddress address,
                                        std::uint8_t value) {
  Resolved resolved{};
  if (!resolve_fast(address, 1, true, 1, resolved)) {
    slow_->write8(address, value);
    return;
  }
  detail::atomic_store_relaxed<std::uint8_t>(resolved.ptr, value);
  note_write(resolved.physical_address, 1);
}
inline void MemoryAccessContext::write16_be(GuestAddress address,
                                            std::uint16_t value) {
  write_integer(address, value, false);
}
inline void MemoryAccessContext::write32_be(GuestAddress address,
                                            std::uint32_t value) {
  write_integer(address, value, false);
}
inline void MemoryAccessContext::write64_be(GuestAddress address,
                                            std::uint64_t value) {
  write_integer(address, value, false);
}
inline void MemoryAccessContext::write128(GuestAddress address,
                                          const Vector128& value) {
  Resolved resolved{};
  if (!resolve_fast(address, 16, true, alignof(std::uint64_t), resolved)) {
    slow_->write128(address, value);
    return;
  }
  std::uint64_t lo{}, hi{};
  std::memcpy(&lo, value.bytes.data(), sizeof(lo));
  std::memcpy(&hi, value.bytes.data() + 8u, sizeof(hi));
  detail::atomic_store_relaxed<std::uint64_t>(resolved.ptr, lo);
  detail::atomic_store_relaxed<std::uint64_t>(resolved.ptr + 8u, hi);
  note_write(resolved.physical_address, 16);
}

inline std::uint16_t MemoryAccessContext::read16_le(GuestAddress address) {
  return read_integer<std::uint16_t>(address, true);
}
inline std::uint32_t MemoryAccessContext::read32_le(GuestAddress address) {
  return read_integer<std::uint32_t>(address, true);
}
inline std::uint64_t MemoryAccessContext::read64_le(GuestAddress address) {
  return read_integer<std::uint64_t>(address, true);
}
inline void MemoryAccessContext::write16_le(GuestAddress address,
                                            std::uint16_t value) {
  write_integer(address, value, true);
}
inline void MemoryAccessContext::write32_le(GuestAddress address,
                                            std::uint32_t value) {
  write_integer(address, value, true);
}
inline void MemoryAccessContext::write64_le(GuestAddress address,
                                            std::uint64_t value) {
  write_integer(address, value, true);
}

}  // namespace xenon::cpu

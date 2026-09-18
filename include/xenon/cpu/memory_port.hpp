#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <thread>
#include <utility>

#include "xenon/cpu/memory_ordering.hpp"
#include "xenon/cpu/types.hpp"

namespace xenon::cpu {

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

  // Memory V2 reservation monitor. Xenon has six hardware threads, so active
  // reservations are represented by six compact slots rather than one atomic
  // generation counter for every 128-byte physical granule. The bitmap is a
  // sticky "has ever hosted a reservation" hint: ordinary stores to untouched
  // granules pay only a bit test and never scan the six slots.
  std::atomic<std::uint64_t>* reservation_slots{};
  std::atomic<std::uint64_t>* reservation_seen_bitmap{};
  std::uint32_t reservation_slot_count{};
  std::uint32_t reservation_seen_word_count{};
  std::uint32_t reservation_granule_size{};
  std::uint32_t reservation_granule_count{};
  std::atomic<std::uint32_t>* reservation_commit_gate{};
  std::atomic<std::uint32_t>* active_reservation_ops{};

  std::atomic<std::uint64_t>* physical_page_epochs{};
  std::uint32_t physical_page_count{};
  std::atomic<std::uint64_t>* global_write_epoch{};
  std::atomic<std::uint32_t>* active_coherency_writers{};
  std::atomic<std::uint64_t>* coherency_journal_epochs{};
  std::atomic<std::uint64_t>* coherency_journal_ranges{};
  std::uint32_t coherency_journal_capacity{};

  // Sticky executable-page generations. Zero means the physical page has not
  // hosted executable guest code. Writes increment nonzero generations so
  // native code caches can validate translations without synchronous store
  // callbacks.
  std::atomic<std::uint32_t>* executable_page_generations{};

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

  // Range-oriented access used by PPC string/vector-partial operations and
  // cache-block operations. Common RAM is translated page-by-page and performs
  // one reservation/coherency publication per physical chunk rather than one
  // callback/bookkeeping operation per byte. Exceptional pages fall back to
  // the reference MemoryPort range methods.
  void read_bytes(GuestAddress address, std::span<std::byte> destination);
  void write_bytes(GuestAddress address, std::span<const std::byte> source);
  void fill_bytes(GuestAddress address, std::uint32_t size, std::uint8_t value);
  void zero_cache_block(GuestAddress address, std::uint32_t bytes);

  [[nodiscard]] bool has_fast_path() const noexcept {
    return fast_.physical_base && fast_.page_table && fast_.page_count;
  }

  struct PhysicalResolution {
    std::byte* ptr{};
    std::uint32_t physical_address{};
  };

  // Internal fast-resolution hook used by specialized CPU operations such as
  // lwarx/stwcx. The context's read-side guard keeps the translated physical
  // backing alive for the lifetime of the returned resolution.
  [[nodiscard]] bool resolve_physical_ram(GuestAddress address,
                                          std::size_t width, bool write,
                                          std::size_t alignment,
                                          PhysicalResolution& out) const noexcept {
    Resolved resolved{};
    if (!resolve_fast(address, width, write, alignment, resolved)) return false;
    out = {resolved.ptr, resolved.physical_address};
    return true;
  }

 private:
  struct Resolved {
    std::byte* ptr{};
    std::uint32_t physical_address{};
  };

  [[nodiscard]] bool resolve_fast(GuestAddress address, std::size_t width,
                                  bool write, std::size_t alignment,
                                  Resolved& out) const noexcept;
  [[nodiscard]] bool begin_write(std::uint32_t physical_address,
                                 std::uint32_t width) const noexcept;
  void complete_write(std::uint32_t physical_address, std::uint32_t width,
                      bool reservation_participant) const noexcept;

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

  // Cold/reference range fallback. Production generated code reaches these
  // only for MMIO, faults, unusual mappings, or MemoryPort implementations
  // without a Memory V2 fast view. Concrete reference/test ports may override
  // them with native contiguous operations.
  virtual void read_bytes(GuestAddress address, std::span<std::byte> destination) {
    for (std::size_t i = 0; i < destination.size(); ++i) {
      destination[i] = static_cast<std::byte>(read8(address + static_cast<GuestAddress>(i)));
    }
  }
  virtual void write_bytes(GuestAddress address, std::span<const std::byte> source) {
    for (std::size_t i = 0; i < source.size(); ++i) {
      write8(address + static_cast<GuestAddress>(i),
             std::to_integer<std::uint8_t>(source[i]));
    }
  }
  virtual void fill_bytes(GuestAddress address, std::uint32_t size,
                          std::uint8_t value) {
    for (std::uint32_t i = 0; i < size; ++i) write8(address + i, value);
  }

  // Reservation operations return an opaque monitor token. The production
  // memory system owns invalidation across aliases and hardware threads.
  virtual std::uint64_t reserve32(GuestAddress address, std::uint32_t& value) = 0;
  virtual std::uint64_t reserve64(GuestAddress address, std::uint64_t& value) = 0;
  virtual bool store_conditional32(GuestAddress address, std::uint64_t token,
                                   std::uint32_t value) = 0;
  virtual bool store_conditional64(GuestAddress address, std::uint64_t token,
                                   std::uint64_t value) = 0;
  // A new load-reserve replaces the previous reservation held by that Xenon
  // hardware thread. Generated code calls this before overwriting CpuState's
  // token, and also when a mismatched conditional store must clear it. Test
  // ports that use generation-only reservations may leave this as a no-op.
  virtual void cancel_reservation(std::uint64_t token) noexcept { (void)token; }

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

inline void atomic_copy_from_guest(std::byte* source,
                                   std::span<std::byte> destination) noexcept {
  std::size_t offset = 0;
  while (offset < destination.size() &&
         (reinterpret_cast<std::uintptr_t>(source + offset) & 7u)) {
    destination[offset] = static_cast<std::byte>(
        atomic_load_relaxed<std::uint8_t>(source + offset));
    ++offset;
  }
  while (offset + sizeof(std::uint64_t) <= destination.size()) {
    const auto value = atomic_load_relaxed<std::uint64_t>(source + offset);
    std::memcpy(destination.data() + offset, &value, sizeof(value));
    offset += sizeof(value);
  }
  while (offset < destination.size()) {
    destination[offset] = static_cast<std::byte>(
        atomic_load_relaxed<std::uint8_t>(source + offset));
    ++offset;
  }
}

inline void atomic_copy_to_guest(std::byte* destination,
                                 std::span<const std::byte> source) noexcept {
  std::size_t offset = 0;
  while (offset < source.size() &&
         (reinterpret_cast<std::uintptr_t>(destination + offset) & 7u)) {
    atomic_store_relaxed<std::uint8_t>(
        destination + offset, std::to_integer<std::uint8_t>(source[offset]));
    ++offset;
  }
  while (offset + sizeof(std::uint64_t) <= source.size()) {
    std::uint64_t value{};
    std::memcpy(&value, source.data() + offset, sizeof(value));
    atomic_store_relaxed<std::uint64_t>(destination + offset, value);
    offset += sizeof(value);
  }
  while (offset < source.size()) {
    atomic_store_relaxed<std::uint8_t>(
        destination + offset, std::to_integer<std::uint8_t>(source[offset]));
    ++offset;
  }
}

inline void atomic_memmove_guest(std::byte* destination, std::byte* source,
                                 std::size_t size) noexcept {
  if (!size || destination == source) return;

  const bool backward = destination > source && destination < source + size;
  if (!backward) {
    std::size_t offset = 0;
    while (offset < size &&
           (((reinterpret_cast<std::uintptr_t>(destination + offset) |
              reinterpret_cast<std::uintptr_t>(source + offset)) &
             (alignof(std::uint64_t) - 1u)) != 0u)) {
      const auto value = atomic_load_relaxed<std::uint8_t>(source + offset);
      atomic_store_relaxed<std::uint8_t>(destination + offset, value);
      ++offset;
    }
    while (offset + sizeof(std::uint64_t) <= size) {
      const auto value = atomic_load_relaxed<std::uint64_t>(source + offset);
      atomic_store_relaxed<std::uint64_t>(destination + offset, value);
      offset += sizeof(std::uint64_t);
    }
    while (offset < size) {
      const auto value = atomic_load_relaxed<std::uint8_t>(source + offset);
      atomic_store_relaxed<std::uint8_t>(destination + offset, value);
      ++offset;
    }
    return;
  }

  std::size_t offset = size;
  while (offset &&
         (((reinterpret_cast<std::uintptr_t>(destination + offset) |
            reinterpret_cast<std::uintptr_t>(source + offset)) &
           (alignof(std::uint64_t) - 1u)) != 0u)) {
    --offset;
    const auto value = atomic_load_relaxed<std::uint8_t>(source + offset);
    atomic_store_relaxed<std::uint8_t>(destination + offset, value);
  }
  while (offset >= sizeof(std::uint64_t)) {
    offset -= sizeof(std::uint64_t);
    const auto value = atomic_load_relaxed<std::uint64_t>(source + offset);
    atomic_store_relaxed<std::uint64_t>(destination + offset, value);
  }
  while (offset) {
    --offset;
    const auto value = atomic_load_relaxed<std::uint8_t>(source + offset);
    atomic_store_relaxed<std::uint8_t>(destination + offset, value);
  }
}

inline void atomic_fill_guest(std::byte* destination, std::size_t size,
                              std::uint8_t value) noexcept {
  std::size_t offset = 0;
  while (offset < size &&
         (reinterpret_cast<std::uintptr_t>(destination + offset) & 7u)) {
    atomic_store_relaxed<std::uint8_t>(destination + offset, value);
    ++offset;
  }
  const auto wide = std::uint64_t{value} * 0x0101010101010101ull;
  while (offset + sizeof(std::uint64_t) <= size) {
    atomic_store_relaxed<std::uint64_t>(destination + offset, wide);
    offset += sizeof(wide);
  }
  while (offset < size) {
    atomic_store_relaxed<std::uint8_t>(destination + offset, value);
    ++offset;
  }
}

}  // namespace detail

inline bool MemoryAccessContext::resolve_fast(GuestAddress address,
                                              std::size_t width, bool write,
                                              std::size_t alignment,
                                              Resolved& out) const noexcept {
  if (!has_fast_path() || !width) return false;
  const auto last64 = std::uint64_t{address} + width - 1u;
  if (last64 > (std::numeric_limits<GuestAddress>::max)()) return false;
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

namespace reservation_monitor_detail {

inline constexpr std::uint64_t kPhysicalAddressMask = (1ull << 29u) - 1u;
inline constexpr unsigned kStatusShift = 29u;
inline constexpr std::uint64_t kStatusMask = 0x3ull << kStatusShift;
inline constexpr std::uint64_t kWidth64 = 1ull << 31u;
inline constexpr unsigned kGenerationShift = 32u;

enum class SlotStatus : std::uint64_t {
  Inactive = 0,
  Active = 1,
  Committing = 2,
};

[[nodiscard]] inline SlotStatus slot_status(std::uint64_t descriptor) noexcept {
  return static_cast<SlotStatus>((descriptor & kStatusMask) >> kStatusShift);
}
[[nodiscard]] inline std::uint32_t slot_physical_address(
    std::uint64_t descriptor) noexcept {
  return static_cast<std::uint32_t>(descriptor & kPhysicalAddressMask);
}
[[nodiscard]] inline std::uint32_t slot_generation(
    std::uint64_t descriptor) noexcept {
  return static_cast<std::uint32_t>(descriptor >> kGenerationShift);
}
[[nodiscard]] inline std::uint32_t slot_width(
    std::uint64_t descriptor) noexcept {
  return descriptor & kWidth64 ? 8u : 4u;
}
[[nodiscard]] inline std::uint64_t encode_slot(
    std::uint32_t physical_address, std::uint32_t width,
    std::uint32_t generation, SlotStatus status) noexcept {
  return (std::uint64_t{generation} << kGenerationShift) |
         (width == 8u ? kWidth64 : 0u) |
         (static_cast<std::uint64_t>(status) << kStatusShift) |
         (std::uint64_t{physical_address} & kPhysicalAddressMask);
}

[[nodiscard]] inline bool reservation_seen(const FastMemoryView& fast,
                                           std::uint32_t granule) noexcept {
  if (!fast.reservation_seen_bitmap || !fast.reservation_seen_word_count) {
    return false;
  }
  const auto word = granule >> 6u;
  if (word >= fast.reservation_seen_word_count) return false;
  const auto bit = std::uint64_t{1} << (granule & 63u);
  return (fast.reservation_seen_bitmap[word].load(std::memory_order_acquire) &
          bit) != 0u;
}

[[nodiscard]] inline bool range_has_seen_reservation(
    const FastMemoryView& fast, std::uint32_t physical_address,
    std::uint32_t width) noexcept {
  if (!width || !fast.reservation_granule_size ||
      !fast.reservation_granule_count) {
    return false;
  }
  const auto first = physical_address / fast.reservation_granule_size;
  const auto last = static_cast<std::uint32_t>(std::min<std::uint64_t>(
      (std::uint64_t{physical_address} + width - 1u) /
          fast.reservation_granule_size,
      fast.reservation_granule_count - 1u));
  for (auto granule = first; granule <= last; ++granule) {
    if (reservation_seen(fast, granule)) return true;
  }
  return false;
}

inline void invalidate_range(const FastMemoryView& fast,
                             std::uint32_t physical_address,
                             std::uint32_t width) noexcept {
  if (!width || !fast.reservation_slots || !fast.reservation_slot_count ||
      !fast.reservation_granule_size || !fast.reservation_granule_count) {
    return;
  }
  const auto first = physical_address / fast.reservation_granule_size;
  const auto last = static_cast<std::uint32_t>(std::min<std::uint64_t>(
      (std::uint64_t{physical_address} + width - 1u) /
          fast.reservation_granule_size,
      fast.reservation_granule_count - 1u));
  bool any_seen = false;
  for (auto granule = first; granule <= last; ++granule) {
    if (reservation_seen(fast, granule)) {
      any_seen = true;
      break;
    }
  }
  if (!any_seen) return;

  for (std::uint32_t slot_index = 0; slot_index < fast.reservation_slot_count;
       ++slot_index) {
    auto& slot = fast.reservation_slots[slot_index];
    auto descriptor = slot.load(std::memory_order_acquire);
    for (;;) {
      if (slot_status(descriptor) != SlotStatus::Active) break;
      const auto slot_granule =
          slot_physical_address(descriptor) / fast.reservation_granule_size;
      if (slot_granule < first || slot_granule > last) break;
      if (slot.compare_exchange_weak(descriptor, 0u,
                                     std::memory_order_acq_rel,
                                     std::memory_order_acquire)) {
        break;
      }
    }
  }
}

[[nodiscard]] inline bool enter_write(const FastMemoryView& fast,
                                      std::uint32_t physical_address,
                                      std::uint32_t width) noexcept {
  // Coherency tracks every writer. Reservation serialization is colder: only
  // granules that have ever hosted a reservation participate in the LR/SC
  // commit gate. The boolean return is carried to finish_write so a sticky-bit
  // transition while the store is in flight cannot unbalance the counter.
  if (fast.active_coherency_writers) {
    fast.active_coherency_writers->fetch_add(1u, std::memory_order_acq_rel);
  }

  const bool reservation_participant =
      range_has_seen_reservation(fast, physical_address, width) &&
      fast.active_reservation_ops;
  if (reservation_participant) {
    for (;;) {
      if (fast.reservation_commit_gate) {
        while (fast.reservation_commit_gate->load(std::memory_order_acquire) !=
               0u) {
          std::this_thread::yield();
        }
      }
      fast.active_reservation_ops->fetch_add(1u, std::memory_order_acq_rel);
      if (!fast.reservation_commit_gate ||
          fast.reservation_commit_gate->load(std::memory_order_acquire) == 0u) {
        break;
      }
      fast.active_reservation_ops->fetch_sub(1u, std::memory_order_release);
    }
  }

  invalidate_range(fast, physical_address, width);
  return reservation_participant;
}

inline void finish_write(const FastMemoryView& fast,
                         std::uint32_t physical_address,
                         std::uint32_t width,
                         bool reservation_participant) noexcept {
  if (fast.global_write_epoch && fast.physical_page_epochs &&
      fast.physical_page_count) {
    auto epoch = fast.global_write_epoch->fetch_add(
                     1u, std::memory_order_acq_rel) +
                 1u;
    if (epoch == 0u) {
      fast.global_write_epoch->store(1u, std::memory_order_release);
      epoch = 1u;
    }
    constexpr std::uint32_t kPhysicalPageShift = 12u;
    const auto first = physical_address >> kPhysicalPageShift;
    const auto last = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        (std::uint64_t{physical_address} + width - 1u) >> kPhysicalPageShift,
        fast.physical_page_count - 1u));
    for (std::uint32_t i = first; i <= last; ++i) {
      fast.physical_page_epochs[i].store(epoch, std::memory_order_release);
    }
    if (fast.coherency_journal_epochs && fast.coherency_journal_ranges &&
        fast.coherency_journal_capacity) {
      const auto index = static_cast<std::uint32_t>(epoch) &
                         (fast.coherency_journal_capacity - 1u);
      fast.coherency_journal_ranges[index].store(
          (std::uint64_t{physical_address} << 32u) | width,
          std::memory_order_relaxed);
      fast.coherency_journal_epochs[index].store(epoch,
                                                 std::memory_order_release);
    }
  }
  if (fast.executable_page_generations && fast.physical_page_count) {
    constexpr std::uint32_t kPhysicalPageShift = 12u;
    const auto first = physical_address >> kPhysicalPageShift;
    const auto last = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        (std::uint64_t{physical_address} + width - 1u) >> kPhysicalPageShift,
        fast.physical_page_count - 1u));
    for (std::uint32_t i = first; i <= last; ++i) {
      auto generation = fast.executable_page_generations[i].load(
          std::memory_order_relaxed);
      while (generation &&
             !fast.executable_page_generations[i].compare_exchange_weak(
                 generation, generation == (std::numeric_limits<std::uint32_t>::max)()
                                 ? 1u
                                 : generation + 1u,
                 std::memory_order_release, std::memory_order_relaxed)) {}
    }
  }
  if (reservation_participant && fast.active_reservation_ops) {
    fast.active_reservation_ops->fetch_sub(1u, std::memory_order_release);
  }
  if (fast.active_coherency_writers) {
    fast.active_coherency_writers->fetch_sub(1u, std::memory_order_release);
  }
}

}  // namespace reservation_monitor_detail

inline bool MemoryAccessContext::begin_write(
    std::uint32_t physical_address, std::uint32_t width) const noexcept {
  if (!width || physical_address >= fast_.physical_size) return false;
  return reservation_monitor_detail::enter_write(fast_, physical_address, width);
}

inline void MemoryAccessContext::complete_write(
    std::uint32_t physical_address, std::uint32_t width,
    bool reservation_participant) const noexcept {
  if (!width || physical_address >= fast_.physical_size) return;
  reservation_monitor_detail::finish_write(
      fast_, physical_address, width, reservation_participant);
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
  } else {
    return value;
  }
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
  const bool reservation_participant =
      begin_write(resolved.physical_address, sizeof(T));
  detail::atomic_store_relaxed<T>(resolved.ptr, byteswap_if(value, swap));
  complete_write(resolved.physical_address, sizeof(T),
                 reservation_participant);
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
  const bool reservation_participant = begin_write(resolved.physical_address, 1);
  detail::atomic_store_relaxed<std::uint8_t>(resolved.ptr, value);
  complete_write(resolved.physical_address, 1, reservation_participant);
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
  const bool reservation_participant = begin_write(resolved.physical_address, 16);
  detail::atomic_store_relaxed<std::uint64_t>(resolved.ptr, lo);
  detail::atomic_store_relaxed<std::uint64_t>(resolved.ptr + 8u, hi);
  complete_write(resolved.physical_address, 16, reservation_participant);
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

inline void MemoryAccessContext::read_bytes(
    GuestAddress address, std::span<std::byte> destination) {
  auto cursor = address;
  auto remaining = destination;
  while (!remaining.empty()) {
    const auto page_size = std::uint32_t{1} << fast_.page_shift;
    const auto page_remaining = has_fast_path()
                                    ? page_size - (cursor & (page_size - 1u))
                                    : static_cast<std::uint32_t>(remaining.size());
    const auto chunk = std::min<std::size_t>(remaining.size(), page_remaining);
    Resolved resolved{};
    if (resolve_fast(cursor, chunk, false, 1u, resolved)) {
      detail::atomic_copy_from_guest(resolved.ptr, remaining.first(chunk));
    } else {
      slow_->read_bytes(cursor, remaining.first(chunk));
    }
    cursor += static_cast<GuestAddress>(chunk);
    remaining = remaining.subspan(chunk);
  }
}

inline void MemoryAccessContext::write_bytes(
    GuestAddress address, std::span<const std::byte> source) {
  auto cursor = address;
  auto remaining = source;
  while (!remaining.empty()) {
    const auto page_size = std::uint32_t{1} << fast_.page_shift;
    const auto page_remaining = has_fast_path()
                                    ? page_size - (cursor & (page_size - 1u))
                                    : static_cast<std::uint32_t>(remaining.size());
    const auto chunk = std::min<std::size_t>(remaining.size(), page_remaining);
    Resolved resolved{};
    if (resolve_fast(cursor, chunk, true, 1u, resolved)) {
      const bool reservation_participant =
          begin_write(resolved.physical_address, static_cast<std::uint32_t>(chunk));
      detail::atomic_copy_to_guest(resolved.ptr, remaining.first(chunk));
      complete_write(resolved.physical_address, static_cast<std::uint32_t>(chunk),
                     reservation_participant);
    } else {
      slow_->write_bytes(cursor, remaining.first(chunk));
    }
    cursor += static_cast<GuestAddress>(chunk);
    remaining = remaining.subspan(chunk);
  }
}

inline void MemoryAccessContext::fill_bytes(GuestAddress address,
                                            std::uint32_t size,
                                            std::uint8_t value) {
  auto cursor = address;
  auto remaining = size;
  while (remaining) {
    const auto page_size = std::uint32_t{1} << fast_.page_shift;
    const auto page_remaining = has_fast_path()
                                    ? page_size - (cursor & (page_size - 1u))
                                    : remaining;
    const auto chunk = (std::min)(remaining, page_remaining);
    Resolved resolved{};
    if (resolve_fast(cursor, chunk, true, 1u, resolved)) {
      const bool reservation_participant =
          begin_write(resolved.physical_address, chunk);
      detail::atomic_fill_guest(resolved.ptr, chunk, value);
      complete_write(resolved.physical_address, chunk, reservation_participant);
    } else {
      slow_->fill_bytes(cursor, chunk, value);
    }
    cursor += chunk;
    remaining -= chunk;
  }
}

inline void MemoryAccessContext::zero_cache_block(GuestAddress address,
                                                   std::uint32_t bytes) {
  if (!bytes || !std::has_single_bit(bytes)) return;
  fill_bytes(address & ~(bytes - 1u), bytes, 0u);
}

}  // namespace xenon::cpu

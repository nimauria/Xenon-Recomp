#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xenon/cpu/memory_port.hpp"
#include "xenon/memory/coherency.hpp"
#include "xenon/memory/fault.hpp"
#include "xenon/memory/types.hpp"

namespace xenon::memory {

class AddressSpace;

// Scoped controlled physical-RAM access for DMA/GPU/APU style writers. The
// handle cannot outlive the scope, exposes only atomic bounded write/fill
// operations, and destruction automatically publishes the declared range to
// the reservation monitor and Xenon-owned coherency tracker. This replaces the
// old "mutate physical_data() then remember notify_external_write()" contract
// that allowed production callers to bypass bookkeeping or race CPU accesses.
class PhysicalWriteWindow {
 public:
  PhysicalWriteWindow() = default;
  ~PhysicalWriteWindow() noexcept;
  PhysicalWriteWindow(PhysicalWriteWindow&& other) noexcept;
  PhysicalWriteWindow& operator=(PhysicalWriteWindow&& other) noexcept;
  PhysicalWriteWindow(const PhysicalWriteWindow&) = delete;
  PhysicalWriteWindow& operator=(const PhysicalWriteWindow&) = delete;

  [[nodiscard]] explicit operator bool() const noexcept { return owner_ != nullptr; }
  [[nodiscard]] bool write(std::uint32_t physical_address,
                           std::span<const std::byte> source,
                           std::uint64_t* published_epoch = nullptr) noexcept;

 private:
  friend class AddressSpace;
  explicit PhysicalWriteWindow(AddressSpace* owner) noexcept : owner_(owner) {}
  void release() noexcept;
  AddressSpace* owner_{};
};

class PhysicalWriteSpan {
 public:
  PhysicalWriteSpan() = default;
  ~PhysicalWriteSpan() noexcept;
  PhysicalWriteSpan(PhysicalWriteSpan&& other) noexcept;
  PhysicalWriteSpan& operator=(PhysicalWriteSpan&& other) noexcept;
  PhysicalWriteSpan(const PhysicalWriteSpan&) = delete;
  PhysicalWriteSpan& operator=(const PhysicalWriteSpan&) = delete;

  [[nodiscard]] explicit operator bool() const noexcept { return owner_ != nullptr; }
  [[nodiscard]] std::uint32_t physical_address() const noexcept {
    return physical_address_;
  }
  [[nodiscard]] std::uint32_t size() const noexcept {
    return static_cast<std::uint32_t>(bytes_.size());
  }

  // Controlled atomic byte transfers keep DMA/GPU writes data-race-free with
  // concurrent CPU MemoryAccessContext accesses. The mutable backing pointer is
  // intentionally not exposed to production callers.
  [[nodiscard]] bool write(std::uint32_t offset,
                           std::span<const std::byte> source) noexcept;
  [[nodiscard]] bool fill(std::uint32_t offset, std::uint32_t size,
                          std::byte value) noexcept;
  // Explicit completion is optional; destruction still commits automatically.
  // The returned Xenon coherency epoch lets source-aware consumers acknowledge
  // their own write without suppressing unrelated CPU/DMA publications.
  [[nodiscard]] std::uint64_t commit() noexcept;

 private:
  friend class AddressSpace;
  PhysicalWriteSpan(AddressSpace* owner, std::uint32_t physical_address,
                    std::span<std::byte> bytes,
                    bool reservation_participant) noexcept
      : owner_(owner),
        physical_address_(physical_address),
        bytes_(bytes),
        reservation_participant_(reservation_participant) {}
  [[nodiscard]] std::uint64_t complete() noexcept;

  AddressSpace* owner_{};
  std::uint32_t physical_address_{};
  std::span<std::byte> bytes_{};
  bool reservation_participant_{};
};

enum class GuestTranslationMode : std::uint8_t {
  Auto,
  Compact,
  DirectAperture,
};

class AddressSpace final : public xenon::cpu::MemoryPort {
 public:
  using MmioRead =
      std::function<std::uint64_t(GuestAddress address, std::uint32_t width)>;
  using MmioWrite = std::function<void(GuestAddress address, std::uint32_t width,
                                       std::uint64_t value)>;
  using InvalidationCallback = std::function<void(GuestAddress address)>;
  struct MmioRange {
    GuestAddress base{};
    std::uint32_t size{};
    MmioRead read{};
    MmioWrite write{};
    std::string name{};
  };

  explicit AddressSpace(GuestTranslationMode mode = GuestTranslationMode::Auto);
  ~AddressSpace() override;
  AddressSpace(const AddressSpace&) = delete;
  AddressSpace& operator=(const AddressSpace&) = delete;

  [[nodiscard]] bool initialize();
  void reset();

  [[nodiscard]] xenon::cpu::MemoryAccessContext access_context() noexcept override;
  [[nodiscard]] GuestMemoryCoherency& coherency() noexcept { return coherency_; }
  [[nodiscard]] const GuestMemoryCoherency& coherency() const noexcept { return coherency_; }
  [[nodiscard]] bool direct_aperture_active() const noexcept;
  [[nodiscard]] bool direct_aperture_maps(GuestAddress address) const noexcept;

  [[nodiscard]] static std::span<const RegionDescriptor> regions() noexcept;
  [[nodiscard]] static const RegionDescriptor* region_for(GuestAddress address) noexcept;

  // Virtual/XEX allocation interface. Reservations create address-space
  // metadata; commits attach physical RAM frames. The 0x800/0x900 XEX views
  // intentionally share the same backing mappings.
  [[nodiscard]] bool reserve_fixed(GuestAddress base, std::uint32_t size,
                                   Protect protect);
  [[nodiscard]] bool commit_fixed(GuestAddress base, std::uint32_t size,
                                  Protect protect,
                                  bool zero_initialize = true);
  [[nodiscard]] bool allocate(std::uint32_t size, std::uint32_t alignment,
                              Protect protect, bool top_down,
                              GuestAddress& out_address,
                              std::optional<std::uint32_t> page_size = std::nullopt,
                              VirtualAllocationOptions options = {});
  [[nodiscard]] bool decommit(GuestAddress base, std::uint32_t size);
  [[nodiscard]] bool release(GuestAddress allocation_base);
  [[nodiscard]] bool protect(GuestAddress base, std::uint32_t size, Protect protect,
                             Protect* old_protect = nullptr);
  [[nodiscard]] std::optional<MappingInfo> query(GuestAddress address) const;

  // Explicitly expose physical memory when a subsystem (later Xenos/APU) owns
  // a physical allocation or needs a shared CPU/GPU mapping.
  [[nodiscard]] bool allocate_physical(std::uint32_t size, std::uint32_t alignment,
                                       bool top_down,
                                       std::uint32_t& out_physical_address);
  [[nodiscard]] bool allocate_physical(
      std::uint32_t size, const PhysicalAllocationOptions& options,
      std::uint32_t& out_physical_address);
  [[nodiscard]] bool free_physical(std::uint32_t physical_base, std::uint32_t size);
  [[nodiscard]] bool protect_physical(std::uint32_t physical_base,
                                      std::uint32_t size, Protect protect,
                                      Protect* old_protect = nullptr);
  [[nodiscard]] std::optional<PhysicalAllocationInfo> query_physical_allocation(
      std::uint32_t physical_address) const;
  [[nodiscard]] static std::optional<GuestAddress> physical_guest_alias(
      std::uint32_t physical_address,
      PhysicalPageClass page_class) noexcept;
  [[nodiscard]] bool map_virtual_to_physical(GuestAddress virtual_base,
                                             std::uint32_t physical_base,
                                             std::uint32_t size,
                                             Protect protect);
  [[nodiscard]] std::uint32_t get_physical_address(GuestAddress address) const;
  [[nodiscard]] std::uint32_t fetch32_be(GuestAddress address) override;

  // Raw physical memory is read-only outside AddressSpace and is intended only
  // for diagnostics/tests or callers that can prove guest execution is quiescent.
  // Production concurrent readers use copy_physical_range() so CPU/GPU reads
  // share the same atomic access discipline; production writers must use
  // write_physical/fill_physical/physical_write_span so reservation invalidation
  // and CPU<->GPU coherency cannot be forgotten.
  [[nodiscard]] const std::byte* physical_data(
      std::uint32_t physical_address = 0) const;
  [[nodiscard]] bool copy_physical_range(std::uint32_t physical_address,
                                         std::span<std::byte> destination) const;
  [[nodiscard]] bool write_physical(
      std::uint32_t physical_address, std::span<const std::byte> source,
      std::uint64_t* published_epoch = nullptr);
  [[nodiscard]] bool fill_physical(std::uint32_t physical_address,
                                   std::uint32_t size, std::byte value);
  [[nodiscard]] std::vector<GuestAddress> dynamic_guest_aliases_for_physical(
      std::uint32_t physical_address) const;
  [[nodiscard]] bool validate_invariants(std::string* error = nullptr) const;
  [[nodiscard]] PhysicalWriteSpan physical_write_span(
      std::uint32_t physical_address, std::uint32_t size) noexcept;
  // Rare GPU->CPU reconciliation window. It serializes only the final
  // physical-RAM ownership handoff; normal RAM accesses remain mutex-free.
  [[nodiscard]] PhysicalWriteWindow physical_write_window() noexcept;
  [[nodiscard]] std::size_t reservation_monitor_storage_bytes() const noexcept;
  [[nodiscard]] std::uint32_t executable_generation(
      GuestAddress address) const noexcept;
  [[nodiscard]] xenon::cpu::ExecutablePageStamp executable_page_stamp(
      xenon::cpu::GuestAddress address) const noexcept override;
  [[nodiscard]] MemoryTypeInfo memory_type_info(
      GuestAddress address) const noexcept;

  void zero(GuestAddress address, std::uint32_t size);
  void fill(GuestAddress address, std::uint32_t size, std::uint8_t value);
  void copy(GuestAddress dest, GuestAddress src, std::uint32_t size);
  void move(GuestAddress dest, GuestAddress src, std::uint32_t size);

  [[nodiscard]] bool add_mmio_range(GuestAddress base, std::uint32_t size,
                                    MmioRead read, MmioWrite write,
                                    std::string name = {});
  void clear_mmio_ranges();

  std::uint64_t add_invalidation_callback(InvalidationCallback callback);
  void remove_invalidation_callback(std::uint64_t id);

  // MemoryPort - this is the CPU's production memory implementation.
  std::uint8_t read8(xenon::cpu::GuestAddress address) override;
  std::uint16_t read16_be(xenon::cpu::GuestAddress address) override;
  std::uint32_t read32_be(xenon::cpu::GuestAddress address) override;
  std::uint64_t read64_be(xenon::cpu::GuestAddress address) override;
  xenon::cpu::Vector128 read128(xenon::cpu::GuestAddress address) override;

  void write8(xenon::cpu::GuestAddress address, std::uint8_t value) override;
  void write16_be(xenon::cpu::GuestAddress address, std::uint16_t value) override;
  void write32_be(xenon::cpu::GuestAddress address, std::uint32_t value) override;
  void write64_be(xenon::cpu::GuestAddress address, std::uint64_t value) override;
  void write128(xenon::cpu::GuestAddress address,
                const xenon::cpu::Vector128& value) override;

  std::uint16_t read16_le(xenon::cpu::GuestAddress address) override;
  std::uint32_t read32_le(xenon::cpu::GuestAddress address) override;
  std::uint64_t read64_le(xenon::cpu::GuestAddress address) override;
  void write16_le(xenon::cpu::GuestAddress address, std::uint16_t value) override;
  void write32_le(xenon::cpu::GuestAddress address, std::uint32_t value) override;
  void write64_le(xenon::cpu::GuestAddress address, std::uint64_t value) override;

  void read_bytes(xenon::cpu::GuestAddress address,
                  std::span<std::byte> destination) override;
  void write_bytes(xenon::cpu::GuestAddress address,
                   std::span<const std::byte> source) override;
  void fill_bytes(xenon::cpu::GuestAddress address, std::uint32_t size,
                  std::uint8_t value) override;

  std::uint64_t reserve32(xenon::cpu::GuestAddress address,
                          std::uint32_t& value) override;
  std::uint64_t reserve64(xenon::cpu::GuestAddress address,
                          std::uint64_t& value) override;
  bool store_conditional32(xenon::cpu::GuestAddress address, std::uint64_t token,
                           std::uint32_t value) override;
  bool store_conditional64(xenon::cpu::GuestAddress address, std::uint64_t token,
                           std::uint64_t value) override;
  void cancel_reservation(std::uint64_t token) noexcept override;

  [[nodiscard]] xenon::cpu::MemoryOrderingDomain ordering_domain(
      xenon::cpu::GuestAddress address) const noexcept override;

  void barrier(xenon::cpu::BarrierKind kind) override;
  void zero_cache_block(xenon::cpu::GuestAddress address,
                        std::uint32_t bytes) override;
  void instruction_cache_invalidate(xenon::cpu::GuestAddress address) override;

 private:
  static constexpr std::uint32_t kPageShift = 12;
  static constexpr std::uint32_t kPageCount = 1u << (32 - kPageShift);
  static constexpr std::uint32_t kPhysicalPageCount =
      kPhysicalMemorySize / kBasePageSize;
  static constexpr std::uint32_t kReservationGranuleCount =
      kPhysicalMemorySize / kReservationGranuleSize;
  static constexpr std::uint32_t kReservationSlotCount = 6u;
  static constexpr std::uint32_t kReservationBitmapWordCount =
      (kReservationGranuleCount + 63u) / 64u;
  static constexpr std::uint32_t kInvalidPhysicalPage = 0xFFFFFFFFu;
  static constexpr std::uint8_t kPhysicalFree = 0;
  static constexpr std::uint8_t kPhysicalSystem = 1;
  static constexpr std::uint8_t kPhysicalAnonymous = 2;
  static constexpr std::uint8_t kPhysicalExplicit = 3;
  static constexpr std::uint8_t kPhysicalAnonymousPendingFree = 4;
  static constexpr std::uint8_t kPhysicalRetired = 5;

  struct Page {
    std::uint32_t physical_page{kInvalidPhysicalPage};
    std::uint32_t allocation_base_page{};
    std::uint32_t allocation_page_count{};
    Protect allocation_protect{Protect::None};
    Protect current_protect{Protect::None};
    PageState state{PageState::Free};
    RegionKind kind{RegionKind::Virtual};
    bool explicit_physical_mapping{};
    bool permanent_guard{};
  };

  struct PhysicalPageMetadata {
    std::uint32_t allocation_base_page{kInvalidPhysicalPage};
    std::uint32_t allocation_page_count{};
    std::uint32_t allocation_page_size{kBasePageSize};
    Protect allocation_protect{kReadWrite};
    Protect current_protect{kReadWrite};
    bool explicit_allocation{};
  };

  struct ResolvedByte {
    std::byte* ptr{};
    std::uint32_t physical_address{kInvalidPhysicalPage};
    bool physical{};
  };
  struct ConstResolvedByte {
    const std::byte* ptr{};
    std::uint32_t physical_address{kInvalidPhysicalPage};
    bool physical{};
  };

  class PhysicalBacking;
  class GuestAperture;
  class PhysicalRangeAllocator;
  class PhysicalReverseMappings;

  [[nodiscard]] bool range_is_allocatable(GuestAddress base, std::uint32_t size,
                                          std::uint32_t required_page_size) const;
  [[nodiscard]] bool reserve_pages(GuestAddress base, std::uint32_t size,
                                   Protect protect, bool commit_now);
  [[nodiscard]] bool commit_pages(GuestAddress base, std::uint32_t size,
                                  Protect protect,
                                  bool zero_initialize = true);
  [[nodiscard]] bool map_xex_alias_page(std::uint32_t page_index,
                                        std::uint32_t physical_page,
                                        PageState state, Protect allocation_protect,
                                        Protect current_protect,
                                        std::uint32_t allocation_base_page,
                                        std::uint32_t allocation_page_count);

  [[nodiscard]] std::uint32_t allocate_physical_page(bool top_down);
  void free_physical_page(std::uint32_t page);
  void reclaim_retired_physical_pages();
  void add_physical_mapping_ref(std::uint32_t physical_page,
                                std::uint32_t guest_page);
  void remove_physical_mapping_ref(std::uint32_t physical_page,
                                   std::uint32_t guest_page);
  [[nodiscard]] bool validate_invariants_locked(std::string* error) const;
  [[nodiscard]] bool reserve_physical_run(std::uint32_t page_count,
                                          std::uint32_t alignment_pages,
                                          bool top_down,
                                          std::uint32_t minimum_page,
                                          std::uint32_t maximum_page_exclusive,
                                          std::uint32_t& out_first_page);
  void publish_physical_alias_range(std::uint32_t physical_address,
                                    std::uint32_t size);
  [[nodiscard]] Protect physical_protect(
      std::uint32_t physical_address) const noexcept;

  [[nodiscard]] ResolvedByte resolve_byte(GuestAddress address, AccessKind access);
  [[nodiscard]] ResolvedByte resolve_byte_for_request(
      GuestAddress fault_address, AccessKind access, GuestAddress request_address,
      std::size_t request_width);
  [[nodiscard]] std::optional<ResolvedByte> resolve_contiguous(GuestAddress address,
                                                                  std::size_t width,
                                                                  AccessKind access);
  [[nodiscard]] ConstResolvedByte resolve_byte_const(GuestAddress address,
                                                      AccessKind access) const;
  // MMIO ranges are kept sorted and non-overlapping. The hot page table only
  // answers RAM-vs-slow; actual device lookup is a cold O(log N) interval
  // lookup. Shared ownership lets callbacks run after the management lock is
  // released, so arbitrary device handlers can never execute while the global
  // AddressSpace mutex is held.
  using MmioRangeRef = std::shared_ptr<const MmioRange>;
  [[nodiscard]] MmioRangeRef find_mmio_locked(GuestAddress address,
                                              std::uint32_t width) const;
  [[nodiscard]] MmioRangeRef snapshot_mmio(GuestAddress address,
                                           std::uint32_t width) const;
  [[nodiscard]] bool range_has_mmio(GuestAddress address,
                                    std::uint32_t width) const;

  [[nodiscard]] std::uint64_t read_mmio(GuestAddress address,
                                        std::uint32_t width) const;
  void write_mmio(GuestAddress address, std::uint32_t width,
                  std::uint64_t value);

  template <typename T>
  [[nodiscard]] T read_integer(GuestAddress address, bool little_endian);
  template <typename T>
  void write_integer(GuestAddress address, T value, bool little_endian);

  friend class PhysicalWriteSpan;
  friend class PhysicalWriteWindow;
  [[nodiscard]] std::uint64_t make_hot_entry(std::uint32_t page_index) const;
  void publish_hot_entry(std::uint32_t page_index, std::uint64_t entry);
  void publish_hot_page(std::uint32_t page_index);
  void publish_hot_range(GuestAddress base, std::uint32_t size);
  void rebuild_hot_pages();
  [[nodiscard]] bool page_has_mmio(std::uint32_t page_index) const;
  [[nodiscard]] xenon::cpu::FastMemoryView make_fast_memory_view() noexcept;
  void advance_executable_generation(std::uint32_t physical_page) noexcept;
  [[nodiscard]] bool begin_physical_write(std::uint32_t physical_address,
                                          std::uint32_t width) noexcept;
  [[nodiscard]] bool write_physical_exclusive(
      std::uint32_t physical_address, std::span<const std::byte> source,
      std::uint64_t* published_epoch) noexcept;
  void release_physical_write_window() noexcept;
  std::uint64_t complete_physical_write(
      std::uint32_t physical_address, std::uint32_t width,
      bool reservation_participant,
      xenon::cpu::MemoryOrderingDomain ordering_domain) noexcept;
  void begin_reservation_operation() noexcept;
  void end_reservation_operation() noexcept;
  [[nodiscard]] std::uint64_t claim_reservation(
      std::uint32_t physical_address, std::uint32_t width) noexcept;
  [[nodiscard]] bool claim_store_conditional(
      std::uint32_t physical_address, std::uint32_t width,
      std::uint64_t token, std::uint32_t& slot_index,
      std::uint64_t& committing_descriptor) noexcept;
  void invalidate_reservations(std::uint32_t physical_address,
                               std::uint32_t width) noexcept;
  [[nodiscard]] std::uint32_t physical_alias_address(GuestAddress address) const;

  [[nodiscard]] MemoryFaultInfo make_fault_info(
      GuestAddress request_address, GuestAddress fault_address,
      std::size_t width, AccessKind access, FaultReason reason) const;
  void validate_guest_range(GuestAddress address, std::size_t width,
                            AccessKind access) const;
  [[noreturn]] void fault(GuestAddress address, std::size_t width,
                          AccessKind access, FaultReason reason,
                          const char* message) const;
  [[noreturn]] void fault_at(GuestAddress request_address,
                             GuestAddress fault_address, std::size_t width,
                             AccessKind access, FaultReason reason,
                             const char* message) const;

  std::unique_ptr<PhysicalBacking> physical_{};
  std::unique_ptr<GuestAperture> guest_aperture_{};
  std::unique_ptr<PhysicalRangeAllocator> physical_allocator_{};
  std::unique_ptr<PhysicalReverseMappings> physical_reverse_mappings_{};
  std::vector<Page> pages_{};
  std::vector<std::atomic<std::uint64_t>> hot_pages_{};
  // Cold physical ownership state is independent of virtual mapping lifetime.
  // A page can have an owner whose original mapping has gone away while other
  // aliases still hold mapping references.
  std::vector<std::uint8_t> physical_page_used_{};
  std::vector<std::uint32_t> physical_mapping_refs_{};
  std::vector<PhysicalPageMetadata> physical_page_metadata_{};
  // Six active reservation slots model Xenon's six hardware threads. The
  // 512 KiB sticky bitmap is only a hot-path hint; exact reservation state
  // remains in the slots and is always keyed by physical RAM.
  std::array<std::atomic<std::uint64_t>, kReservationSlotCount>
      reservation_slots_{};
  std::vector<std::atomic<std::uint64_t>> reservation_seen_bitmap_{};
  std::atomic<std::uint32_t> reservation_next_generation_{1u};
  std::atomic<std::uint32_t> reservation_commit_gate_{0u};
  std::atomic<std::uint32_t> active_reservation_ops_{0u};
  std::atomic<std::uint32_t> coherency_commit_gate_{0u};
  GuestMemoryCoherency coherency_{};
  std::vector<std::atomic<std::uint32_t>> executable_page_generations_{};
  std::atomic<std::uint32_t> active_fast_readers_{0};

  mutable std::recursive_mutex mutex_{};
  std::vector<MmioRangeRef> mmio_ranges_{};
  std::vector<std::pair<std::uint64_t, InvalidationCallback>>
      invalidation_callbacks_{};
  std::uint64_t next_invalidation_callback_id_{1};
  bool direct_aperture_requested_{true};
  bool initialized_{};
};

}  // namespace xenon::memory

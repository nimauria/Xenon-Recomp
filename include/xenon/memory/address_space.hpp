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
#include "xenon/memory/fault.hpp"
#include "xenon/memory/types.hpp"

namespace xenon::memory {

class AddressSpace final : public xenon::cpu::MemoryPort {
 public:
  using MmioRead =
      std::function<std::uint64_t(GuestAddress address, std::uint32_t width)>;
  using MmioWrite = std::function<void(GuestAddress address, std::uint32_t width,
                                       std::uint64_t value)>;
  using InvalidationCallback = std::function<void(GuestAddress address)>;
  using PhysicalWriteCallback =
      std::function<void(std::uint32_t physical_address, std::uint32_t width)>;

  struct MmioRange {
    GuestAddress base{};
    std::uint32_t size{};
    MmioRead read{};
    MmioWrite write{};
    std::string name{};
  };

  AddressSpace();
  ~AddressSpace() override;
  AddressSpace(const AddressSpace&) = delete;
  AddressSpace& operator=(const AddressSpace&) = delete;

  [[nodiscard]] bool initialize();
  void reset();

  [[nodiscard]] static std::span<const RegionDescriptor> regions() noexcept;
  [[nodiscard]] static const RegionDescriptor* region_for(GuestAddress address) noexcept;

  // Virtual/XEX allocation interface. Reservations create address-space
  // metadata; commits attach physical RAM frames. The 0x800/0x900 XEX views
  // intentionally share the same backing mappings.
  [[nodiscard]] bool reserve_fixed(GuestAddress base, std::uint32_t size,
                                   Protect protect);
  [[nodiscard]] bool commit_fixed(GuestAddress base, std::uint32_t size,
                                  Protect protect);
  [[nodiscard]] bool allocate(std::uint32_t size, std::uint32_t alignment,
                              Protect protect, bool top_down,
                              GuestAddress& out_address,
                              std::optional<std::uint32_t> page_size = std::nullopt);
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
  [[nodiscard]] bool free_physical(std::uint32_t physical_base, std::uint32_t size);
  [[nodiscard]] bool map_virtual_to_physical(GuestAddress virtual_base,
                                             std::uint32_t physical_base,
                                             std::uint32_t size,
                                             Protect protect);
  [[nodiscard]] std::uint32_t get_physical_address(GuestAddress address) const;
  [[nodiscard]] std::uint32_t fetch32_be(GuestAddress address);

  [[nodiscard]] std::byte* physical_data(std::uint32_t physical_address = 0);
  [[nodiscard]] const std::byte* physical_data(
      std::uint32_t physical_address = 0) const;

  void zero(GuestAddress address, std::uint32_t size);
  void fill(GuestAddress address, std::uint32_t size, std::uint8_t value);
  void copy(GuestAddress dest, GuestAddress src, std::uint32_t size);

  [[nodiscard]] bool add_mmio_range(GuestAddress base, std::uint32_t size,
                                    MmioRead read, MmioWrite write,
                                    std::string name = {});
  void clear_mmio_ranges();

  std::uint64_t add_invalidation_callback(InvalidationCallback callback);
  void remove_invalidation_callback(std::uint64_t id);

  // DMA/GPU/APU code that mutates physical_data() directly must call this so
  // CPU reservations and shared-memory observers see the write.
  void notify_external_write(std::uint32_t physical_address, std::uint32_t width);
  std::uint64_t add_physical_write_callback(PhysicalWriteCallback callback);
  void remove_physical_write_callback(std::uint64_t id);

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

  std::uint64_t reserve32(xenon::cpu::GuestAddress address,
                          std::uint32_t& value) override;
  std::uint64_t reserve64(xenon::cpu::GuestAddress address,
                          std::uint64_t& value) override;
  bool store_conditional32(xenon::cpu::GuestAddress address, std::uint64_t token,
                           std::uint32_t value) override;
  bool store_conditional64(xenon::cpu::GuestAddress address, std::uint64_t token,
                           std::uint64_t value) override;

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
  static constexpr std::uint32_t kInvalidPhysicalPage = 0xFFFFFFFFu;

  struct Page {
    std::uint32_t physical_page{kInvalidPhysicalPage};
    std::uint32_t allocation_base_page{};
    std::uint32_t allocation_page_count{};
    Protect allocation_protect{Protect::None};
    Protect current_protect{Protect::None};
    PageState state{PageState::Free};
    RegionKind kind{RegionKind::Virtual};
    bool explicit_physical_mapping{};
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

  [[nodiscard]] bool range_is_allocatable(GuestAddress base, std::uint32_t size,
                                          std::uint32_t required_page_size) const;
  [[nodiscard]] bool reserve_pages(GuestAddress base, std::uint32_t size,
                                   Protect protect, bool commit_now);
  [[nodiscard]] bool commit_pages(GuestAddress base, std::uint32_t size,
                                  Protect protect);
  [[nodiscard]] bool map_xex_alias_page(std::uint32_t page_index,
                                        std::uint32_t physical_page,
                                        PageState state, Protect allocation_protect,
                                        Protect current_protect,
                                        std::uint32_t allocation_base_page,
                                        std::uint32_t allocation_page_count);

  [[nodiscard]] std::uint32_t allocate_physical_page(bool top_down);
  void free_physical_page(std::uint32_t page);
  [[nodiscard]] bool reserve_physical_run(std::uint32_t page_count,
                                          std::uint32_t alignment_pages,
                                          bool top_down,
                                          std::uint32_t& out_first_page);

  [[nodiscard]] ResolvedByte resolve_byte(GuestAddress address, AccessKind access);
  [[nodiscard]] std::optional<ResolvedByte> resolve_contiguous(GuestAddress address,
                                                                  std::size_t width,
                                                                  AccessKind access);
  [[nodiscard]] ConstResolvedByte resolve_byte_const(GuestAddress address,
                                                      AccessKind access) const;
  [[nodiscard]] const MmioRange* find_mmio(GuestAddress address,
                                           std::uint32_t width) const;
  [[nodiscard]] MmioRange* find_mmio(GuestAddress address, std::uint32_t width);

  [[nodiscard]] std::uint64_t read_mmio(GuestAddress address,
                                        std::uint32_t width) const;
  void write_mmio(GuestAddress address, std::uint32_t width,
                  std::uint64_t value);

  template <typename T>
  [[nodiscard]] T read_integer(GuestAddress address, bool little_endian);
  template <typename T>
  void write_integer(GuestAddress address, T value, bool little_endian);

  void note_physical_write(std::uint32_t physical_address, std::uint32_t width);
  [[nodiscard]] std::uint64_t reservation_version(
      std::uint32_t physical_address) const;
  [[nodiscard]] std::uint32_t physical_alias_address(GuestAddress address) const;

  [[noreturn]] static void fault(GuestAddress address, std::size_t width,
                                 AccessKind access, FaultReason reason,
                                 const char* message);

  std::unique_ptr<PhysicalBacking> physical_{};
  std::vector<Page> pages_{};
  std::vector<std::uint8_t> physical_page_used_{};
  std::vector<std::atomic<std::uint32_t>> reservation_versions_{};

  mutable std::recursive_mutex mutex_{};
  std::vector<MmioRange> mmio_ranges_{};
  std::vector<std::pair<std::uint64_t, InvalidationCallback>>
      invalidation_callbacks_{};
  std::uint64_t next_invalidation_callback_id_{1};
  std::vector<std::pair<std::uint64_t, PhysicalWriteCallback>>
      physical_write_callbacks_{};
  std::uint64_t next_physical_write_callback_id_{1};
  bool initialized_{};
};

}  // namespace xenon::memory

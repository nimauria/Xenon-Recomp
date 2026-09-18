#include "xenon/memory/address_space.hpp"
#include "xenon/memory/host_vm.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>


namespace xenon::memory {
namespace {

constexpr std::array<RegionDescriptor, 9> kRegions{{
    {kVirtual4KBase, kVirtual4KEnd, kBasePageSize, RegionKind::Virtual},
    {kVirtual64KBase, kVirtual64KEnd, kLargePageSize, RegionKind::Virtual},
    {kGpuWritebackBase, kGpuWritebackEnd, kLargePageSize, RegionKind::GpuWriteback},
    {kXex64KBase, kXex64KEnd, kLargePageSize, RegionKind::Xex},
    {kXex4KBase, kXex4KEnd, kBasePageSize, RegionKind::Xex},
    {kPhysical64KBase, kPhysical64KEnd, kLargePageSize, RegionKind::PhysicalAlias},
    {kPhysical16MBase, kPhysical16MEnd, kHugePageSize, RegionKind::PhysicalAlias},
    {kPhysical4KBase, kPhysical4KHeapEnd, kBasePageSize, RegionKind::PhysicalAlias},
    {kMmioBase, kMmioEnd, kBasePageSize, RegionKind::Mmio},
}};

constexpr std::uint32_t align_down(std::uint32_t value, std::uint32_t alignment) {
  return value & ~(alignment - 1u);
}
constexpr std::uint32_t align_up(std::uint32_t value, std::uint32_t alignment) {
  const std::uint64_t v = value;
  return static_cast<std::uint32_t>((v + alignment - 1u) & ~(std::uint64_t(alignment) - 1u));
}
constexpr std::uint32_t page_count_for(std::uint32_t size) {
  return (size + kBasePageSize - 1u) / kBasePageSize;
}
constexpr bool overlaps(std::uint32_t a_base, std::uint32_t a_size,
                        std::uint32_t b_base, std::uint32_t b_size) {
  const std::uint64_t a_end = std::uint64_t(a_base) + a_size;
  const std::uint64_t b_end = std::uint64_t(b_base) + b_size;
  return std::uint64_t(a_base) < b_end && std::uint64_t(b_base) < a_end;
}

template <typename T>
constexpr T byteswap_if(T value, bool do_swap);

constexpr std::uint64_t kReservationTokenSlotMask = 0x7ull;

[[nodiscard]] constexpr std::uint64_t make_reservation_token(
    std::uint32_t slot_index, std::uint32_t generation) noexcept {
  return (std::uint64_t{generation} << 3u) |
         std::uint64_t{slot_index + 1u};
}

[[nodiscard]] constexpr bool decode_reservation_token(
    std::uint64_t token, std::uint32_t& slot_index,
    std::uint32_t& generation) noexcept {
  const auto encoded_slot =
      static_cast<std::uint32_t>(token & kReservationTokenSlotMask);
  if (encoded_slot == 0u || encoded_slot > 6u) return false;
  slot_index = encoded_slot - 1u;
  generation = static_cast<std::uint32_t>(token >> 3u);
  return generation != 0u;
}

template <typename T>
[[nodiscard]] T atomic_load_guest_be(std::byte* ptr) noexcept {
  T raw{};
  if ((reinterpret_cast<std::uintptr_t>(ptr) & (alignof(T) - 1u)) == 0u) {
    auto& object = *reinterpret_cast<T*>(ptr);
    std::atomic_ref<T> atomic(object);
    raw = atomic.load(std::memory_order_relaxed);
  } else {
    auto* bytes = reinterpret_cast<std::uint8_t*>(ptr);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
      std::atomic_ref<std::uint8_t> atomic(bytes[i]);
      raw = static_cast<T>((raw << 8u) |
                           atomic.load(std::memory_order_relaxed));
    }
    return raw;
  }
  return byteswap_if(raw, std::endian::native == std::endian::little);
}

template <typename T>
void atomic_store_guest_be(std::byte* ptr, T value) noexcept {
  if ((reinterpret_cast<std::uintptr_t>(ptr) & (alignof(T) - 1u)) == 0u) {
    const auto raw =
        byteswap_if(value, std::endian::native == std::endian::little);
    auto& object = *reinterpret_cast<T*>(ptr);
    std::atomic_ref<T> atomic(object);
    atomic.store(raw, std::memory_order_relaxed);
    return;
  }
  auto* bytes = reinterpret_cast<std::uint8_t*>(ptr);
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    const auto shift = static_cast<unsigned>((sizeof(T) - 1u - i) * 8u);
    std::atomic_ref<std::uint8_t> atomic(bytes[i]);
    atomic.store(static_cast<std::uint8_t>((value >> shift) & 0xFFu),
                 std::memory_order_relaxed);
  }
}

template <typename T>
constexpr T byteswap_if(T value, bool do_swap) {
  if (!do_swap) return value;
  if constexpr (sizeof(T) == 2) {
    const auto v = static_cast<std::uint16_t>(value);
    return static_cast<T>((v << 8) | (v >> 8));
  }
  else if constexpr (sizeof(T) == 4) {
    const auto v = static_cast<std::uint32_t>(value);
    return static_cast<T>(((v & 0x000000FFu) << 24) |
                          ((v & 0x0000FF00u) << 8) |
                          ((v & 0x00FF0000u) >> 8) |
                          ((v & 0xFF000000u) >> 24));
  }
  else if constexpr (sizeof(T) == 8) {
    const auto v = static_cast<std::uint64_t>(value);
    return static_cast<T>(((v & 0x00000000000000FFull) << 56) |
                          ((v & 0x000000000000FF00ull) << 40) |
                          ((v & 0x0000000000FF0000ull) << 24) |
                          ((v & 0x00000000FF000000ull) << 8) |
                          ((v & 0x000000FF00000000ull) >> 8) |
                          ((v & 0x0000FF0000000000ull) >> 24) |
                          ((v & 0x00FF000000000000ull) >> 40) |
                          ((v & 0xFF00000000000000ull) >> 56));
  }
  else {
    return value;
  }
}

}  // namespace

class AddressSpace::PhysicalBacking {
 public:
  PhysicalBacking() = default;
  ~PhysicalBacking() { dispose(); }

  bool initialize() {
    if (data_) return true;
    auto* reservation = host_vm::reserve(kPhysicalMemorySize);
    if (!reservation) return false;
    if (!host_vm::commit(reservation, kPhysicalMemorySize,
                         host_vm::Protection::ReadWrite)) {
      host_vm::release(reservation, kPhysicalMemorySize);
      return false;
    }
    data_ = static_cast<std::byte*>(reservation);
    return true;
  }

  void dispose() noexcept {
    if (!data_) return;
    host_vm::release(data_, kPhysicalMemorySize);
    data_ = nullptr;
  }

  void reset() {
    if (!data_) return;
    if (!host_vm::discard(data_, kPhysicalMemorySize,
                          host_vm::Protection::ReadWrite)) {
      // The host VM abstraction is allowed to fall back internally, but a
      // failed discard here must never leave guest RAM containing stale data.
      std::memset(data_, 0, kPhysicalMemorySize);
    }
  }

  void discard_page(std::uint32_t page) noexcept {
    discard_range(page, 1u);
  }

  void discard_range(std::uint32_t first_page, std::uint32_t page_count) noexcept {
    if (!data_ || !page_count || first_page >= kPhysicalPageCount ||
        std::uint64_t(first_page) + page_count > kPhysicalPageCount) {
      return;
    }
    auto* address = data_ + std::size_t(first_page) * kBasePageSize;
    const auto size = std::size_t(page_count) * kBasePageSize;
    if (!host_vm::discard(address, size, host_vm::Protection::ReadWrite)) {
      std::memset(address, 0, size);
    }
  }

  std::byte* data() noexcept { return data_; }
  const std::byte* data() const noexcept { return data_; }

 private:
  std::byte* data_{};
};


class AddressSpace::PhysicalRangeAllocator {
 public:
  void reset(std::uint32_t first_page, std::uint32_t page_count) {
    free_ranges_.clear();
    retired_ranges_.clear();
    if (page_count) free_ranges_.emplace(first_page, page_count);
  }

  [[nodiscard]] bool allocate(std::uint32_t page_count,
                              std::uint32_t alignment_pages, bool top_down,
                              std::uint32_t& out_first_page) {
    if (!page_count || !alignment_pages ||
        !std::has_single_bit(alignment_pages)) {
      return false;
    }

    if (!top_down) {
      for (auto it = free_ranges_.begin(); it != free_ranges_.end(); ++it) {
        const auto range_first = it->first;
        const auto range_end = std::uint64_t(range_first) + it->second;
        const auto candidate = align_page_up(range_first, alignment_pages);
        if (std::uint64_t(candidate) + page_count > range_end) continue;
        consume(it, candidate, page_count);
        out_first_page = candidate;
        return true;
      }
      return false;
    }

    for (auto rit = free_ranges_.rbegin(); rit != free_ranges_.rend(); ++rit) {
      const auto range_first = rit->first;
      const auto range_end = std::uint64_t(range_first) + rit->second;
      if (rit->second < page_count) continue;
      const auto latest = static_cast<std::uint32_t>(range_end - page_count);
      const auto candidate = align_page_down(latest, alignment_pages);
      if (candidate < range_first) continue;
      auto it = std::prev(rit.base());
      consume(it, candidate, page_count);
      out_first_page = candidate;
      return true;
    }
    return false;
  }

  [[nodiscard]] bool contains_free(std::uint32_t first_page,
                                   std::uint32_t page_count) const {
    return contains(free_ranges_, first_page, page_count);
  }

  [[nodiscard]] bool contains_retired(std::uint32_t first_page,
                                      std::uint32_t page_count) const {
    return contains(retired_ranges_, first_page, page_count);
  }

  [[nodiscard]] bool claim(std::uint32_t first_page,
                           std::uint32_t page_count) {
    if (!contains_free(first_page, page_count)) return false;
    auto it = free_ranges_.upper_bound(first_page);
    --it;
    consume(it, first_page, page_count);
    return true;
  }

  [[nodiscard]] bool release(std::uint32_t first_page,
                             std::uint32_t page_count) {
    return insert_coalesced(free_ranges_, first_page, page_count);
  }

  [[nodiscard]] bool retire(std::uint32_t first_page,
                            std::uint32_t page_count) {
    return insert_coalesced(retired_ranges_, first_page, page_count);
  }

  [[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint32_t>>
  take_retired_ranges() {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
    ranges.reserve(retired_ranges_.size());
    for (const auto& range : retired_ranges_) ranges.push_back(range);
    retired_ranges_.clear();
    return ranges;
  }

 private:
  using RangeMap = std::map<std::uint32_t, std::uint32_t>;

  static bool contains(const RangeMap& ranges, std::uint32_t first_page,
                       std::uint32_t page_count) {
    if (!page_count) return false;
    auto it = ranges.upper_bound(first_page);
    if (it == ranges.begin()) return false;
    --it;
    const auto range_end = std::uint64_t(it->first) + it->second;
    return first_page >= it->first &&
           std::uint64_t(first_page) + page_count <= range_end;
  }

  static bool insert_coalesced(RangeMap& ranges, std::uint32_t first_page,
                               std::uint32_t page_count) {
    if (!page_count) return false;
    const auto end_page = std::uint64_t(first_page) + page_count;
    if (end_page > kPhysicalPageCount) return false;

    auto next = ranges.lower_bound(first_page);
    if (next != ranges.end() && end_page > next->first) return false;

    auto prev = next;
    if (prev != ranges.begin()) {
      --prev;
      const auto prev_end = std::uint64_t(prev->first) + prev->second;
      if (prev_end > first_page) return false;
    } else {
      prev = ranges.end();
    }

    std::uint32_t merged_first = first_page;
    std::uint32_t merged_count = page_count;
    if (prev != ranges.end() &&
        std::uint64_t(prev->first) + prev->second == first_page) {
      merged_first = prev->first;
      merged_count += prev->second;
      ranges.erase(prev);
    }

    next = ranges.lower_bound(merged_first);
    if (next != ranges.end() &&
        std::uint64_t(merged_first) + merged_count == next->first) {
      merged_count += next->second;
      ranges.erase(next);
    }

    ranges.emplace(merged_first, merged_count);
    return true;
  }

  static std::uint32_t align_page_up(std::uint32_t value,
                                     std::uint32_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
  }

  static std::uint32_t align_page_down(std::uint32_t value,
                                       std::uint32_t alignment) {
    return value & ~(alignment - 1u);
  }

  void consume(RangeMap::iterator it, std::uint32_t first_page,
               std::uint32_t page_count) {
    const auto range_first = it->first;
    const auto range_count = it->second;
    const auto range_end = range_first + range_count;
    const auto allocation_end = first_page + page_count;
    free_ranges_.erase(it);
    if (range_first < first_page) {
      free_ranges_.emplace(range_first, first_page - range_first);
    }
    if (allocation_end < range_end) {
      free_ranges_.emplace(allocation_end, range_end - allocation_end);
    }
  }

  RangeMap free_ranges_{};
  RangeMap retired_ranges_{};
};

class AddressSpace::PhysicalReverseMappings {
 public:
  void clear() { mappings_.clear(); }

  [[nodiscard]] bool add(std::uint32_t physical_page,
                         std::uint32_t guest_page) {
    auto& guests = mappings_[physical_page];
    if (std::find(guests.begin(), guests.end(), guest_page) != guests.end()) {
      return false;
    }
    guests.push_back(guest_page);
    return true;
  }

  [[nodiscard]] bool remove(std::uint32_t physical_page,
                            std::uint32_t guest_page) {
    const auto it = mappings_.find(physical_page);
    if (it == mappings_.end()) return false;
    auto& guests = it->second;
    const auto guest_it = std::find(guests.begin(), guests.end(), guest_page);
    if (guest_it == guests.end()) return false;
    *guest_it = guests.back();
    guests.pop_back();
    if (guests.empty()) mappings_.erase(it);
    return true;
  }

  [[nodiscard]] std::size_t count(std::uint32_t physical_page) const {
    const auto it = mappings_.find(physical_page);
    return it == mappings_.end() ? 0u : it->second.size();
  }

  [[nodiscard]] bool contains(std::uint32_t physical_page,
                              std::uint32_t guest_page) const {
    const auto it = mappings_.find(physical_page);
    if (it == mappings_.end()) return false;
    const auto& guests = it->second;
    return std::find(guests.begin(), guests.end(), guest_page) != guests.end();
  }

  [[nodiscard]] std::vector<std::uint32_t> guests(
      std::uint32_t physical_page) const {
    const auto it = mappings_.find(physical_page);
    return it == mappings_.end() ? std::vector<std::uint32_t>{} : it->second;
  }

  using Map = std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>;
  [[nodiscard]] const Map& entries() const noexcept { return mappings_; }

 private:
  Map mappings_{};
};

PhysicalWriteSpan::~PhysicalWriteSpan() noexcept { complete(); }

PhysicalWriteSpan::PhysicalWriteSpan(PhysicalWriteSpan&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      physical_address_(other.physical_address_),
      bytes_(other.bytes_),
      reservation_participant_(other.reservation_participant_) {
  other.bytes_ = {};
}

PhysicalWriteSpan& PhysicalWriteSpan::operator=(PhysicalWriteSpan&& other) noexcept {
  if (this == &other) return *this;
  complete();
  owner_ = std::exchange(other.owner_, nullptr);
  physical_address_ = other.physical_address_;
  bytes_ = other.bytes_;
  reservation_participant_ = other.reservation_participant_;
  other.bytes_ = {};
  other.reservation_participant_ = false;
  return *this;
}

bool PhysicalWriteSpan::write(
    std::uint32_t offset, std::span<const std::byte> source) noexcept {
  if (!owner_ || offset > bytes_.size() ||
      source.size() > bytes_.size() - offset) {
    return false;
  }
  if (!source.empty()) {
    xenon::cpu::detail::atomic_copy_to_guest(bytes_.data() + offset, source);
  }
  return true;
}

bool PhysicalWriteSpan::fill(std::uint32_t offset, std::uint32_t size,
                             std::byte value) noexcept {
  if (!owner_ || offset > bytes_.size() || size > bytes_.size() - offset) {
    return false;
  }
  if (size) {
    xenon::cpu::detail::atomic_fill_guest(
        bytes_.data() + offset, size, std::to_integer<std::uint8_t>(value));
  }
  return true;
}

void PhysicalWriteSpan::complete() noexcept {
  if (!owner_) return;
  auto* owner = std::exchange(owner_, nullptr);
  const auto address = physical_address_;
  const auto size = static_cast<std::uint32_t>(bytes_.size());
  bytes_ = {};
  if (size) owner->complete_physical_write(address, size, reservation_participant_);
  reservation_participant_ = false;
}

AddressSpace::AddressSpace()
    : physical_(std::make_unique<PhysicalBacking>()),
      physical_allocator_(std::make_unique<PhysicalRangeAllocator>()),
      physical_reverse_mappings_(std::make_unique<PhysicalReverseMappings>()),
      pages_(kPageCount),
      hot_pages_(kPageCount),
      physical_page_used_(kPhysicalPageCount),
      physical_mapping_refs_(kPhysicalPageCount),
      executable_page_generations_(kPhysicalPageCount),
      reservation_seen_bitmap_(kReservationBitmapWordCount) {
  for (auto& entry : hot_pages_) entry.store(0u, std::memory_order_relaxed);
  for (auto& generation : executable_page_generations_) {
    generation.store(0u, std::memory_order_relaxed);
  }
  for (auto& slot : reservation_slots_) slot.store(0u, std::memory_order_relaxed);
  for (auto& word : reservation_seen_bitmap_) {
    word.store(0u, std::memory_order_relaxed);
  }
}

AddressSpace::~AddressSpace() = default;

xenon::cpu::FastMemoryView AddressSpace::make_fast_memory_view() noexcept {
  xenon::cpu::FastMemoryView view{};
  if (initialized_ && physical_ && physical_->data()) {
    view.physical_base = physical_->data();
    view.page_table = hot_pages_.data();
    view.page_count = kPageCount;
    view.page_shift = kPageShift;
    view.physical_size = kPhysicalMemorySize;
    view.reservation_slots = reservation_slots_.data();
    view.reservation_seen_bitmap = reservation_seen_bitmap_.data();
    view.reservation_slot_count = kReservationSlotCount;
    view.reservation_seen_word_count = kReservationBitmapWordCount;
    view.reservation_granule_size = kReservationGranuleSize;
    view.reservation_granule_count = kReservationGranuleCount;
    view.reservation_commit_gate = &reservation_commit_gate_;
    view.active_reservation_ops = &active_reservation_ops_;
    view.physical_page_epochs = coherency_.page_epochs_data();
    view.physical_page_count = kPhysicalPageCount;
    view.global_write_epoch = coherency_.write_epoch_data();
    view.active_coherency_writers = coherency_.active_writers_data();
    view.coherency_journal_epochs = coherency_.write_journal_epochs_data();
    view.coherency_journal_ranges = coherency_.write_journal_ranges_data();
    view.coherency_journal_capacity = GuestMemoryCoherency::kWriteJournalCapacity;
    view.executable_page_generations = executable_page_generations_.data();
    view.active_fast_readers = &active_fast_readers_;
  }
  return view;
}

xenon::cpu::MemoryAccessContext AddressSpace::access_context() noexcept {
  return xenon::cpu::MemoryAccessContext(*this, make_fast_memory_view());
}

bool AddressSpace::initialize() {
  std::lock_guard lock(mutex_);
  if (initialized_) return true;
  if (!physical_->initialize()) return false;
  initialized_ = true;
  reset();
  return true;
}

void AddressSpace::reset() {
  std::lock_guard lock(mutex_);
  if (!initialized_) return;
  physical_->reset();
  std::fill(pages_.begin(), pages_.end(), Page{});
  std::fill(physical_page_used_.begin(), physical_page_used_.end(), kPhysicalFree);
  std::fill(physical_mapping_refs_.begin(), physical_mapping_refs_.end(), 0u);
  for (auto& generation : executable_page_generations_) {
    generation.store(0u, std::memory_order_relaxed);
  }
  physical_reverse_mappings_->clear();
  for (auto& slot : reservation_slots_) slot.store(0u, std::memory_order_relaxed);
  for (auto& word : reservation_seen_bitmap_) {
    word.store(0u, std::memory_order_relaxed);
  }
  reservation_next_generation_.store(1u, std::memory_order_relaxed);
  reservation_commit_gate_.store(0u, std::memory_order_relaxed);
  active_reservation_ops_.store(0u, std::memory_order_relaxed);

  // The first 16 MiB are the GPU writeback/XPS physical window. Keep them out
  // of the anonymous physical-frame allocator while preserving direct aliases.
  const auto system_page_count = kHugePageSize / kBasePageSize;
  std::fill_n(physical_page_used_.begin(), system_page_count, kPhysicalSystem);
  physical_allocator_->reset(system_page_count,
                             kPhysicalPageCount - system_page_count);

  // Match the retail user virtual behavior: null/low-memory guard region.
  const auto guard_pages = 0x10000u / kBasePageSize;
  for (std::uint32_t i = 0; i < guard_pages; ++i) {
    auto& page = pages_[i];
    page.state = PageState::Reserved;
    page.allocation_base_page = 0;
    page.allocation_page_count = guard_pages;
    page.allocation_protect = Protect::None;
    page.current_protect = Protect::None;
    page.kind = RegionKind::Virtual;
  }
  rebuild_hot_pages();
  coherency_.mark_all_dirty();
}

std::span<const RegionDescriptor> AddressSpace::regions() noexcept { return kRegions; }

const RegionDescriptor* AddressSpace::region_for(GuestAddress address) noexcept {
  for (const auto& region : kRegions) {
    if (address >= region.base && address <= region.end) return &region;
  }
  return nullptr;
}

bool AddressSpace::page_has_mmio(std::uint32_t page_index) const {
  const auto page_base = page_index << kPageShift;
  const auto page_end = std::uint64_t{page_base} + kBasePageSize;
  for (const auto& range : mmio_ranges_) {
    const auto range_end = std::uint64_t{range.base} + range.size;
    if (std::uint64_t{page_base} < range_end &&
        std::uint64_t{range.base} < page_end) {
      return true;
    }
  }
  return false;
}

std::uint64_t AddressSpace::make_hot_entry(std::uint32_t page_index) const {
  if (page_index >= kPageCount) return 0;
  const auto address = page_index << kPageShift;
  const bool slow = page_has_mmio(page_index);

  const auto direct = physical_alias_address(address);
  if (direct != 0xFFFFFFFFu && direct < kPhysicalMemorySize) {
    return xenon::cpu::fast_memory::encode_page(
        direct >> kPageShift, true, true, false, slow);
  }

  const auto* region = region_for(address);
  if (!region || region->kind == RegionKind::Mmio) {
    return slow || (region && region->kind == RegionKind::Mmio)
               ? xenon::cpu::fast_memory::kSlow
               : 0u;
  }
  if (region->kind != RegionKind::Virtual && region->kind != RegionKind::Xex) {
    return slow ? xenon::cpu::fast_memory::kSlow : 0u;
  }

  const auto& page = pages_[page_index];
  if (page.state != PageState::Committed ||
      page.physical_page == kInvalidPhysicalPage) {
    return slow ? xenon::cpu::fast_memory::kSlow : 0u;
  }

  return xenon::cpu::fast_memory::encode_page(
      page.physical_page, has(page.current_protect, Protect::Read),
      has(page.current_protect, Protect::Write),
      has(page.current_protect, Protect::Execute), slow,
      has(page.current_protect, Protect::NoCache),
      has(page.current_protect, Protect::WriteCombine));
}

void AddressSpace::publish_hot_page(std::uint32_t page_index) {
  if (page_index >= kPageCount) return;
  const auto entry = make_hot_entry(page_index);
  if ((entry & (xenon::cpu::fast_memory::kMapped |
                xenon::cpu::fast_memory::kExecute)) ==
      (xenon::cpu::fast_memory::kMapped |
       xenon::cpu::fast_memory::kExecute)) {
    const auto physical_page = static_cast<std::uint32_t>(
        entry & xenon::cpu::fast_memory::kPhysicalPageMask);
    if (physical_page < executable_page_generations_.size()) {
      std::uint32_t expected = 0u;
      (void)executable_page_generations_[physical_page].compare_exchange_strong(
          expected, 1u, std::memory_order_release, std::memory_order_relaxed);
    }
  }
  hot_pages_[page_index].store(entry, std::memory_order_release);
}

void AddressSpace::publish_hot_range(GuestAddress base, std::uint32_t size) {
  if (!size) return;
  const auto first = base >> kPageShift;
  const auto last64 = (std::uint64_t{base} + size - 1u) >> kPageShift;
  const auto last = static_cast<std::uint32_t>(
      std::min<std::uint64_t>(last64, kPageCount - 1u));
  for (std::uint32_t page = first; page <= last; ++page) {
    publish_hot_page(page);
  }
}

void AddressSpace::rebuild_hot_pages() {
  for (std::uint32_t page = 0; page < kPageCount; ++page) {
    publish_hot_page(page);
  }
}

bool AddressSpace::range_is_allocatable(GuestAddress base, std::uint32_t size,
                                        std::uint32_t required_page_size) const {
  if (!size || !std::has_single_bit(required_page_size)) return false;
  const auto* region = region_for(base);
  if (!region || (region->kind != RegionKind::Virtual && region->kind != RegionKind::Xex)) return false;
  if (required_page_size != region->allocation_page_size) return false;
  if ((base & (required_page_size - 1u)) != 0 || (size & (required_page_size - 1u)) != 0) return false;
  const std::uint64_t end = std::uint64_t(base) + size - 1u;
  return end <= region->end;
}

bool AddressSpace::reserve_fixed(GuestAddress base, std::uint32_t size, Protect protect) {
  std::lock_guard lock(mutex_);
  if (!initialized_) return false;
  const auto* region = region_for(base);
  if (!region || !range_is_allocatable(base, size, region->allocation_page_size)) return false;
  return reserve_pages(base, size, protect, false);
}

bool AddressSpace::commit_fixed(GuestAddress base, std::uint32_t size, Protect protect) {
  std::lock_guard lock(mutex_);
  if (!initialized_) return false;
  const auto* region = region_for(base);
  if (!region || !range_is_allocatable(base, size, region->allocation_page_size)) return false;
  const std::uint32_t first = base >> kPageShift;
  const std::uint32_t count = size >> kPageShift;
  bool all_free = true;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (pages_[first + i].state != PageState::Free) {
      all_free = false;
      break;
    }
  }
  if (all_free && !reserve_pages(base, size, protect, false)) return false;
  return commit_pages(base, size, protect);
}

bool AddressSpace::reserve_pages(GuestAddress base, std::uint32_t size, Protect protect,
                                 bool commit_now) {
  const auto* region = region_for(base);
  if (!region) return false;
  const std::uint32_t first = base >> kPageShift;
  const std::uint32_t count = size >> kPageShift;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (pages_[first + i].state != PageState::Free) return false;
    if (region->kind == RegionKind::Xex) {
      const GuestAddress addr = base + i * kBasePageSize;
      const GuestAddress alias = addr < kXex4KBase ? addr + 0x10000000u : addr - 0x10000000u;
      if (pages_[alias >> kPageShift].state != PageState::Free) return false;
    }
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    auto& page = pages_[first + i];
    page.state = PageState::Reserved;
    page.allocation_base_page = first;
    page.allocation_page_count = count;
    page.allocation_protect = protect;
    page.current_protect = protect;
    page.kind = region->kind;
    if (region->kind == RegionKind::Xex) {
      const GuestAddress addr = base + i * kBasePageSize;
      const GuestAddress alias = addr < kXex4KBase ? addr + 0x10000000u : addr - 0x10000000u;
      auto& alias_page = pages_[alias >> kPageShift];
      alias_page = page;
      alias_page.allocation_base_page =
          (base < kXex4KBase ? base + 0x10000000u : base - 0x10000000u) >> kPageShift;
    }
  }
  publish_hot_range(base, size);
  if (region->kind == RegionKind::Xex) {
    const auto alias_base = base < kXex4KBase ? base + 0x10000000u
                                               : base - 0x10000000u;
    publish_hot_range(alias_base, size);
  }
  return !commit_now || commit_pages(base, size, protect);
}

bool AddressSpace::commit_pages(GuestAddress base, std::uint32_t size, Protect protect) {
  const auto* region = region_for(base);
  if (!region) return false;
  const std::uint32_t first = base >> kPageShift;
  const std::uint32_t count = size >> kPageShift;
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto& page = pages_[first + i];
    if (page.state == PageState::Free) return false;
  }

  std::vector<std::uint32_t> newly_allocated;
  newly_allocated.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    auto& page = pages_[first + i];
    if (page.state == PageState::Committed) {
      page.current_protect = protect;
      if (region->kind == RegionKind::Xex) {
        const GuestAddress addr = base + i * kBasePageSize;
        const GuestAddress alias = addr < kXex4KBase
                                       ? addr + 0x10000000u
                                       : addr - 0x10000000u;
        pages_[alias >> kPageShift].current_protect = protect;
      }
      continue;
    }
    const auto physical_page = allocate_physical_page(false);
    if (physical_page == kInvalidPhysicalPage) {
      for (std::uint32_t j = 0; j < i; ++j) {
        auto& rollback = pages_[first + j];
        if (rollback.physical_page == kInvalidPhysicalPage ||
            std::find(newly_allocated.begin(), newly_allocated.end(),
                      rollback.physical_page) == newly_allocated.end()) {
          continue;
        }
        const auto rollback_physical = rollback.physical_page;
        remove_physical_mapping_ref(rollback_physical, first + j);
        if (region->kind == RegionKind::Xex) {
          const GuestAddress addr = base + j * kBasePageSize;
          const GuestAddress alias = addr < kXex4KBase
                                         ? addr + 0x10000000u
                                         : addr - 0x10000000u;
          const auto alias_page_index = alias >> kPageShift;
          auto& alias_page = pages_[alias_page_index];
          remove_physical_mapping_ref(rollback_physical, alias_page_index);
          alias_page.physical_page = kInvalidPhysicalPage;
          alias_page.state = PageState::Reserved;
        }
        rollback.physical_page = kInvalidPhysicalPage;
        rollback.state = PageState::Reserved;
        free_physical_page(rollback_physical);
      }
      return false;
    }
    newly_allocated.push_back(physical_page);
    page.physical_page = physical_page;
    page.state = PageState::Committed;
    page.current_protect = protect;
    add_physical_mapping_ref(physical_page, first + i);
    const auto physical_address = physical_page * kBasePageSize;
    const bool reservation_participant =
        begin_physical_write(physical_address, kBasePageSize);
    std::memset(physical_->data() + std::size_t(physical_page) * kBasePageSize, 0,
                kBasePageSize);
    complete_physical_write(physical_address, kBasePageSize,
                            reservation_participant);

    if (region->kind == RegionKind::Xex) {
      const GuestAddress addr = base + i * kBasePageSize;
      const GuestAddress alias = addr < kXex4KBase ? addr + 0x10000000u : addr - 0x10000000u;
      const auto alias_page_index = alias >> kPageShift;
      auto& alias_page = pages_[alias_page_index];
      alias_page.physical_page = physical_page;
      alias_page.state = PageState::Committed;
      alias_page.current_protect = protect;
      add_physical_mapping_ref(physical_page, alias_page_index);
    }
  }
  publish_hot_range(base, size);
  if (region->kind == RegionKind::Xex) {
    const auto alias_base = base < kXex4KBase ? base + 0x10000000u
                                               : base - 0x10000000u;
    publish_hot_range(alias_base, size);
  }
  return true;
}

bool AddressSpace::allocate(std::uint32_t size, std::uint32_t alignment, Protect protect,
                            bool top_down, GuestAddress& out_address,
                            std::optional<std::uint32_t> requested_page_size) {
  std::lock_guard lock(mutex_);
  if (!initialized_ || !size) return false;
  const std::uint32_t page_size = requested_page_size.value_or(kBasePageSize);
  const RegionDescriptor* region = nullptr;
  for (const auto& candidate : kRegions) {
    if (candidate.kind == RegionKind::Virtual && candidate.allocation_page_size == page_size) {
      region = &candidate;
      break;
    }
  }
  if (!region) return false;
  alignment = std::max(alignment ? alignment : page_size, page_size);
  if (!std::has_single_bit(alignment)) return false;
  size = align_up(size, page_size);

  if (!top_down) {
    for (std::uint64_t candidate = align_up(region->base, alignment);
         candidate + size - 1u <= region->end; candidate += alignment) {
      const auto first = static_cast<std::uint32_t>(candidate) >> kPageShift;
      const auto count = size >> kPageShift;
      bool free = true;
      for (std::uint32_t i = 0; i < count; ++i) {
        if (pages_[first + i].state != PageState::Free) { free = false; break; }
      }
      if (free) {
        out_address = static_cast<GuestAddress>(candidate);
        return reserve_pages(out_address, size, protect, false) &&
               commit_pages(out_address, size, protect);
      }
    }
  } else {
    const std::uint64_t high_start = std::uint64_t(region->end) + 1u - size;
    for (std::uint64_t candidate = align_down(static_cast<std::uint32_t>(high_start), alignment);;
         candidate -= alignment) {
      if (candidate < region->base) break;
      const auto first = static_cast<std::uint32_t>(candidate) >> kPageShift;
      const auto count = size >> kPageShift;
      bool free = true;
      for (std::uint32_t i = 0; i < count; ++i) {
        if (pages_[first + i].state != PageState::Free) { free = false; break; }
      }
      if (free) {
        out_address = static_cast<GuestAddress>(candidate);
        return reserve_pages(out_address, size, protect, false) &&
               commit_pages(out_address, size, protect);
      }
      if (candidate < region->base + alignment) break;
    }
  }
  return false;
}

bool AddressSpace::decommit(GuestAddress base, std::uint32_t size) {
  std::lock_guard lock(mutex_);
  if (!size) return false;
  const auto* region = region_for(base);
  if (!region || (region->kind != RegionKind::Virtual && region->kind != RegionKind::Xex)) return false;
  const std::uint32_t first = base >> kPageShift;
  const std::uint32_t count = page_count_for(size);
  for (std::uint32_t i = 0; i < count; ++i) {
    if (pages_[first + i].state == PageState::Free) return false;
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    auto& page = pages_[first + i];
    if (page.state == PageState::Committed &&
        page.physical_page != kInvalidPhysicalPage) {
      const auto physical_page = page.physical_page;
      remove_physical_mapping_ref(physical_page, first + i);
      if (region->kind == RegionKind::Xex) {
        const GuestAddress addr = (first + i) << kPageShift;
        const GuestAddress alias = addr < kXex4KBase
                                       ? addr + 0x10000000u
                                       : addr - 0x10000000u;
        const auto alias_page_index = alias >> kPageShift;
        auto& alias_page = pages_[alias_page_index];
        remove_physical_mapping_ref(physical_page, alias_page_index);
        alias_page.physical_page = kInvalidPhysicalPage;
        alias_page.state = PageState::Reserved;
      }
      if (!page.explicit_physical_mapping) {
        free_physical_page(physical_page);
      }
      page.physical_page = kInvalidPhysicalPage;
      page.explicit_physical_mapping = false;
    }
    page.state = PageState::Reserved;
  }
  publish_hot_range(base, size);
  if (region->kind == RegionKind::Xex) {
    const auto alias_base = base < kXex4KBase ? base + 0x10000000u
                                               : base - 0x10000000u;
    publish_hot_range(alias_base, size);
  }
  return true;
}

bool AddressSpace::release(GuestAddress allocation_base) {
  std::lock_guard lock(mutex_);
  const auto page_index = allocation_base >> kPageShift;
  auto page = pages_[page_index];
  if (page.state == PageState::Free || page.allocation_base_page != page_index || !page.allocation_page_count) return false;
  const auto* region = region_for(allocation_base);
  if (!region || (region->kind != RegionKind::Virtual && region->kind != RegionKind::Xex)) return false;
  const auto allocation_page_count = page.allocation_page_count;
  for (std::uint32_t i = 0; i < allocation_page_count; ++i) {
    auto& p = pages_[page_index + i];
    if (p.state == PageState::Committed &&
        p.physical_page != kInvalidPhysicalPage) {
      const auto physical_page = p.physical_page;
      remove_physical_mapping_ref(physical_page, page_index + i);
      if (region->kind == RegionKind::Xex) {
        const GuestAddress addr = (page_index + i) << kPageShift;
        const GuestAddress alias = addr < kXex4KBase
                                       ? addr + 0x10000000u
                                       : addr - 0x10000000u;
        const auto alias_page_index = alias >> kPageShift;
        remove_physical_mapping_ref(physical_page, alias_page_index);
        pages_[alias_page_index] = Page{};
      }
      if (!p.explicit_physical_mapping) {
        free_physical_page(physical_page);
      }
    } else if (region->kind == RegionKind::Xex) {
      const GuestAddress addr = (page_index + i) << kPageShift;
      const GuestAddress alias = addr < kXex4KBase
                                     ? addr + 0x10000000u
                                     : addr - 0x10000000u;
      pages_[alias >> kPageShift] = Page{};
    }
    p = Page{};
  }
  const auto allocation_size = allocation_page_count * kBasePageSize;
  publish_hot_range(allocation_base, allocation_size);
  if (region->kind == RegionKind::Xex) {
    const auto alias_base = allocation_base < kXex4KBase
                                ? allocation_base + 0x10000000u
                                : allocation_base - 0x10000000u;
    publish_hot_range(alias_base, allocation_size);
  }
  return true;
}

bool AddressSpace::protect(GuestAddress base, std::uint32_t size, Protect protect_value,
                           Protect* old_protect) {
  std::lock_guard lock(mutex_);
  if (!size) return false;
  const std::uint32_t first = base >> kPageShift;
  const std::uint32_t last = static_cast<std::uint32_t>((std::uint64_t(base) + size - 1u) >> kPageShift);
  if (last >= pages_.size()) return false;
  for (std::uint32_t i = first; i <= last; ++i) if (pages_[i].state != PageState::Committed) return false;
  if (old_protect) *old_protect = pages_[first].current_protect;
  for (std::uint32_t i = first; i <= last; ++i) {
    pages_[i].current_protect = protect_value;
    if (pages_[i].kind == RegionKind::Xex) {
      GuestAddress addr = i << kPageShift;
      GuestAddress alias = addr < kXex4KBase ? addr + 0x10000000u : addr - 0x10000000u;
      pages_[alias >> kPageShift].current_protect = protect_value;
    }
    publish_hot_page(i);
    if (pages_[i].kind == RegionKind::Xex) {
      const GuestAddress addr = i << kPageShift;
      const GuestAddress alias = addr < kXex4KBase ? addr + 0x10000000u : addr - 0x10000000u;
      publish_hot_page(alias >> kPageShift);
    }
  }
  return true;
}

std::optional<MappingInfo> AddressSpace::query(GuestAddress address) const {
  std::lock_guard lock(mutex_);
  const auto* region = region_for(address);
  if (!region) return std::nullopt;
  if (region->kind == RegionKind::PhysicalAlias || region->kind == RegionKind::GpuWriteback) {
    return MappingInfo{address, physical_alias_address(address), region->base,
                       region->end - region->base + 1u,
                       region->end - address + 1u, region->allocation_page_size,
                       PageState::Committed, kReadWrite, kReadWrite, region->kind};
  }
  if (region->kind == RegionKind::Mmio) {
    return MappingInfo{address, 0xFFFFFFFFu, region->base, region->end - region->base + 1u,
                       region->end - address + 1u, region->allocation_page_size,
                       PageState::Committed, kReadWrite, kReadWrite, region->kind};
  }
  const auto& page = pages_[address >> kPageShift];
  MappingInfo info{};
  info.guest_address = address;
  info.physical_address = page.state == PageState::Committed
                              ? page.physical_page * kBasePageSize + (address & (kBasePageSize - 1u))
                              : 0xFFFFFFFFu;
  info.allocation_base = page.allocation_base_page << kPageShift;
  info.allocation_size = page.allocation_page_count * kBasePageSize;
  info.page_size = region->allocation_page_size;
  info.state = page.state;
  info.allocation_protect = page.allocation_protect;
  info.current_protect = page.current_protect;
  info.kind = region->kind;
  if (page.state != PageState::Free) {
    std::uint32_t run = 0;
    const auto start = address >> kPageShift;
    for (std::uint32_t i = start; i < pages_.size(); ++i) {
      const auto& p = pages_[i];
      if (p.state != page.state || p.current_protect != page.current_protect || p.kind != page.kind) break;
      ++run;
    }
    info.region_size = run * kBasePageSize;
  }
  return info;
}

std::uint32_t AddressSpace::allocate_physical_page(bool top_down) {
  reclaim_retired_physical_pages();
  std::uint32_t page = kInvalidPhysicalPage;
  if (!physical_allocator_->allocate(1u, 1u, top_down, page)) {
    return kInvalidPhysicalPage;
  }
  physical_page_used_[page] = kPhysicalAnonymous;
  return page;
}

void AddressSpace::free_physical_page(std::uint32_t page) {
  if (page >= kPhysicalPageCount) return;
  // Dedicated GPU writeback pages stay reserved. Explicit physical allocations
  // are released only by free_physical().
  if (page < kHugePageSize / kBasePageSize) return;
  auto& ownership = physical_page_used_[page];
  if (ownership != kPhysicalAnonymous &&
      ownership != kPhysicalAnonymousPendingFree) {
    return;
  }
  if (physical_mapping_refs_[page] != 0u) {
    ownership = kPhysicalAnonymousPendingFree;
    return;
  }
  // Do not immediately recycle the backing page. A generated fast access may
  // have loaded the old hot translation immediately before it was unpublished.
  // Retired pages become allocator-visible only after all pre-existing
  // MemoryAccessContext read-side guards have drained.
  ownership = kPhysicalRetired;
  if (!physical_allocator_->retire(page, 1u)) {
    throw std::logic_error("physical retired-range overlap");
  }
}

void AddressSpace::reclaim_retired_physical_pages() {
  if (active_fast_readers_.load(std::memory_order_acquire) != 0u) return;

  auto retired = physical_allocator_->take_retired_ranges();
  for (const auto& [first, count] : retired) {
    for (std::uint32_t i = 0; i < count; ++i) {
      if (physical_page_used_[first + i] != kPhysicalRetired ||
          physical_mapping_refs_[first + i] != 0u) {
        throw std::logic_error("retired physical range ownership mismatch");
      }
    }
    std::fill_n(physical_page_used_.begin() + first, count, kPhysicalFree);
    const auto physical_address = first * kBasePageSize;
    const auto physical_size = count * kBasePageSize;
    const bool reservation_participant =
        begin_physical_write(physical_address, physical_size);
    physical_->discard_range(first, count);
    complete_physical_write(physical_address, physical_size,
                            reservation_participant);
    if (!physical_allocator_->release(first, count)) {
      throw std::logic_error("physical free-range allocator ownership mismatch");
    }
  }
}

void AddressSpace::add_physical_mapping_ref(std::uint32_t physical_page,
                                            std::uint32_t guest_page) {
  if (physical_page >= kPhysicalPageCount || guest_page >= kPageCount) {
    throw std::logic_error("physical mapping reference out of range");
  }
  if (!physical_reverse_mappings_->add(physical_page, guest_page)) {
    throw std::logic_error("duplicate physical reverse mapping");
  }
  ++physical_mapping_refs_[physical_page];
}

void AddressSpace::remove_physical_mapping_ref(std::uint32_t physical_page,
                                               std::uint32_t guest_page) {
  if (physical_page >= kPhysicalPageCount || guest_page >= kPageCount ||
      physical_mapping_refs_[physical_page] == 0u) {
    throw std::logic_error("invalid physical mapping reference removal");
  }
  if (!physical_reverse_mappings_->remove(physical_page, guest_page)) {
    throw std::logic_error("missing physical reverse mapping");
  }
  --physical_mapping_refs_[physical_page];
  if (physical_mapping_refs_[physical_page] == 0u &&
      physical_page_used_[physical_page] == kPhysicalAnonymousPendingFree) {
    free_physical_page(physical_page);
  }
}

bool AddressSpace::reserve_physical_run(std::uint32_t count,
                                        std::uint32_t alignment_pages,
                                        bool top_down,
                                        std::uint32_t& out_first_page) {
  if (!count || count > kPhysicalPageCount || !alignment_pages ||
      !std::has_single_bit(alignment_pages)) {
    return false;
  }
  reclaim_retired_physical_pages();
  if (!physical_allocator_->allocate(count, alignment_pages, top_down,
                                     out_first_page)) {
    return false;
  }
  std::fill_n(physical_page_used_.begin() + out_first_page, count,
              kPhysicalExplicit);
  return true;
}

bool AddressSpace::allocate_physical(std::uint32_t size, std::uint32_t alignment,
                                     bool top_down, std::uint32_t& out_physical_address) {
  std::lock_guard lock(mutex_);
  if (!initialized_ || !size) return false;
  alignment = std::max(alignment ? alignment : kBasePageSize, kBasePageSize);
  if (!std::has_single_bit(alignment)) return false;
  const auto count = page_count_for(size);
  const auto alignment_pages = alignment / kBasePageSize;
  std::uint32_t first = 0;
  if (!reserve_physical_run(count, alignment_pages, top_down, first)) return false;
  out_physical_address = first * kBasePageSize;
  const auto physical_size = count * kBasePageSize;
  const bool reservation_participant =
      begin_physical_write(out_physical_address, physical_size);
  std::memset(physical_->data() + out_physical_address, 0,
              std::size_t(count) * kBasePageSize);
  complete_physical_write(out_physical_address, physical_size,
                          reservation_participant);
  return true;
}

bool AddressSpace::free_physical(std::uint32_t physical_base, std::uint32_t size) {
  std::lock_guard lock(mutex_);
  if (!size || (physical_base & (kBasePageSize - 1u)) ||
      std::uint64_t(physical_base) + size > kPhysicalMemorySize) return false;
  const auto first = physical_base / kBasePageSize;
  const auto count = page_count_for(size);
  if (first + count > kPhysicalPageCount) return false;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (physical_page_used_[first + i] != kPhysicalExplicit ||
        physical_mapping_refs_[first + i] != 0u) {
      return false;
    }
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    physical_page_used_[first + i] = kPhysicalRetired;
  }
  if (!physical_allocator_->retire(first, count)) {
    throw std::logic_error("physical retired-range overlap");
  }
  return true;
}

bool AddressSpace::map_virtual_to_physical(GuestAddress virtual_base,
                                           std::uint32_t physical_base,
                                           std::uint32_t size, Protect protect_value) {
  std::lock_guard lock(mutex_);
  if (!size || (virtual_base & (kBasePageSize - 1u)) ||
      (physical_base & (kBasePageSize - 1u)) || (size & (kBasePageSize - 1u))) return false;
  if (std::uint64_t(physical_base) + size > kPhysicalMemorySize) return false;
  const auto* region = region_for(virtual_base);
  if (!region || region->kind != RegionKind::Virtual) return false;
  if (std::uint64_t(virtual_base) + size - 1u > region->end) return false;
  const auto first = virtual_base >> kPageShift;
  const auto count = size >> kPageShift;
  for (std::uint32_t i = 0; i < count; ++i) if (pages_[first + i].state != PageState::Free) return false;
  reclaim_retired_physical_pages();
  const auto physical_first = physical_base / kBasePageSize;
  for (std::uint32_t i = 0; i < count;) {
    if (physical_page_used_[physical_first + i] == kPhysicalRetired) {
      return false;
    }
    if (physical_page_used_[physical_first + i] != kPhysicalFree) {
      ++i;
      continue;
    }
    const auto run_first = i;
    while (i < count &&
           physical_page_used_[physical_first + i] == kPhysicalFree) {
      ++i;
    }
    if (!physical_allocator_->contains_free(physical_first + run_first,
                                            i - run_first)) {
      throw std::logic_error("physical allocator/free-state mismatch");
    }
  }

  // Mapping previously unowned physical RAM establishes explicit physical
  // ownership. Remove those pages from the free-range index before publishing
  // the guest mapping so the allocator can never hand the same frame out.
  for (std::uint32_t i = 0; i < count;) {
    if (physical_page_used_[physical_first + i] != kPhysicalFree) {
      ++i;
      continue;
    }
    const auto run_first = i;
    while (i < count &&
           physical_page_used_[physical_first + i] == kPhysicalFree) {
      ++i;
    }
    const auto run_count = i - run_first;
    if (!physical_allocator_->claim(physical_first + run_first, run_count)) {
      throw std::logic_error("failed to claim free physical range");
    }
    std::fill_n(physical_page_used_.begin() + physical_first + run_first,
                run_count, kPhysicalExplicit);
  }

  for (std::uint32_t i = 0; i < count; ++i) {
    auto& p = pages_[first + i];
    p.physical_page = physical_first + i;
    p.allocation_base_page = first;
    p.allocation_page_count = count;
    p.allocation_protect = protect_value;
    p.current_protect = protect_value;
    p.state = PageState::Committed;
    p.kind = RegionKind::Virtual;
    p.explicit_physical_mapping = true;
    add_physical_mapping_ref(p.physical_page, first + i);
  }
  publish_hot_range(virtual_base, size);
  return true;
}

std::uint32_t AddressSpace::physical_alias_address(GuestAddress address) const {
  if (address >= kGpuWritebackBase && address <= kGpuWritebackEnd)
    return address - kGpuWritebackBase;
  if (address >= kPhysical64KBase && address <= kPhysical16MEnd)
    return address & 0x1FFFFFFFu;
  if (address >= kPhysical4KBase && address <= kPhysical4KHeapEnd)
    return (address - kPhysical4KBase) + kPhysical4KViewOffset;
  return 0xFFFFFFFFu;
}

std::uint32_t AddressSpace::get_physical_address(GuestAddress address) const {
  std::lock_guard lock(mutex_);
  const auto direct = physical_alias_address(address);
  if (direct != 0xFFFFFFFFu && direct < kPhysicalMemorySize) return direct;
  const auto* region = region_for(address);
  if (!region || (region->kind != RegionKind::Virtual && region->kind != RegionKind::Xex)) return 0xFFFFFFFFu;
  const auto& page = pages_[address >> kPageShift];
  if (page.state != PageState::Committed || page.physical_page == kInvalidPhysicalPage) return 0xFFFFFFFFu;
  return page.physical_page * kBasePageSize + (address & (kBasePageSize - 1u));
}

std::uint32_t AddressSpace::fetch32_be(GuestAddress address) {
  std::lock_guard lock(mutex_);
  if (find_mmio(address, 4)) {
    fault(address, 4, AccessKind::Execute, FaultReason::Protection,
          "instructions may not be fetched from MMIO");
  }
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    const auto b = std::to_integer<std::uint8_t>(
        *resolve_byte(address + static_cast<GuestAddress>(i), AccessKind::Execute).ptr);
    value = (value << 8u) | b;
  }
  return value;
}

const std::byte* AddressSpace::physical_data(std::uint32_t physical_address) const {
  if (!initialized_ || physical_address >= kPhysicalMemorySize) return nullptr;
  return physical_->data() + physical_address;
}

bool AddressSpace::copy_physical_range(
    std::uint32_t physical_address, std::span<std::byte> destination) const {
  std::lock_guard lock(mutex_);
  if (!initialized_ || std::uint64_t(physical_address) + destination.size() >
                           kPhysicalMemorySize) {
    return false;
  }
  xenon::cpu::detail::atomic_copy_from_guest(
      physical_->data() + physical_address, destination);
  return true;
}

bool AddressSpace::write_physical(std::uint32_t physical_address,
                                  std::span<const std::byte> source) {
  if (source.empty()) return true;
  auto write = physical_write_span(
      physical_address, static_cast<std::uint32_t>(source.size()));
  if (!write) return false;
  return write.write(0u, source);
}

bool AddressSpace::fill_physical(std::uint32_t physical_address,
                                 std::uint32_t size, std::byte value) {
  if (!size) return true;
  auto write = physical_write_span(physical_address, size);
  if (!write) return false;
  return write.fill(0u, size, value);
}

std::vector<GuestAddress> AddressSpace::dynamic_guest_aliases_for_physical(
    std::uint32_t physical_address) const {
  std::lock_guard lock(mutex_);
  std::vector<GuestAddress> aliases;
  if (!initialized_ || physical_address >= kPhysicalMemorySize) return aliases;
  const auto physical_page = physical_address >> kPageShift;
  const auto page_offset = physical_address & (kBasePageSize - 1u);
  const auto guest_pages = physical_reverse_mappings_->guests(physical_page);
  aliases.reserve(guest_pages.size());
  for (const auto guest_page : guest_pages) {
    aliases.push_back((guest_page << kPageShift) | page_offset);
  }
  std::sort(aliases.begin(), aliases.end());
  return aliases;
}

bool AddressSpace::validate_invariants(std::string* error) const {
  std::lock_guard lock(mutex_);
  return validate_invariants_locked(error);
}

bool AddressSpace::validate_invariants_locked(std::string* error) const {
  const auto fail = [&](std::string message) {
    if (error) *error = std::move(message);
    return false;
  };
  if (!initialized_) return fail("address space is not initialized");

  for (std::uint32_t physical_page = 0; physical_page < kPhysicalPageCount;
       ++physical_page) {
    const auto reverse_count =
        physical_reverse_mappings_->count(physical_page);
    if (physical_mapping_refs_[physical_page] != reverse_count) {
      return fail("physical mapping refcount/reverse-map mismatch at page " +
                  std::to_string(physical_page));
    }

    const auto ownership = physical_page_used_[physical_page];
    const bool allocator_free =
        physical_allocator_->contains_free(physical_page, 1u);
    const bool allocator_retired =
        physical_allocator_->contains_retired(physical_page, 1u);
    if (ownership == kPhysicalFree) {
      if (physical_mapping_refs_[physical_page] != 0u || !allocator_free ||
          allocator_retired) {
        return fail("free physical page has live ownership metadata at page " +
                    std::to_string(physical_page));
      }
    } else {
      if (allocator_free) {
        return fail("owned physical page appears in free allocator at page " +
                    std::to_string(physical_page));
      }
      if ((ownership == kPhysicalRetired) != allocator_retired) {
        return fail("retired physical state/range mismatch at page " +
                    std::to_string(physical_page));
      }
    }

    if (ownership == kPhysicalRetired &&
        physical_mapping_refs_[physical_page] != 0u) {
      return fail("retired physical page still has guest mappings at page " +
                  std::to_string(physical_page));
    }
    if (ownership == kPhysicalAnonymousPendingFree &&
        physical_mapping_refs_[physical_page] == 0u) {
      return fail("pending-free physical page has no remaining alias at page " +
                  std::to_string(physical_page));
    }
    if (ownership == kPhysicalAnonymous &&
        physical_mapping_refs_[physical_page] == 0u) {
      return fail("anonymous physical page has no owning guest mapping at page " +
                  std::to_string(physical_page));
    }
  }

  for (const auto& [physical_page, guest_pages] :
       physical_reverse_mappings_->entries()) {
    if (physical_page >= kPhysicalPageCount) {
      return fail("reverse map contains out-of-range physical page");
    }
    for (std::size_t i = 0; i < guest_pages.size(); ++i) {
      const auto guest_page = guest_pages[i];
      if (guest_page >= kPageCount) {
        return fail("reverse map contains out-of-range guest page");
      }
      for (std::size_t j = i + 1; j < guest_pages.size(); ++j) {
        if (guest_pages[j] == guest_page) {
          return fail("reverse map contains duplicate guest page");
        }
      }
      const auto& page = pages_[guest_page];
      if (page.state != PageState::Committed ||
          page.physical_page != physical_page) {
        return fail("reverse map points at stale guest mapping page " +
                    std::to_string(guest_page));
      }
    }
  }

  for (std::uint32_t guest_page = 0; guest_page < kPageCount; ++guest_page) {
    const auto& page = pages_[guest_page];
    if (page.state == PageState::Committed) {
      if (page.physical_page == kInvalidPhysicalPage ||
          page.physical_page >= kPhysicalPageCount) {
        return fail("committed guest page has invalid physical backing at page " +
                    std::to_string(guest_page));
      }
      if ((page.kind == RegionKind::Virtual || page.kind == RegionKind::Xex) &&
          !physical_reverse_mappings_->contains(page.physical_page,
                                                guest_page)) {
        return fail("committed guest page missing reverse mapping at page " +
                    std::to_string(guest_page));
      }
    } else if (page.physical_page != kInvalidPhysicalPage) {
      return fail("non-committed guest page retains physical backing at page " +
                  std::to_string(guest_page));
    }

    if (page.explicit_physical_mapping && page.state != PageState::Committed) {
      return fail("non-committed guest page retains explicit-mapping state at page " +
                  std::to_string(guest_page));
    }

    if (page.kind == RegionKind::Xex && page.state != PageState::Free) {
      const auto address = guest_page << kPageShift;
      const auto alias_address =
          address < kXex4KBase ? address + 0x10000000u
                               : address - 0x10000000u;
      const auto alias_page_index = alias_address >> kPageShift;
      if (alias_page_index >= kPageCount) {
        return fail("XEX alias resolves outside guest page table");
      }
      const auto& alias = pages_[alias_page_index];
      if (alias.kind != RegionKind::Xex || alias.state != page.state ||
          alias.allocation_page_count != page.allocation_page_count ||
          alias.allocation_protect != page.allocation_protect ||
          alias.current_protect != page.current_protect ||
          alias.physical_page != page.physical_page) {
        return fail("XEX alias metadata mismatch at page " +
                    std::to_string(guest_page));
      }
    }
  }

  if (error) error->clear();
  return true;
}

PhysicalWriteSpan AddressSpace::physical_write_span(
    std::uint32_t physical_address, std::uint32_t size) noexcept {
  if (!initialized_ || !size || physical_address >= kPhysicalMemorySize ||
      std::uint64_t{physical_address} + size > kPhysicalMemorySize) {
    return {};
  }
  const bool reservation_participant =
      begin_physical_write(physical_address, size);
  return PhysicalWriteSpan(
      this, physical_address,
      std::span<std::byte>(physical_->data() + physical_address, size),
      reservation_participant);
}

AddressSpace::ResolvedByte AddressSpace::resolve_byte(GuestAddress address, AccessKind access) {
  const auto direct = physical_alias_address(address);
  if (direct != 0xFFFFFFFFu) {
    if (direct >= kPhysicalMemorySize) fault(address, 1, access, FaultReason::OutOfRange, "physical alias outside RAM");
    return {physical_->data() + direct, direct, true};
  }
  const auto* region = region_for(address);
  if (!region) fault(address, 1, access, FaultReason::Unmapped, "guest address has no region");
  if (region->kind == RegionKind::Mmio) fault(address, 1, access, FaultReason::Unmapped, "MMIO range has no handler");
  const auto& page = pages_[address >> kPageShift];
  if (page.state == PageState::Free) fault(address, 1, access, FaultReason::Unmapped, "guest page is free");
  if (page.state != PageState::Committed || page.physical_page == kInvalidPhysicalPage)
    fault(address, 1, access, FaultReason::Uncommitted, "guest page is reserved but uncommitted");
  const bool allowed = access == AccessKind::Read ? has(page.current_protect, Protect::Read)
                     : access == AccessKind::Write ? has(page.current_protect, Protect::Write)
                                                   : has(page.current_protect, Protect::Execute);
  if (!allowed) fault(address, 1, access, FaultReason::Protection, "guest page protection violation");
  const auto physical_address = page.physical_page * kBasePageSize + (address & (kBasePageSize - 1u));
  return {physical_->data() + physical_address, physical_address, true};
}

std::optional<AddressSpace::ResolvedByte> AddressSpace::resolve_contiguous(
    GuestAddress address, std::size_t width, AccessKind access) {
  if (!width) return std::nullopt;
  const std::uint64_t last_address64 = std::uint64_t(address) + width - 1u;
  if (last_address64 > 0xFFFFFFFFull) return std::nullopt;
  const auto last_address = static_cast<GuestAddress>(last_address64);

  const auto direct = physical_alias_address(address);
  if (direct != 0xFFFFFFFFu) {
    const auto direct_last = physical_alias_address(last_address);
    if (direct_last == direct + width - 1u && direct_last < kPhysicalMemorySize) {
      return ResolvedByte{physical_->data() + direct, direct, true};
    }
    return std::nullopt;
  }

  if ((address >> kPageShift) != (last_address >> kPageShift)) return std::nullopt;
  const auto* region = region_for(address);
  if (!region || region->kind == RegionKind::Mmio || region->kind == RegionKind::PhysicalAlias ||
      region->kind == RegionKind::GpuWriteback) return std::nullopt;
  const auto& page = pages_[address >> kPageShift];
  if (page.state == PageState::Free) fault(address, width, access, FaultReason::Unmapped, "guest page is free");
  if (page.state != PageState::Committed || page.physical_page == kInvalidPhysicalPage)
    fault(address, width, access, FaultReason::Uncommitted, "guest page is reserved but uncommitted");
  const bool allowed = access == AccessKind::Read ? has(page.current_protect, Protect::Read)
                     : access == AccessKind::Write ? has(page.current_protect, Protect::Write)
                                                   : has(page.current_protect, Protect::Execute);
  if (!allowed) fault(address, width, access, FaultReason::Protection, "guest page protection violation");
  const auto physical_address = page.physical_page * kBasePageSize + (address & (kBasePageSize - 1u));
  return ResolvedByte{physical_->data() + physical_address, physical_address, true};
}

AddressSpace::ConstResolvedByte AddressSpace::resolve_byte_const(GuestAddress address, AccessKind access) const {
  const auto r = const_cast<AddressSpace*>(this)->resolve_byte(address, access);
  return {r.ptr, r.physical_address, r.physical};
}

const AddressSpace::MmioRange* AddressSpace::find_mmio(GuestAddress address, std::uint32_t width) const {
  for (const auto& range : mmio_ranges_) {
    if (address >= range.base && std::uint64_t(address) + width <= std::uint64_t(range.base) + range.size)
      return &range;
  }
  return nullptr;
}
AddressSpace::MmioRange* AddressSpace::find_mmio(GuestAddress address, std::uint32_t width) {
  return const_cast<MmioRange*>(std::as_const(*this).find_mmio(address, width));
}

std::uint64_t AddressSpace::read_mmio(GuestAddress address, std::uint32_t width) const {
  const auto* range = find_mmio(address, width);
  if (!range || !range->read) fault(address, width, AccessKind::Read, FaultReason::Unmapped, "MMIO read has no handler");
  return range->read(address, width);
}
void AddressSpace::write_mmio(GuestAddress address, std::uint32_t width, std::uint64_t value) {
  auto* range = find_mmio(address, width);
  if (!range || !range->write) fault(address, width, AccessKind::Write, FaultReason::Unmapped, "MMIO write has no handler");
  range->write(address, width, value);
}

bool AddressSpace::add_mmio_range(GuestAddress base, std::uint32_t size, MmioRead read,
                                  MmioWrite write, std::string name) {
  std::lock_guard lock(mutex_);
  if (!size) return false;
  for (const auto& range : mmio_ranges_) if (overlaps(base, size, range.base, range.size)) return false;
  mmio_ranges_.push_back({base, size, std::move(read), std::move(write), std::move(name)});
  publish_hot_range(base, size);
  return true;
}
void AddressSpace::clear_mmio_ranges() {
  std::lock_guard lock(mutex_);
  mmio_ranges_.clear();
  rebuild_hot_pages();
}

std::uint64_t AddressSpace::add_invalidation_callback(InvalidationCallback callback) {
  std::lock_guard lock(mutex_);
  const auto id = next_invalidation_callback_id_++;
  invalidation_callbacks_.push_back({id, std::move(callback)});
  return id;
}
void AddressSpace::remove_invalidation_callback(std::uint64_t id) {
  std::lock_guard lock(mutex_);
  std::erase_if(invalidation_callbacks_, [id](const auto& pair) { return pair.first == id; });
}

template <typename T>
T AddressSpace::read_integer(GuestAddress address, bool little_endian) {
  // Direct MemoryPort callers get the same lock-free common-RAM path as
  // generated PPC. Only exceptional/unaligned accesses fall through to the
  // cold management-protected resolver below.
  {
    auto access = access_context();
    xenon::cpu::MemoryAccessContext::PhysicalResolution resolved{};
    if (access.resolve_physical_ram(address, sizeof(T), false, alignof(T),
                                    resolved)) {
      if constexpr (sizeof(T) == 2u) {
        return little_endian ? static_cast<T>(access.read16_le(address))
                             : static_cast<T>(access.read16_be(address));
      } else if constexpr (sizeof(T) == 4u) {
        return little_endian ? static_cast<T>(access.read32_le(address))
                             : static_cast<T>(access.read32_be(address));
      } else {
        return little_endian ? static_cast<T>(access.read64_le(address))
                             : static_cast<T>(access.read64_be(address));
      }
    }
  }

  std::lock_guard lock(mutex_);
  if (find_mmio(address, sizeof(T))) {
    T v = static_cast<T>(read_mmio(address, sizeof(T)));
    return little_endian ? byteswap_if(v, true) : v;
  }
  if (auto contiguous = resolve_contiguous(address, sizeof(T), AccessKind::Read);
      contiguous &&
      (reinterpret_cast<std::uintptr_t>(contiguous->ptr) & (alignof(T) - 1u)) == 0u) {
    const auto raw = xenon::cpu::detail::atomic_load_relaxed<T>(contiguous->ptr);
    const bool host_little = std::endian::native == std::endian::little;
    const bool swap = little_endian ? !host_little : host_little;
    return byteswap_if(raw, swap);
  }
  T value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    const auto resolved =
        resolve_byte(address + static_cast<GuestAddress>(i), AccessKind::Read);
    const auto b = xenon::cpu::detail::atomic_load_relaxed<std::uint8_t>(resolved.ptr);
    if (little_endian) value |= static_cast<T>(b) << (i * 8u);
    else value = static_cast<T>((value << 8u) | b);
  }
  return value;
}

template <typename T>
void AddressSpace::write_integer(GuestAddress address, T value, bool little_endian) {
  {
    auto access = access_context();
    xenon::cpu::MemoryAccessContext::PhysicalResolution resolved{};
    if (access.resolve_physical_ram(address, sizeof(T), true, alignof(T),
                                    resolved)) {
      if constexpr (sizeof(T) == 2u) {
        if (little_endian) access.write16_le(address, static_cast<std::uint16_t>(value));
        else access.write16_be(address, static_cast<std::uint16_t>(value));
      } else if constexpr (sizeof(T) == 4u) {
        if (little_endian) access.write32_le(address, static_cast<std::uint32_t>(value));
        else access.write32_be(address, static_cast<std::uint32_t>(value));
      } else {
        if (little_endian) access.write64_le(address, static_cast<std::uint64_t>(value));
        else access.write64_be(address, static_cast<std::uint64_t>(value));
      }
      return;
    }
  }

  std::lock_guard lock(mutex_);
  if (find_mmio(address, sizeof(T))) {
    write_mmio(address, sizeof(T), little_endian ? byteswap_if(value, true) : value);
    return;
  }
  if (auto contiguous = resolve_contiguous(address, sizeof(T), AccessKind::Write);
      contiguous &&
      (reinterpret_cast<std::uintptr_t>(contiguous->ptr) & (alignof(T) - 1u)) == 0u) {
    const bool host_little = std::endian::native == std::endian::little;
    const bool swap = little_endian ? !host_little : host_little;
    const T raw = byteswap_if(value, swap);
    const bool reservation_participant =
        begin_physical_write(contiguous->physical_address, sizeof(T));
    xenon::cpu::detail::atomic_store_relaxed<T>(contiguous->ptr, raw);
    complete_physical_write(contiguous->physical_address, sizeof(T),
                            reservation_participant);
    return;
  }
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    const unsigned shift = little_endian ? static_cast<unsigned>(i * 8u)
                                         : static_cast<unsigned>((sizeof(T) - 1u - i) * 8u);
    auto resolved = resolve_byte(address + static_cast<GuestAddress>(i), AccessKind::Write);
    const bool reservation_participant =
        begin_physical_write(resolved.physical_address, 1u);
    xenon::cpu::detail::atomic_store_relaxed<std::uint8_t>(
        resolved.ptr, static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    complete_physical_write(resolved.physical_address, 1u,
                            reservation_participant);
  }
}

std::uint8_t AddressSpace::read8(GuestAddress address) {
  {
    auto access = access_context();
    xenon::cpu::MemoryAccessContext::PhysicalResolution resolved{};
    if (access.resolve_physical_ram(address, 1u, false, 1u, resolved)) {
      return xenon::cpu::detail::atomic_load_relaxed<std::uint8_t>(resolved.ptr);
    }
  }
  std::lock_guard lock(mutex_);
  if (find_mmio(address, 1)) {
    return static_cast<std::uint8_t>(read_mmio(address, 1));
  }
  const auto resolved = resolve_byte(address, AccessKind::Read);
  return xenon::cpu::detail::atomic_load_relaxed<std::uint8_t>(resolved.ptr);
}
std::uint16_t AddressSpace::read16_be(GuestAddress a) { return read_integer<std::uint16_t>(a, false); }
std::uint32_t AddressSpace::read32_be(GuestAddress a) { return read_integer<std::uint32_t>(a, false); }
std::uint64_t AddressSpace::read64_be(GuestAddress a) { return read_integer<std::uint64_t>(a, false); }
xenon::cpu::Vector128 AddressSpace::read128(GuestAddress address) {
  {
    auto access = access_context();
    xenon::cpu::MemoryAccessContext::PhysicalResolution resolved{};
    if (access.resolve_physical_ram(address, 16u, false,
                                    alignof(std::uint64_t), resolved)) {
      return access.read128(address);
    }
  }

  std::lock_guard lock(mutex_);
  xenon::cpu::Vector128 v{};
  if (auto contiguous = resolve_contiguous(address, v.bytes.size(), AccessKind::Read);
      contiguous &&
      (reinterpret_cast<std::uintptr_t>(contiguous->ptr) &
       (alignof(std::uint64_t) - 1u)) == 0u) {
    const auto lo = xenon::cpu::detail::atomic_load_relaxed<std::uint64_t>(
        contiguous->ptr);
    const auto hi = xenon::cpu::detail::atomic_load_relaxed<std::uint64_t>(
        contiguous->ptr + 8u);
    std::memcpy(v.bytes.data(), &lo, sizeof(lo));
    std::memcpy(v.bytes.data() + 8u, &hi, sizeof(hi));
    return v;
  }
  for (std::size_t i = 0; i < v.bytes.size(); ++i) {
    const auto resolved = resolve_byte(
        address + static_cast<GuestAddress>(i), AccessKind::Read);
    v.bytes[i] =
        xenon::cpu::detail::atomic_load_relaxed<std::uint8_t>(resolved.ptr);
  }
  return v;
}

void AddressSpace::write8(GuestAddress address, std::uint8_t value) {
  {
    auto access = access_context();
    xenon::cpu::MemoryAccessContext::PhysicalResolution resolved{};
    if (access.resolve_physical_ram(address, 1u, true, 1u, resolved)) {
      access.write8(address, value);
      return;
    }
  }

  std::lock_guard lock(mutex_);
  if (find_mmio(address, 1)) {
    write_mmio(address, 1, value);
    return;
  }
  auto resolved = resolve_byte(address, AccessKind::Write);
  const bool reservation_participant =
      begin_physical_write(resolved.physical_address, 1u);
  xenon::cpu::detail::atomic_store_relaxed<std::uint8_t>(resolved.ptr, value);
  complete_physical_write(resolved.physical_address, 1u,
                          reservation_participant);
}
void AddressSpace::write16_be(GuestAddress a, std::uint16_t v) { write_integer(a, v, false); }
void AddressSpace::write32_be(GuestAddress a, std::uint32_t v) { write_integer(a, v, false); }
void AddressSpace::write64_be(GuestAddress a, std::uint64_t v) { write_integer(a, v, false); }
void AddressSpace::write128(GuestAddress address, const xenon::cpu::Vector128& value) {
  {
    auto access = access_context();
    xenon::cpu::MemoryAccessContext::PhysicalResolution resolved{};
    if (access.resolve_physical_ram(address, 16u, true,
                                    alignof(std::uint64_t), resolved)) {
      access.write128(address, value);
      return;
    }
  }

  std::lock_guard lock(mutex_);
  if (auto contiguous = resolve_contiguous(address, value.bytes.size(), AccessKind::Write);
      contiguous &&
      (reinterpret_cast<std::uintptr_t>(contiguous->ptr) &
       (alignof(std::uint64_t) - 1u)) == 0u) {
    std::uint64_t lo{}, hi{};
    std::memcpy(&lo, value.bytes.data(), sizeof(lo));
    std::memcpy(&hi, value.bytes.data() + 8u, sizeof(hi));
    const auto width = static_cast<std::uint32_t>(value.bytes.size());
    const bool reservation_participant =
        begin_physical_write(contiguous->physical_address, width);
    xenon::cpu::detail::atomic_store_relaxed<std::uint64_t>(
        contiguous->ptr, lo);
    xenon::cpu::detail::atomic_store_relaxed<std::uint64_t>(
        contiguous->ptr + 8u, hi);
    complete_physical_write(contiguous->physical_address, width,
                            reservation_participant);
    return;
  }
  for (std::size_t i = 0; i < value.bytes.size(); ++i) {
    auto resolved = resolve_byte(
        address + static_cast<GuestAddress>(i), AccessKind::Write);
    const bool reservation_participant =
        begin_physical_write(resolved.physical_address, 1u);
    xenon::cpu::detail::atomic_store_relaxed<std::uint8_t>(
        resolved.ptr, value.bytes[i]);
    complete_physical_write(resolved.physical_address, 1u,
                            reservation_participant);
  }
}

std::uint16_t AddressSpace::read16_le(GuestAddress a) { return read_integer<std::uint16_t>(a, true); }
std::uint32_t AddressSpace::read32_le(GuestAddress a) { return read_integer<std::uint32_t>(a, true); }
std::uint64_t AddressSpace::read64_le(GuestAddress a) { return read_integer<std::uint64_t>(a, true); }
void AddressSpace::write16_le(GuestAddress a, std::uint16_t v) { write_integer(a, v, true); }
void AddressSpace::write32_le(GuestAddress a, std::uint32_t v) { write_integer(a, v, true); }
void AddressSpace::write64_le(GuestAddress a, std::uint64_t v) { write_integer(a, v, true); }

void AddressSpace::read_bytes(GuestAddress address,
                              std::span<std::byte> destination) {
  if (destination.empty()) return;
  {
    auto access = access_context();
    bool all_fast = destination.size() <=
                    std::numeric_limits<GuestAddress>::max() -
                        std::uint64_t{address} + 1u;
    auto cursor = address;
    std::size_t remaining = destination.size();
    while (all_fast && remaining) {
      const auto page_remaining =
          kBasePageSize - (cursor & (kBasePageSize - 1u));
      const auto chunk = std::min<std::size_t>(remaining, page_remaining);
      xenon::cpu::MemoryAccessContext::PhysicalResolution resolved{};
      all_fast = access.resolve_physical_ram(cursor, chunk, false, 1u, resolved);
      cursor += static_cast<GuestAddress>(chunk);
      remaining -= chunk;
    }
    if (all_fast) {
      access.read_bytes(address, destination);
      return;
    }
  }

  // Exceptional/unaligned/MMIO range fallback. Device callbacks and mapping
  // metadata remain cold and management-protected. Individual RAM bytes use
  // atomic_ref-backed operations, so overlapping concurrent aligned accesses
  // remain data-race-free.
  std::lock_guard lock(mutex_);
  for (std::size_t i = 0; i < destination.size(); ++i) {
    destination[i] = static_cast<std::byte>(
        read8(address + static_cast<GuestAddress>(i)));
  }
}

void AddressSpace::write_bytes(GuestAddress address,
                               std::span<const std::byte> source) {
  if (source.empty()) return;
  {
    auto access = access_context();
    bool all_fast = source.size() <=
                    std::numeric_limits<GuestAddress>::max() -
                        std::uint64_t{address} + 1u;
    auto cursor = address;
    std::size_t remaining = source.size();
    while (all_fast && remaining) {
      const auto page_remaining =
          kBasePageSize - (cursor & (kBasePageSize - 1u));
      const auto chunk = std::min<std::size_t>(remaining, page_remaining);
      xenon::cpu::MemoryAccessContext::PhysicalResolution resolved{};
      all_fast = access.resolve_physical_ram(cursor, chunk, true, 1u, resolved);
      cursor += static_cast<GuestAddress>(chunk);
      remaining -= chunk;
    }
    if (all_fast) {
      access.write_bytes(address, source);
      return;
    }
  }

  std::lock_guard lock(mutex_);
  for (std::size_t i = 0; i < source.size(); ++i) {
    write8(address + static_cast<GuestAddress>(i),
           std::to_integer<std::uint8_t>(source[i]));
  }
}

void AddressSpace::fill_bytes(GuestAddress address, std::uint32_t size,
                              std::uint8_t value) {
  if (!size) return;
  {
    auto access = access_context();
    bool all_fast = std::uint64_t{address} + size <=
                    std::uint64_t{std::numeric_limits<GuestAddress>::max()} + 1u;
    auto cursor = address;
    std::uint32_t remaining = size;
    while (all_fast && remaining) {
      const auto page_remaining =
          kBasePageSize - (cursor & (kBasePageSize - 1u));
      const auto chunk = std::min(remaining, page_remaining);
      xenon::cpu::MemoryAccessContext::PhysicalResolution resolved{};
      all_fast = access.resolve_physical_ram(cursor, chunk, true, 1u, resolved);
      cursor += chunk;
      remaining -= chunk;
    }
    if (all_fast) {
      access.fill_bytes(address, size, value);
      return;
    }
  }

  std::lock_guard lock(mutex_);
  for (std::uint32_t i = 0; i < size; ++i) write8(address + i, value);
}

bool AddressSpace::begin_physical_write(std::uint32_t physical_address,
                                       std::uint32_t width) noexcept {
  if (!width || physical_address >= kPhysicalMemorySize) return false;
  auto view = make_fast_memory_view();
  return xenon::cpu::reservation_monitor_detail::enter_write(
      view, physical_address, width);
}

void AddressSpace::complete_physical_write(
    std::uint32_t physical_address, std::uint32_t width,
    bool reservation_participant) noexcept {
  if (!width || physical_address >= kPhysicalMemorySize) return;
  auto view = make_fast_memory_view();
  xenon::cpu::reservation_monitor_detail::finish_write(
      view, physical_address, width, reservation_participant);
}

void AddressSpace::begin_reservation_operation() noexcept {
  for (;;) {
    while (reservation_commit_gate_.load(std::memory_order_acquire) != 0u) {
      std::this_thread::yield();
    }
    active_reservation_ops_.fetch_add(1u, std::memory_order_acq_rel);
    if (reservation_commit_gate_.load(std::memory_order_acquire) == 0u) return;
    active_reservation_ops_.fetch_sub(1u, std::memory_order_release);
  }
}

void AddressSpace::end_reservation_operation() noexcept {
  active_reservation_ops_.fetch_sub(1u, std::memory_order_release);
}

std::uint64_t AddressSpace::claim_reservation(
    std::uint32_t physical_address, std::uint32_t width) noexcept {
  if (physical_address >= kPhysicalMemorySize ||
      (width != 4u && width != 8u)) {
    return 0u;
  }

  const auto granule = physical_address / kReservationGranuleSize;
  const auto word = granule >> 6u;
  const auto bit = std::uint64_t{1} << (granule & 63u);
  reservation_seen_bitmap_[word].fetch_or(bit, std::memory_order_release);

  auto generation = reservation_next_generation_.fetch_add(
      1u, std::memory_order_acq_rel);
  if (generation == 0u) {
    generation = reservation_next_generation_.fetch_add(
        1u, std::memory_order_acq_rel);
    if (generation == 0u) generation = 1u;
  }

  const auto descriptor =
      xenon::cpu::reservation_monitor_detail::encode_slot(
          physical_address, width, generation,
          xenon::cpu::reservation_monitor_detail::SlotStatus::Active);
  for (std::uint32_t slot_index = 0; slot_index < kReservationSlotCount;
       ++slot_index) {
    auto expected = std::uint64_t{0};
    if (reservation_slots_[slot_index].compare_exchange_strong(
            expected, descriptor, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      return make_reservation_token(slot_index, generation);
    }
  }

  // Xenon exposes six hardware threads, so production code should never need a
  // seventh simultaneously live reservation. Failing to claim a slot models a
  // conservatively lost reservation rather than stealing another thread's.
  return 0u;
}

void AddressSpace::cancel_reservation(std::uint64_t token) noexcept {
  std::uint32_t slot_index = 0;
  std::uint32_t generation = 0;
  if (!decode_reservation_token(token, slot_index, generation) ||
      slot_index >= kReservationSlotCount) {
    return;
  }
  auto& slot = reservation_slots_[slot_index];
  auto descriptor = slot.load(std::memory_order_acquire);
  for (;;) {
    if (descriptor == 0u ||
        xenon::cpu::reservation_monitor_detail::slot_generation(descriptor) !=
            generation ||
        xenon::cpu::reservation_monitor_detail::slot_status(descriptor) !=
            xenon::cpu::reservation_monitor_detail::SlotStatus::Active) {
      return;
    }
    if (slot.compare_exchange_weak(descriptor, 0u,
                                   std::memory_order_acq_rel,
                                   std::memory_order_acquire)) {
      return;
    }
  }
}

void AddressSpace::invalidate_reservations(
    std::uint32_t physical_address, std::uint32_t width) noexcept {
  auto view = make_fast_memory_view();
  xenon::cpu::reservation_monitor_detail::invalidate_range(
      view, physical_address, width);
}

bool AddressSpace::claim_store_conditional(
    std::uint32_t physical_address, std::uint32_t width, std::uint64_t token,
    std::uint32_t& slot_index,
    std::uint64_t& committing_descriptor) noexcept {
  std::uint32_t generation = 0;
  if (!decode_reservation_token(token, slot_index, generation) ||
      slot_index >= kReservationSlotCount) {
    return false;
  }

  auto& slot = reservation_slots_[slot_index];
  auto descriptor = slot.load(std::memory_order_acquire);
  if (descriptor == 0u ||
      xenon::cpu::reservation_monitor_detail::slot_status(descriptor) !=
          xenon::cpu::reservation_monitor_detail::SlotStatus::Active ||
      xenon::cpu::reservation_monitor_detail::slot_generation(descriptor) !=
          generation ||
      xenon::cpu::reservation_monitor_detail::slot_physical_address(
          descriptor) != physical_address ||
      xenon::cpu::reservation_monitor_detail::slot_width(descriptor) != width) {
    cancel_reservation(token);
    return false;
  }

  committing_descriptor =
      xenon::cpu::reservation_monitor_detail::encode_slot(
          physical_address, width, generation,
          xenon::cpu::reservation_monitor_detail::SlotStatus::Committing);
  return slot.compare_exchange_strong(descriptor, committing_descriptor,
                                      std::memory_order_acq_rel,
                                      std::memory_order_acquire);
}

std::uint64_t AddressSpace::reserve32(GuestAddress address,
                                      std::uint32_t& value) {
  auto access = access_context();
  xenon::cpu::MemoryAccessContext::PhysicalResolution fast_resolved{};
  std::byte* ptr = nullptr;
  std::uint32_t physical_address = 0u;
  if (access.resolve_physical_ram(address, sizeof(value), false, alignof(std::uint32_t),
                                  fast_resolved)) {
    ptr = fast_resolved.ptr;
    physical_address = fast_resolved.physical_address;
  } else {
    std::lock_guard lock(mutex_);
    auto resolved = resolve_contiguous(address, sizeof(value), AccessKind::Read);
    if (!resolved || !resolved->physical) {
      fault(address, sizeof(value), AccessKind::Read, FaultReason::Unmapped,
            "reservation has no physical RAM backing");
    }
    ptr = resolved->ptr;
    physical_address = resolved->physical_address;
  }

  begin_reservation_operation();
  const auto epoch_before =
      coherency_.write_epoch_data()->load(std::memory_order_acquire);
  auto token = claim_reservation(physical_address, sizeof(value));
  value = atomic_load_guest_be<std::uint32_t>(ptr);
  const auto epoch_after =
      coherency_.write_epoch_data()->load(std::memory_order_acquire);
  const auto writers =
      coherency_.active_writers_data()->load(std::memory_order_acquire);
  if (token && (epoch_before != epoch_after || writers != 0u)) {
    cancel_reservation(token);
    token = 0u;
  }
  end_reservation_operation();
  return token;
}

std::uint64_t AddressSpace::reserve64(GuestAddress address,
                                      std::uint64_t& value) {
  auto access = access_context();
  xenon::cpu::MemoryAccessContext::PhysicalResolution fast_resolved{};
  std::byte* ptr = nullptr;
  std::uint32_t physical_address = 0u;
  if (access.resolve_physical_ram(address, sizeof(value), false, alignof(std::uint64_t),
                                  fast_resolved)) {
    ptr = fast_resolved.ptr;
    physical_address = fast_resolved.physical_address;
  } else {
    std::lock_guard lock(mutex_);
    auto resolved = resolve_contiguous(address, sizeof(value), AccessKind::Read);
    if (!resolved || !resolved->physical) {
      fault(address, sizeof(value), AccessKind::Read, FaultReason::Unmapped,
            "reservation has no physical RAM backing");
    }
    ptr = resolved->ptr;
    physical_address = resolved->physical_address;
  }

  begin_reservation_operation();
  const auto epoch_before =
      coherency_.write_epoch_data()->load(std::memory_order_acquire);
  auto token = claim_reservation(physical_address, sizeof(value));
  value = atomic_load_guest_be<std::uint64_t>(ptr);
  const auto epoch_after =
      coherency_.write_epoch_data()->load(std::memory_order_acquire);
  const auto writers =
      coherency_.active_writers_data()->load(std::memory_order_acquire);
  if (token && (epoch_before != epoch_after || writers != 0u)) {
    cancel_reservation(token);
    token = 0u;
  }
  end_reservation_operation();
  return token;
}

bool AddressSpace::store_conditional32(GuestAddress address,
                                       std::uint64_t token,
                                       std::uint32_t value) {
  auto access = access_context();
  xenon::cpu::MemoryAccessContext::PhysicalResolution fast_resolved{};
  std::byte* ptr = nullptr;
  std::uint32_t physical_address = 0u;
  if (access.resolve_physical_ram(address, sizeof(value), true, alignof(std::uint32_t),
                                  fast_resolved)) {
    ptr = fast_resolved.ptr;
    physical_address = fast_resolved.physical_address;
  } else {
    std::lock_guard lock(mutex_);
    auto resolved = resolve_contiguous(address, sizeof(value), AccessKind::Write);
    if (!resolved || !resolved->physical || !token) {
      cancel_reservation(token);
      return false;
    }
    ptr = resolved->ptr;
    physical_address = resolved->physical_address;
  }
  if (!token) return false;

  std::uint32_t expected_gate = 0u;
  while (!reservation_commit_gate_.compare_exchange_weak(
      expected_gate, 1u, std::memory_order_acq_rel,
      std::memory_order_acquire)) {
    expected_gate = 0u;
    std::this_thread::yield();
  }
  while (coherency_.active_writers_data()->load(std::memory_order_acquire) !=
             0u ||
         active_reservation_ops_.load(std::memory_order_acquire) != 0u) {
    std::this_thread::yield();
  }

  std::uint32_t slot_index = 0u;
  std::uint64_t committing = 0u;
  const auto claimed = claim_store_conditional(
      physical_address, sizeof(value), token, slot_index, committing);
  if (!claimed) {
    reservation_commit_gate_.store(0u, std::memory_order_release);
    return false;
  }

  invalidate_reservations(physical_address, sizeof(value));
  atomic_store_guest_be<std::uint32_t>(ptr, value);
  coherency_.mark_write(physical_address, sizeof(value));
  reservation_slots_[slot_index].store(0u, std::memory_order_release);
  reservation_commit_gate_.store(0u, std::memory_order_release);
  return true;
}

bool AddressSpace::store_conditional64(GuestAddress address,
                                       std::uint64_t token,
                                       std::uint64_t value) {
  auto access = access_context();
  xenon::cpu::MemoryAccessContext::PhysicalResolution fast_resolved{};
  std::byte* ptr = nullptr;
  std::uint32_t physical_address = 0u;
  if (access.resolve_physical_ram(address, sizeof(value), true, alignof(std::uint64_t),
                                  fast_resolved)) {
    ptr = fast_resolved.ptr;
    physical_address = fast_resolved.physical_address;
  } else {
    std::lock_guard lock(mutex_);
    auto resolved = resolve_contiguous(address, sizeof(value), AccessKind::Write);
    if (!resolved || !resolved->physical || !token) {
      cancel_reservation(token);
      return false;
    }
    ptr = resolved->ptr;
    physical_address = resolved->physical_address;
  }
  if (!token) return false;

  std::uint32_t expected_gate = 0u;
  while (!reservation_commit_gate_.compare_exchange_weak(
      expected_gate, 1u, std::memory_order_acq_rel,
      std::memory_order_acquire)) {
    expected_gate = 0u;
    std::this_thread::yield();
  }
  while (coherency_.active_writers_data()->load(std::memory_order_acquire) !=
             0u ||
         active_reservation_ops_.load(std::memory_order_acquire) != 0u) {
    std::this_thread::yield();
  }

  std::uint32_t slot_index = 0u;
  std::uint64_t committing = 0u;
  const auto claimed = claim_store_conditional(
      physical_address, sizeof(value), token, slot_index, committing);
  if (!claimed) {
    reservation_commit_gate_.store(0u, std::memory_order_release);
    return false;
  }

  invalidate_reservations(physical_address, sizeof(value));
  atomic_store_guest_be<std::uint64_t>(ptr, value);
  coherency_.mark_write(physical_address, sizeof(value));
  reservation_slots_[slot_index].store(0u, std::memory_order_release);
  reservation_commit_gate_.store(0u, std::memory_order_release);
  return true;
}

std::size_t AddressSpace::reservation_monitor_storage_bytes() const noexcept {
  return reservation_slots_.size() * sizeof(reservation_slots_[0]) +
         reservation_seen_bitmap_.size() * sizeof(reservation_seen_bitmap_[0]) +
         sizeof(reservation_next_generation_) +
         sizeof(reservation_commit_gate_) + sizeof(active_reservation_ops_);
}

std::uint32_t AddressSpace::executable_generation(
    GuestAddress address) const noexcept {
  const auto page_index = address >> kPageShift;
  if (page_index >= hot_pages_.size()) return 0u;
  const auto entry = hot_pages_[page_index].load(std::memory_order_acquire);
  if ((entry & (xenon::cpu::fast_memory::kMapped |
                xenon::cpu::fast_memory::kExecute)) !=
      (xenon::cpu::fast_memory::kMapped |
       xenon::cpu::fast_memory::kExecute)) {
    return 0u;
  }
  const auto physical_page = static_cast<std::uint32_t>(
      entry & xenon::cpu::fast_memory::kPhysicalPageMask);
  return physical_page < executable_page_generations_.size()
             ? executable_page_generations_[physical_page].load(
                   std::memory_order_acquire)
             : 0u;
}

void AddressSpace::barrier(xenon::cpu::BarrierKind kind) {
  xenon::cpu::host_memory_ordering::apply(kind);
}

void AddressSpace::zero_cache_block(GuestAddress address, std::uint32_t bytes) {
  auto access = access_context();
  access.zero_cache_block(address, bytes);
}

void AddressSpace::instruction_cache_invalidate(GuestAddress address) {
  const auto page_index = address >> kPageShift;
  if (page_index < hot_pages_.size()) {
    const auto entry = hot_pages_[page_index].load(std::memory_order_acquire);
    if ((entry & xenon::cpu::fast_memory::kMapped) != 0u) {
      const auto physical_page = static_cast<std::uint32_t>(
          entry & xenon::cpu::fast_memory::kPhysicalPageMask);
      if (physical_page < executable_page_generations_.size()) {
        auto generation = executable_page_generations_[physical_page].load(
            std::memory_order_relaxed);
        while (generation &&
               !executable_page_generations_[physical_page]
                    .compare_exchange_weak(
                        generation,
                        generation == std::numeric_limits<std::uint32_t>::max()
                            ? 1u
                            : generation + 1u,
                        std::memory_order_release,
                        std::memory_order_relaxed)) {}
      }
    }
  }
  std::vector<InvalidationCallback> callbacks;
  {
    std::lock_guard lock(mutex_);
    callbacks.reserve(invalidation_callbacks_.size());
    for (const auto& [id, callback] : invalidation_callbacks_) {
      (void)id;
      callbacks.push_back(callback);
    }
  }
  for (auto& callback : callbacks) if (callback) callback(address);
}

void AddressSpace::zero(GuestAddress address, std::uint32_t size) {
  fill(address, size, 0);
}

void AddressSpace::fill(GuestAddress address, std::uint32_t size,
                        std::uint8_t value) {
  if (!size) return;
  auto access = access_context();
  access.fill_bytes(address, size, value);
}

void AddressSpace::copy(GuestAddress dest, GuestAddress src, std::uint32_t size) {
  move(dest, src, size);
}

void AddressSpace::move(GuestAddress dest, GuestAddress src, std::uint32_t size) {
  if (!size || dest == src) return;

  // Keep a read-side guard alive from mapping inspection through the copy. Cold
  // management may unpublish mappings concurrently, but retired physical pages
  // cannot be recycled until this context leaves.
  auto access = access_context();

  struct RangePlan {
    bool all_ram{true};
    bool linear{true};
    bool has_first{};
    std::uint32_t first_physical{};
  };

  constexpr std::size_t kPhysicalBitmapWords =
      (kPhysicalPageCount + 63u) / 64u;
  std::array<std::uint64_t, kPhysicalBitmapWords> source_pages{};
  RangePlan source_plan{};
  RangePlan destination_plan{};
  bool physical_page_overlap = false;

  auto inspect_source = [&](GuestAddress base, std::uint32_t length) {
    std::uint32_t remaining = length;
    std::uint32_t logical_offset = 0;
    auto cursor = base;
    while (remaining) {
      const auto page_remaining =
          kBasePageSize - (cursor & (kBasePageSize - 1u));
      const auto chunk = std::min(remaining, page_remaining);
      xenon::cpu::MemoryAccessContext::PhysicalResolution resolved{};
      if (!access.resolve_physical_ram(cursor, chunk, false, 1u, resolved)) {
        source_plan.all_ram = false;
        return;
      }
      if (!source_plan.has_first) {
        source_plan.has_first = true;
        source_plan.first_physical = resolved.physical_address;
      } else if (resolved.physical_address !=
                 source_plan.first_physical + logical_offset) {
        source_plan.linear = false;
      }
      const auto physical_page = resolved.physical_address >> kPageShift;
      source_pages[physical_page >> 6u] |=
          std::uint64_t{1} << (physical_page & 63u);
      cursor += chunk;
      logical_offset += chunk;
      remaining -= chunk;
    }
  };

  auto inspect_destination = [&](GuestAddress base, std::uint32_t length) {
    std::uint32_t remaining = length;
    std::uint32_t logical_offset = 0;
    auto cursor = base;
    while (remaining) {
      const auto page_remaining =
          kBasePageSize - (cursor & (kBasePageSize - 1u));
      const auto chunk = std::min(remaining, page_remaining);
      xenon::cpu::MemoryAccessContext::PhysicalResolution resolved{};
      if (!access.resolve_physical_ram(cursor, chunk, true, 1u, resolved)) {
        destination_plan.all_ram = false;
        return;
      }
      if (!destination_plan.has_first) {
        destination_plan.has_first = true;
        destination_plan.first_physical = resolved.physical_address;
      } else if (resolved.physical_address !=
                 destination_plan.first_physical + logical_offset) {
        destination_plan.linear = false;
      }
      const auto physical_page = resolved.physical_address >> kPageShift;
      if (source_pages[physical_page >> 6u] &
          (std::uint64_t{1} << (physical_page & 63u))) {
        physical_page_overlap = true;
      }
      cursor += chunk;
      logical_offset += chunk;
      remaining -= chunk;
    }
  };

  inspect_source(src, size);
  inspect_destination(dest, size);

  if (source_plan.all_ram && destination_plan.all_ram &&
      source_plan.linear && destination_plan.linear) {
    // The common case: both guest ranges map to linear physical RAM. One host
    // memmove preserves arbitrary virtual alias overlap and one range-level
    // reservation/coherency publication covers the destination.
    const bool reservation_participant =
        begin_physical_write(destination_plan.first_physical, size);
    xenon::cpu::detail::atomic_memmove_guest(
        physical_->data() + destination_plan.first_physical,
        physical_->data() + source_plan.first_physical, size);
    complete_physical_write(destination_plan.first_physical, size,
                            reservation_participant);
    return;
  }

  constexpr std::size_t kCopyScratchSize = 64u * 1024u;
  std::array<std::byte, kCopyScratchSize> scratch{};

  if (source_plan.all_ram && destination_plan.all_ram &&
      !physical_page_overlap) {
    // Non-linear mappings without physical overlap can stream through bounded
    // scratch. This keeps memory use constant regardless of transfer size.
    std::uint32_t offset = 0;
    while (offset < size) {
      const auto chunk = static_cast<std::uint32_t>(
          std::min<std::size_t>(size - offset, scratch.size()));
      auto bytes = std::span<std::byte>(scratch).first(chunk);
      access.read_bytes(src + offset, bytes);
      access.write_bytes(dest + offset, bytes);
      offset += chunk;
    }
    return;
  }

  // MMIO and pathological non-linear physical alias overlap are deliberately
  // slow-path cases. Snapshot all reads before any writes to preserve the V1
  // memmove-like observable ordering and device side effects. Importantly, RAM
  // access inside the snapshot is still page/range based rather than scalar.
  std::vector<std::byte> snapshot(size);
  access.read_bytes(src, snapshot);
  access.write_bytes(dest, snapshot);
}

[[noreturn]] void AddressSpace::fault(GuestAddress address, std::size_t width,
                                      AccessKind access, FaultReason reason,
                                      const char* message) {
  throw MemoryFault(address, width, access, reason, message);
}

}  // namespace xenon::memory

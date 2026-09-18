#include "xenon/memory/address_space.hpp"
#include "xenon/memory/host_vm.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>
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
    if (!data_ || page >= kPhysicalPageCount) return;
    auto* address = data_ + std::size_t(page) * kBasePageSize;
    if (!host_vm::discard(address, kBasePageSize,
                          host_vm::Protection::ReadWrite)) {
      std::memset(address, 0, kBasePageSize);
    }
  }

  std::byte* data() noexcept { return data_; }
  const std::byte* data() const noexcept { return data_; }

 private:
  std::byte* data_{};
};

PhysicalWriteSpan::~PhysicalWriteSpan() noexcept { complete(); }

PhysicalWriteSpan::PhysicalWriteSpan(PhysicalWriteSpan&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      physical_address_(other.physical_address_),
      bytes_(other.bytes_) {
  other.bytes_ = {};
}

PhysicalWriteSpan& PhysicalWriteSpan::operator=(PhysicalWriteSpan&& other) noexcept {
  if (this == &other) return *this;
  complete();
  owner_ = std::exchange(other.owner_, nullptr);
  physical_address_ = other.physical_address_;
  bytes_ = other.bytes_;
  other.bytes_ = {};
  return *this;
}

void PhysicalWriteSpan::complete() noexcept {
  if (!owner_) return;
  auto* owner = std::exchange(owner_, nullptr);
  const auto address = physical_address_;
  const auto size = static_cast<std::uint32_t>(bytes_.size());
  bytes_ = {};
  if (size) owner->note_physical_write(address, size);
}

AddressSpace::AddressSpace()
    : physical_(std::make_unique<PhysicalBacking>()),
      pages_(kPageCount),
      hot_pages_(kPageCount),
      physical_page_used_(kPhysicalPageCount),
      physical_mapping_refs_(kPhysicalPageCount),
      reservation_versions_(kReservationGranuleCount) {
  for (auto& entry : hot_pages_) entry.store(0u, std::memory_order_relaxed);
  for (auto& version : reservation_versions_) version.store(1u, std::memory_order_relaxed);
}

AddressSpace::~AddressSpace() = default;

xenon::cpu::MemoryAccessContext AddressSpace::access_context() noexcept {
  xenon::cpu::FastMemoryView view{};
  if (initialized_ && physical_ && physical_->data()) {
    view.physical_base = physical_->data();
    view.page_table = hot_pages_.data();
    view.page_count = kPageCount;
    view.page_shift = kPageShift;
    view.physical_size = kPhysicalMemorySize;
    view.reservation_versions = reservation_versions_.data();
    view.reservation_granule_size = kReservationGranuleSize;
    view.reservation_granule_count = kReservationGranuleCount;
    view.physical_page_epochs = coherency_.page_epochs_data();
    view.physical_page_count = kPhysicalPageCount;
    view.global_write_epoch = coherency_.write_epoch_data();
    view.active_coherency_writers = coherency_.active_writers_data();
    view.active_fast_readers = &active_fast_readers_;
  }
  return xenon::cpu::MemoryAccessContext(*this, view);
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
  for (auto& version : reservation_versions_) version.store(1u, std::memory_order_relaxed);

  // The first 16 MiB are the GPU writeback/XPS physical window. Keep them out
  // of the anonymous physical-frame allocator while preserving direct aliases.
  std::fill_n(physical_page_used_.begin(), kHugePageSize / kBasePageSize,
              kPhysicalSystem);

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
  hot_pages_[page_index].store(make_hot_entry(page_index),
                               std::memory_order_release);
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
        remove_physical_mapping_ref(rollback_physical);
        if (region->kind == RegionKind::Xex) {
          const GuestAddress addr = base + j * kBasePageSize;
          const GuestAddress alias = addr < kXex4KBase
                                         ? addr + 0x10000000u
                                         : addr - 0x10000000u;
          auto& alias_page = pages_[alias >> kPageShift];
          remove_physical_mapping_ref(rollback_physical);
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
    add_physical_mapping_ref(physical_page);
    std::memset(physical_->data() + std::size_t(physical_page) * kBasePageSize, 0,
                kBasePageSize);

    if (region->kind == RegionKind::Xex) {
      const GuestAddress addr = base + i * kBasePageSize;
      const GuestAddress alias = addr < kXex4KBase ? addr + 0x10000000u : addr - 0x10000000u;
      auto& alias_page = pages_[alias >> kPageShift];
      alias_page.physical_page = physical_page;
      alias_page.state = PageState::Committed;
      alias_page.current_protect = protect;
      add_physical_mapping_ref(physical_page);
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
      remove_physical_mapping_ref(physical_page);
      if (region->kind == RegionKind::Xex) {
        const GuestAddress addr = (first + i) << kPageShift;
        const GuestAddress alias = addr < kXex4KBase
                                       ? addr + 0x10000000u
                                       : addr - 0x10000000u;
        auto& alias_page = pages_[alias >> kPageShift];
        remove_physical_mapping_ref(physical_page);
        alias_page.physical_page = kInvalidPhysicalPage;
        alias_page.state = PageState::Reserved;
      }
      if (!page.explicit_physical_mapping) {
        free_physical_page(physical_page);
      }
      page.physical_page = kInvalidPhysicalPage;
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
      remove_physical_mapping_ref(physical_page);
      if (region->kind == RegionKind::Xex) {
        const GuestAddress addr = (page_index + i) << kPageShift;
        const GuestAddress alias = addr < kXex4KBase
                                       ? addr + 0x10000000u
                                       : addr - 0x10000000u;
        remove_physical_mapping_ref(physical_page);
        pages_[alias >> kPageShift] = Page{};
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
  if (!top_down) {
    for (std::uint32_t i = 0; i < kPhysicalPageCount; ++i) {
      if (physical_page_used_[i] == kPhysicalFree) {
        physical_page_used_[i] = kPhysicalAnonymous;
        return i;
      }
    }
  } else {
    for (std::uint32_t i = kPhysicalPageCount; i-- > 0;) {
      if (physical_page_used_[i] == kPhysicalFree) {
        physical_page_used_[i] = kPhysicalAnonymous;
        return i;
      }
    }
  }
  return kInvalidPhysicalPage;
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
}

void AddressSpace::reclaim_retired_physical_pages() {
  if (active_fast_readers_.load(std::memory_order_acquire) != 0u) return;

  std::uint32_t page = kHugePageSize / kBasePageSize;
  while (page < kPhysicalPageCount) {
    if (physical_page_used_[page] != kPhysicalRetired) {
      ++page;
      continue;
    }
    const auto first = page;
    while (page < kPhysicalPageCount &&
           physical_page_used_[page] == kPhysicalRetired) {
      physical_page_used_[page] = kPhysicalFree;
      physical_->discard_page(page);
      ++page;
    }
    note_physical_write(first * kBasePageSize,
                        (page - first) * kBasePageSize);
  }
}

void AddressSpace::add_physical_mapping_ref(std::uint32_t page) {
  if (page >= kPhysicalPageCount) return;
  ++physical_mapping_refs_[page];
}

void AddressSpace::remove_physical_mapping_ref(std::uint32_t page) {
  if (page >= kPhysicalPageCount || physical_mapping_refs_[page] == 0u) return;
  --physical_mapping_refs_[page];
  if (physical_mapping_refs_[page] == 0u &&
      physical_page_used_[page] == kPhysicalAnonymousPendingFree) {
    free_physical_page(page);
  }
}

bool AddressSpace::reserve_physical_run(std::uint32_t count, std::uint32_t alignment_pages,
                                        bool top_down, std::uint32_t& out_first_page) {
  if (!count || count > kPhysicalPageCount || !alignment_pages) return false;
  reclaim_retired_physical_pages();
  auto free_run = [&](std::uint32_t first) {
    if (first + count > kPhysicalPageCount) return false;
    for (std::uint32_t i = 0; i < count; ++i) if (physical_page_used_[first + i]) return false;
    return true;
  };
  if (!top_down) {
    for (std::uint32_t first = 0; first + count <= kPhysicalPageCount; first += alignment_pages) {
      if (!free_run(first)) continue;
      std::fill_n(physical_page_used_.begin() + first, count, kPhysicalExplicit);
      out_first_page = first;
      return true;
    }
  } else {
    std::uint32_t first = (kPhysicalPageCount - count) / alignment_pages * alignment_pages;
    while (true) {
      if (free_run(first)) {
        std::fill_n(physical_page_used_.begin() + first, count, kPhysicalExplicit);
        out_first_page = first;
        return true;
      }
      if (first < alignment_pages) break;
      first -= alignment_pages;
    }
  }
  return false;
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
  std::memset(physical_->data() + out_physical_address, 0, std::size_t(count) * kBasePageSize);
  note_physical_write(out_physical_address, count * kBasePageSize);
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
  for (std::uint32_t i = 0; i < count; ++i) {
    if (physical_page_used_[physical_base / kBasePageSize + i] ==
        kPhysicalRetired) {
      return false;
    }
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    auto& p = pages_[first + i];
    p.physical_page = physical_base / kBasePageSize + i;
    p.allocation_base_page = first;
    p.allocation_page_count = count;
    p.allocation_protect = protect_value;
    p.current_protect = protect_value;
    p.state = PageState::Committed;
    p.kind = RegionKind::Virtual;
    p.explicit_physical_mapping = true;
    if (physical_page_used_[p.physical_page] == kPhysicalFree) {
      physical_page_used_[p.physical_page] = kPhysicalExplicit;
    }
    add_physical_mapping_ref(p.physical_page);
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
  std::memcpy(destination.data(), physical_->data() + physical_address,
              destination.size());
  return true;
}

bool AddressSpace::write_physical(std::uint32_t physical_address,
                                  std::span<const std::byte> source) {
  if (source.empty()) return true;
  auto write = physical_write_span(
      physical_address, static_cast<std::uint32_t>(source.size()));
  if (!write) return false;
  std::memcpy(write.bytes().data(), source.data(), source.size());
  return true;
}

bool AddressSpace::fill_physical(std::uint32_t physical_address,
                                 std::uint32_t size, std::byte value) {
  if (!size) return true;
  auto write = physical_write_span(physical_address, size);
  if (!write) return false;
  std::memset(write.bytes().data(), std::to_integer<int>(value), size);
  return true;
}

PhysicalWriteSpan AddressSpace::physical_write_span(
    std::uint32_t physical_address, std::uint32_t size) noexcept {
  if (!initialized_ || !size || physical_address >= kPhysicalMemorySize ||
      std::uint64_t{physical_address} + size > kPhysicalMemorySize) {
    return {};
  }
  return PhysicalWriteSpan(
      this, physical_address,
      std::span<std::byte>(physical_->data() + physical_address, size));
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
  std::lock_guard lock(mutex_);
  if (find_mmio(address, sizeof(T))) {
    T v = static_cast<T>(read_mmio(address, sizeof(T)));
    return little_endian ? byteswap_if(v, true) : v;
  }
  if (auto contiguous = resolve_contiguous(address, sizeof(T), AccessKind::Read)) {
    T raw{};
    std::memcpy(&raw, contiguous->ptr, sizeof(T));
    const bool host_little = std::endian::native == std::endian::little;
    const bool swap = little_endian ? !host_little : host_little;
    return byteswap_if(raw, swap);
  }
  T value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    const auto b = std::to_integer<std::uint8_t>(
        *resolve_byte(address + static_cast<GuestAddress>(i), AccessKind::Read).ptr);
    if (little_endian) value |= static_cast<T>(b) << (i * 8u);
    else value = static_cast<T>((value << 8u) | b);
  }
  return value;
}

template <typename T>
void AddressSpace::write_integer(GuestAddress address, T value, bool little_endian) {
  std::lock_guard lock(mutex_);
  if (find_mmio(address, sizeof(T))) {
    write_mmio(address, sizeof(T), little_endian ? byteswap_if(value, true) : value);
    return;
  }
  if (auto contiguous = resolve_contiguous(address, sizeof(T), AccessKind::Write)) {
    const bool host_little = std::endian::native == std::endian::little;
    const bool swap = little_endian ? !host_little : host_little;
    const T raw = byteswap_if(value, swap);
    std::memcpy(contiguous->ptr, &raw, sizeof(T));
    note_physical_write(contiguous->physical_address, sizeof(T));
    return;
  }
  std::array<std::uint32_t, sizeof(T)> physical_addresses{};
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    const unsigned shift = little_endian ? static_cast<unsigned>(i * 8u)
                                         : static_cast<unsigned>((sizeof(T) - 1u - i) * 8u);
    auto resolved = resolve_byte(address + static_cast<GuestAddress>(i), AccessKind::Write);
    *resolved.ptr = static_cast<std::byte>((value >> shift) & 0xFFu);
    physical_addresses[i] = resolved.physical_address;
  }
  note_physical_write_addresses(physical_addresses);
}

std::uint8_t AddressSpace::read8(GuestAddress address) {
  std::lock_guard lock(mutex_);
  if (find_mmio(address, 1)) return static_cast<std::uint8_t>(read_mmio(address, 1));
  return std::to_integer<std::uint8_t>(*resolve_byte(address, AccessKind::Read).ptr);
}
std::uint16_t AddressSpace::read16_be(GuestAddress a) { return read_integer<std::uint16_t>(a, false); }
std::uint32_t AddressSpace::read32_be(GuestAddress a) { return read_integer<std::uint32_t>(a, false); }
std::uint64_t AddressSpace::read64_be(GuestAddress a) { return read_integer<std::uint64_t>(a, false); }
xenon::cpu::Vector128 AddressSpace::read128(GuestAddress address) {
  std::lock_guard lock(mutex_);
  xenon::cpu::Vector128 v{};
  if (auto contiguous = resolve_contiguous(address, v.bytes.size(), AccessKind::Read)) {
    std::memcpy(v.bytes.data(), contiguous->ptr, v.bytes.size());
    return v;
  }
  for (std::size_t i = 0; i < v.bytes.size(); ++i)
    v.bytes[i] = std::to_integer<std::uint8_t>(*resolve_byte(address + static_cast<GuestAddress>(i), AccessKind::Read).ptr);
  return v;
}

void AddressSpace::write8(GuestAddress address, std::uint8_t value) {
  std::lock_guard lock(mutex_);
  if (find_mmio(address, 1)) { write_mmio(address, 1, value); return; }
  auto r = resolve_byte(address, AccessKind::Write);
  *r.ptr = static_cast<std::byte>(value);
  note_physical_write(r.physical_address, 1);
}
void AddressSpace::write16_be(GuestAddress a, std::uint16_t v) { write_integer(a, v, false); }
void AddressSpace::write32_be(GuestAddress a, std::uint32_t v) { write_integer(a, v, false); }
void AddressSpace::write64_be(GuestAddress a, std::uint64_t v) { write_integer(a, v, false); }
void AddressSpace::write128(GuestAddress address, const xenon::cpu::Vector128& value) {
  std::lock_guard lock(mutex_);
  if (auto contiguous = resolve_contiguous(address, value.bytes.size(), AccessKind::Write)) {
    std::memcpy(contiguous->ptr, value.bytes.data(), value.bytes.size());
    note_physical_write(contiguous->physical_address, static_cast<std::uint32_t>(value.bytes.size()));
    return;
  }
  std::array<std::uint32_t, 16> physical_addresses{};
  for (std::size_t i = 0; i < value.bytes.size(); ++i) {
    auto r = resolve_byte(address + static_cast<GuestAddress>(i), AccessKind::Write);
    *r.ptr = static_cast<std::byte>(value.bytes[i]);
    physical_addresses[i] = r.physical_address;
  }
  note_physical_write_addresses(physical_addresses);
}

std::uint16_t AddressSpace::read16_le(GuestAddress a) { return read_integer<std::uint16_t>(a, true); }
std::uint32_t AddressSpace::read32_le(GuestAddress a) { return read_integer<std::uint32_t>(a, true); }
std::uint64_t AddressSpace::read64_le(GuestAddress a) { return read_integer<std::uint64_t>(a, true); }
void AddressSpace::write16_le(GuestAddress a, std::uint16_t v) { write_integer(a, v, true); }
void AddressSpace::write32_le(GuestAddress a, std::uint32_t v) { write_integer(a, v, true); }
void AddressSpace::write64_le(GuestAddress a, std::uint64_t v) { write_integer(a, v, true); }

std::uint64_t AddressSpace::reservation_version(std::uint32_t physical_address) const {
  if (physical_address >= kPhysicalMemorySize) return 0;
  return reservation_versions_[physical_address / kReservationGranuleSize].load(std::memory_order_acquire);
}
void AddressSpace::note_physical_write(std::uint32_t physical_address,
                                       std::uint32_t width) noexcept {
  if (physical_address >= kPhysicalMemorySize || !width) return;
  const auto first = physical_address / kReservationGranuleSize;
  const auto last = std::min<std::uint32_t>(
      static_cast<std::uint32_t>((std::uint64_t{physical_address} + width - 1u) /
                                 kReservationGranuleSize),
      kReservationGranuleCount - 1u);
  for (std::uint32_t i = first; i <= last; ++i) {
    auto next = reservation_versions_[i].fetch_add(1u, std::memory_order_acq_rel) + 1u;
    if (next == 0u) reservation_versions_[i].store(1u, std::memory_order_release);
  }
  coherency_.mark_write(physical_address, width);
}

void AddressSpace::note_physical_write_addresses(
    std::span<const std::uint32_t> physical_addresses) {
  if (physical_addresses.empty()) return;
  std::size_t first = 0;
  for (std::size_t i = 1; i <= physical_addresses.size(); ++i) {
    if (i != physical_addresses.size() &&
        physical_addresses[i] == physical_addresses[i - 1u] + 1u) {
      continue;
    }
    note_physical_write(
        physical_addresses[first],
        static_cast<std::uint32_t>(i - first));
    first = i;
  }
}

std::uint64_t AddressSpace::reserve32(GuestAddress address, std::uint32_t& value) {
  std::lock_guard lock(mutex_);
  value = read32_be(address);
  const auto phys = get_physical_address(address);
  if (phys == 0xFFFFFFFFu) fault(address, 4, AccessKind::Read, FaultReason::Unmapped, "reservation has no physical RAM backing");
  return (std::uint64_t(phys / kReservationGranuleSize) << 32) |
         std::uint64_t(reservation_version(phys));
}
std::uint64_t AddressSpace::reserve64(GuestAddress address, std::uint64_t& value) {
  std::lock_guard lock(mutex_);
  value = read64_be(address);
  const auto phys = get_physical_address(address);
  if (phys == 0xFFFFFFFFu) fault(address, 8, AccessKind::Read, FaultReason::Unmapped, "reservation has no physical RAM backing");
  return (std::uint64_t(phys / kReservationGranuleSize) << 32) |
         std::uint64_t(reservation_version(phys));
}
bool AddressSpace::store_conditional32(GuestAddress address, std::uint64_t token,
                                       std::uint32_t value) {
  std::lock_guard lock(mutex_);
  const auto phys = get_physical_address(address);
  if (phys == 0xFFFFFFFFu) return false;
  const auto granule = phys / kReservationGranuleSize;
  const auto token_granule = static_cast<std::uint32_t>(token >> 32);
  const auto token_version = static_cast<std::uint32_t>(token);
  if (granule != token_granule || reservation_version(phys) != token_version) return false;
  write32_be(address, value);
  return true;
}
bool AddressSpace::store_conditional64(GuestAddress address, std::uint64_t token,
                                       std::uint64_t value) {
  std::lock_guard lock(mutex_);
  const auto phys = get_physical_address(address);
  if (phys == 0xFFFFFFFFu) return false;
  const auto granule = phys / kReservationGranuleSize;
  const auto token_granule = static_cast<std::uint32_t>(token >> 32);
  const auto token_version = static_cast<std::uint32_t>(token);
  if (granule != token_granule || reservation_version(phys) != token_version) return false;
  write64_be(address, value);
  return true;
}

void AddressSpace::barrier(xenon::cpu::BarrierKind kind) {
  switch (kind) {
    case xenon::cpu::BarrierKind::LightweightSync:
      std::atomic_thread_fence(std::memory_order_acq_rel);
      break;
    case xenon::cpu::BarrierKind::Eieio:
      std::atomic_thread_fence(std::memory_order_release);
      break;
    case xenon::cpu::BarrierKind::InstructionSync:
      std::atomic_thread_fence(std::memory_order_acquire);
      break;
    case xenon::cpu::BarrierKind::Sync:
    default:
      std::atomic_thread_fence(std::memory_order_seq_cst);
      break;
  }
}

void AddressSpace::zero_cache_block(GuestAddress address, std::uint32_t bytes) {
  if (!bytes || !std::has_single_bit(bytes)) return;
  const auto base = address & ~(bytes - 1u);
  fill(base, bytes, 0);
}

void AddressSpace::instruction_cache_invalidate(GuestAddress address) {
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
  std::lock_guard lock(mutex_);
  std::uint32_t remaining = size;
  auto cursor = address;
  while (remaining) {
    const auto page_remaining =
        kBasePageSize - (cursor & (kBasePageSize - 1u));
    const auto chunk = std::min(remaining, page_remaining);
    if (page_has_mmio(cursor >> kPageShift)) {
      // Preserve the old byte-wise MMIO semantics on the exceptional path.
      for (std::uint32_t i = 0; i < chunk; ++i) {
        write8(cursor + i, value);
      }
    } else {
      auto resolved = resolve_contiguous(cursor, chunk, AccessKind::Write);
      if (!resolved) {
        for (std::uint32_t i = 0; i < chunk; ++i) write8(cursor + i, value);
      } else {
        std::memset(resolved->ptr, value, chunk);
        note_physical_write(resolved->physical_address, chunk);
      }
    }
    cursor += chunk;
    remaining -= chunk;
  }
}

void AddressSpace::copy(GuestAddress dest, GuestAddress src, std::uint32_t size) {
  if (!size || dest == src) return;
  std::lock_guard lock(mutex_);

  // Snapshot first so arbitrary physical aliases retain memmove-like semantics
  // even when guest virtual ordering doesn't reveal the backing overlap. Common
  // RAM pages are copied natively a page chunk at a time rather than as scalar
  // MemoryPort operations.
  std::vector<std::byte> temporary(size);
  std::uint32_t remaining = size;
  auto source = src;
  std::size_t offset = 0;
  while (remaining) {
    const auto page_remaining =
        kBasePageSize - (source & (kBasePageSize - 1u));
    const auto chunk = std::min(remaining, page_remaining);
    if (page_has_mmio(source >> kPageShift)) {
      for (std::uint32_t i = 0; i < chunk; ++i) {
        temporary[offset + i] = static_cast<std::byte>(read8(source + i));
      }
    } else {
      auto resolved = resolve_contiguous(source, chunk, AccessKind::Read);
      if (!resolved) {
        for (std::uint32_t i = 0; i < chunk; ++i) {
          temporary[offset + i] = static_cast<std::byte>(read8(source + i));
        }
      } else {
        std::memcpy(temporary.data() + offset, resolved->ptr, chunk);
      }
    }
    source += chunk;
    offset += chunk;
    remaining -= chunk;
  }

  remaining = size;
  auto destination = dest;
  offset = 0;
  while (remaining) {
    const auto page_remaining =
        kBasePageSize - (destination & (kBasePageSize - 1u));
    const auto chunk = std::min(remaining, page_remaining);
    if (page_has_mmio(destination >> kPageShift)) {
      for (std::uint32_t i = 0; i < chunk; ++i) {
        write8(destination + i,
               std::to_integer<std::uint8_t>(temporary[offset + i]));
      }
    } else {
      auto resolved =
          resolve_contiguous(destination, chunk, AccessKind::Write);
      if (!resolved) {
        for (std::uint32_t i = 0; i < chunk; ++i) {
          write8(destination + i,
                 std::to_integer<std::uint8_t>(temporary[offset + i]));
        }
      } else {
        std::memcpy(resolved->ptr, temporary.data() + offset, chunk);
        note_physical_write(resolved->physical_address, chunk);
      }
    }
    destination += chunk;
    offset += chunk;
    remaining -= chunk;
  }
}

[[noreturn]] void AddressSpace::fault(GuestAddress address, std::size_t width,
                                      AccessKind access, FaultReason reason,
                                      const char* message) {
  throw MemoryFault(address, width, access, reason, message);
}

}  // namespace xenon::memory

#include "xenon/memory/address_space.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

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
#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_NORESERVE)
    flags |= MAP_NORESERVE;
#endif
    void* p = mmap(nullptr, kPhysicalMemorySize, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (p == MAP_FAILED) return false;
    data_ = static_cast<std::byte*>(p);
    mapped_ = true;
#elif defined(_WIN32)
    void* p = VirtualAlloc(nullptr, kPhysicalMemorySize,
                           MEM_RESERVE, PAGE_READWRITE);
    if (!p) return false;
    if (!VirtualAlloc(p, kPhysicalMemorySize, MEM_COMMIT, PAGE_READWRITE)) {
      VirtualFree(p, 0, MEM_RELEASE);
      return false;
    }
    data_ = static_cast<std::byte*>(p);
    mapped_ = true;
#else
    fallback_ = std::make_unique<std::byte[]>(kPhysicalMemorySize);
    if (!fallback_) return false;
    std::memset(fallback_.get(), 0, kPhysicalMemorySize);
    data_ = fallback_.get();
#endif
    return true;
  }

  void dispose() {
    if (!data_) return;
#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
    if (mapped_) munmap(data_, kPhysicalMemorySize);
#elif defined(_WIN32)
    if (mapped_) VirtualFree(data_, 0, MEM_RELEASE);
#endif
    fallback_.reset();
    data_ = nullptr;
    mapped_ = false;
  }

  void reset() {
    if (!data_) return;
#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
#if defined(MADV_DONTNEED)
    if (mapped_) {
      madvise(data_, kPhysicalMemorySize, MADV_DONTNEED);
      return;
    }
#endif
#elif defined(_WIN32)
    if (mapped_) {
      if (VirtualFree(data_, kPhysicalMemorySize, MEM_DECOMMIT)) {
        if (VirtualAlloc(data_, kPhysicalMemorySize, MEM_COMMIT,
                         PAGE_READWRITE)) {
          return;
        }
        // The range is still reserved but no longer accessible. Continuing
        // into memset would access decommitted pages and hide the real host
        // allocation failure behind an access violation.
        throw std::bad_alloc{};
      }
      // Retain a valid backing even if a host refuses whole-region decommit.
      std::memset(data_, 0, kPhysicalMemorySize);
      return;
    }
#endif
    std::memset(data_, 0, kPhysicalMemorySize);
  }

  std::byte* data() noexcept { return data_; }
  const std::byte* data() const noexcept { return data_; }

 private:
  std::byte* data_{};
  bool mapped_{};
  std::unique_ptr<std::byte[]> fallback_{};
};

AddressSpace::AddressSpace()
    : physical_(std::make_unique<PhysicalBacking>()),
      pages_(kPageCount),
      physical_page_used_(kPhysicalPageCount),
      reservation_versions_(kReservationGranuleCount) {
  for (auto& version : reservation_versions_) version.store(1u, std::memory_order_relaxed);
}

AddressSpace::~AddressSpace() = default;

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
  std::fill(physical_page_used_.begin(), physical_page_used_.end(), std::uint8_t{0});
  for (auto& version : reservation_versions_) version.store(1u, std::memory_order_relaxed);

  // The first 16 MiB are the GPU writeback/XPS physical window. Keep them out
  // of the anonymous physical-frame allocator while preserving direct aliases.
  std::fill_n(physical_page_used_.begin(), kHugePageSize / kBasePageSize,
              std::uint8_t{1});

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
  // A reset zeroes all physical RAM. Existing GPU/APU mirrors must invalidate
  // their complete copies even though there were no individual CPU writes.
  std::vector<PhysicalWriteCallback> callbacks;
  callbacks.reserve(physical_write_callbacks_.size());
  for (const auto& [id, callback] : physical_write_callbacks_) {
    (void)id;
    callbacks.push_back(callback);
  }
  for (auto& callback : callbacks) {
    if (callback) callback(0, kPhysicalMemorySize);
  }
}

std::span<const RegionDescriptor> AddressSpace::regions() noexcept { return kRegions; }

const RegionDescriptor* AddressSpace::region_for(GuestAddress address) noexcept {
  for (const auto& region : kRegions) {
    if (address >= region.base && address <= region.end) return &region;
  }
  return nullptr;
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
      continue;
    }
    const auto physical_page = allocate_physical_page(false);
    if (physical_page == kInvalidPhysicalPage) {
      for (auto p : newly_allocated) free_physical_page(p);
      for (std::uint32_t j = 0; j < i; ++j) {
        auto& rollback = pages_[first + j];
        if (std::find(newly_allocated.begin(), newly_allocated.end(), rollback.physical_page) != newly_allocated.end()) {
          rollback.physical_page = kInvalidPhysicalPage;
          rollback.state = PageState::Reserved;
        }
      }
      return false;
    }
    newly_allocated.push_back(physical_page);
    page.physical_page = physical_page;
    page.state = PageState::Committed;
    page.current_protect = protect;
    std::memset(physical_->data() + std::size_t(physical_page) * kBasePageSize, 0,
                kBasePageSize);

    if (region->kind == RegionKind::Xex) {
      const GuestAddress addr = base + i * kBasePageSize;
      const GuestAddress alias = addr < kXex4KBase ? addr + 0x10000000u : addr - 0x10000000u;
      auto& alias_page = pages_[alias >> kPageShift];
      alias_page.physical_page = physical_page;
      alias_page.state = PageState::Committed;
      alias_page.current_protect = protect;
    }
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
    if (page.state == PageState::Committed && page.physical_page != kInvalidPhysicalPage) {
      free_physical_page(page.physical_page);
      page.physical_page = kInvalidPhysicalPage;
    }
    page.state = PageState::Reserved;
    if (region->kind == RegionKind::Xex) {
      const GuestAddress addr = (first + i) << kPageShift;
      const GuestAddress alias = addr < kXex4KBase ? addr + 0x10000000u : addr - 0x10000000u;
      auto& alias_page = pages_[alias >> kPageShift];
      alias_page.physical_page = kInvalidPhysicalPage;
      alias_page.state = PageState::Reserved;
    }
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
  for (std::uint32_t i = 0; i < page.allocation_page_count; ++i) {
    auto& p = pages_[page_index + i];
    if (p.state == PageState::Committed && p.physical_page != kInvalidPhysicalPage) free_physical_page(p.physical_page);
    p = Page{};
    if (region->kind == RegionKind::Xex) {
      const GuestAddress addr = (page_index + i) << kPageShift;
      const GuestAddress alias = addr < kXex4KBase ? addr + 0x10000000u : addr - 0x10000000u;
      pages_[alias >> kPageShift] = Page{};
    }
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
  if (!top_down) {
    for (std::uint32_t i = 0; i < kPhysicalPageCount; ++i) {
      if (!physical_page_used_[i]) { physical_page_used_[i] = 2; return i; }
    }
  } else {
    for (std::uint32_t i = kPhysicalPageCount; i-- > 0;) {
      if (!physical_page_used_[i]) { physical_page_used_[i] = 2; return i; }
    }
  }
  return kInvalidPhysicalPage;
}

void AddressSpace::free_physical_page(std::uint32_t page) {
  if (page >= kPhysicalPageCount) return;
  // Dedicated GPU writeback pages stay reserved.
  if (page < kHugePageSize / kBasePageSize) return;
  if (physical_page_used_[page] != 2) return;
  physical_page_used_[page] = 0;
#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
#if defined(MADV_DONTNEED)
  madvise(physical_->data() + std::size_t(page) * kBasePageSize, kBasePageSize, MADV_DONTNEED);
#else
  std::memset(physical_->data() + std::size_t(page) * kBasePageSize, 0, kBasePageSize);
#endif
#else
  std::memset(physical_->data() + std::size_t(page) * kBasePageSize, 0, kBasePageSize);
#endif
  note_physical_write(page * kBasePageSize, kBasePageSize);
}

bool AddressSpace::reserve_physical_run(std::uint32_t count, std::uint32_t alignment_pages,
                                        bool top_down, std::uint32_t& out_first_page) {
  if (!count || count > kPhysicalPageCount || !alignment_pages) return false;
  auto free_run = [&](std::uint32_t first) {
    if (first + count > kPhysicalPageCount) return false;
    for (std::uint32_t i = 0; i < count; ++i) if (physical_page_used_[first + i]) return false;
    return true;
  };
  if (!top_down) {
    for (std::uint32_t first = 0; first + count <= kPhysicalPageCount; first += alignment_pages) {
      if (!free_run(first)) continue;
      std::fill_n(physical_page_used_.begin() + first, count, std::uint8_t{3});
      out_first_page = first;
      return true;
    }
  } else {
    std::uint32_t first = (kPhysicalPageCount - count) / alignment_pages * alignment_pages;
    while (true) {
      if (free_run(first)) {
        std::fill_n(physical_page_used_.begin() + first, count, std::uint8_t{3});
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
    if (physical_page_used_[first + i] != 3) return false;
  }
  // A physical allocation can't be released while a virtual mapping still
  // references it. The owning GPU/APU/kernel layer must unmap first.
  for (const auto& page : pages_) {
    if (page.state != PageState::Committed || page.physical_page == kInvalidPhysicalPage) continue;
    if (page.physical_page >= first && page.physical_page < first + count) return false;
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    physical_page_used_[first + i] = 0;
#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
#if defined(MADV_DONTNEED)
    madvise(physical_->data() + std::size_t(first + i) * kBasePageSize,
            kBasePageSize, MADV_DONTNEED);
#else
    std::memset(physical_->data() + std::size_t(first + i) * kBasePageSize, 0,
                kBasePageSize);
#endif
#else
    std::memset(physical_->data() + std::size_t(first + i) * kBasePageSize, 0,
                kBasePageSize);
#endif
  }
  note_physical_write(physical_base, count * kBasePageSize);
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
    if (physical_page_used_[p.physical_page] == 0) physical_page_used_[p.physical_page] = 3;
  }
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

std::byte* AddressSpace::physical_data(std::uint32_t physical_address) {
  if (!initialized_ || physical_address >= kPhysicalMemorySize) return nullptr;
  return physical_->data() + physical_address;
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
  return true;
}
void AddressSpace::clear_mmio_ranges() {
  std::lock_guard lock(mutex_);
  mmio_ranges_.clear();
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

void AddressSpace::notify_external_write(std::uint32_t physical_address,
                                         std::uint32_t width) {
  std::lock_guard lock(mutex_);
  if (!width || std::uint64_t(physical_address) + width > kPhysicalMemorySize) {
    fault(physical_address, width, AccessKind::Write, FaultReason::OutOfRange,
          "external physical write outside RAM");
  }
  note_physical_write(physical_address, width);
}

std::uint64_t AddressSpace::add_physical_write_callback(PhysicalWriteCallback callback) {
  std::lock_guard lock(mutex_);
  const auto id = next_physical_write_callback_id_++;
  physical_write_callbacks_.push_back({id, std::move(callback)});
  return id;
}

void AddressSpace::remove_physical_write_callback(std::uint64_t id) {
  std::lock_guard lock(mutex_);
  std::erase_if(physical_write_callbacks_,
                [id](const auto& pair) { return pair.first == id; });
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
void AddressSpace::note_physical_write(std::uint32_t physical_address, std::uint32_t width) {
  if (physical_address >= kPhysicalMemorySize || !width) return;
  const auto first = physical_address / kReservationGranuleSize;
  const auto last = std::min<std::uint32_t>((physical_address + width - 1u) / kReservationGranuleSize,
                                            kReservationGranuleCount - 1u);
  for (std::uint32_t i = first; i <= last; ++i) {
    auto next = reservation_versions_[i].fetch_add(1u, std::memory_order_acq_rel) + 1u;
    if (next == 0u) reservation_versions_[i].store(1u, std::memory_order_release);
  }
  // These observers are intended for lightweight cache-dirty notifications in
  // GPU/APU subsystems. They run synchronously so visibility follows the write.
  std::vector<PhysicalWriteCallback> callbacks;
  callbacks.reserve(physical_write_callbacks_.size());
  for (const auto& [id, callback] : physical_write_callbacks_) {
    (void)id;
    callbacks.push_back(callback);
  }
  for (auto& callback : callbacks)
    if (callback) callback(physical_address, width);
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

void AddressSpace::zero(GuestAddress address, std::uint32_t size) { fill(address, size, 0); }
void AddressSpace::fill(GuestAddress address, std::uint32_t size, std::uint8_t value) {
  for (std::uint32_t i = 0; i < size; ++i) write8(address + i, value);
}
void AddressSpace::copy(GuestAddress dest, GuestAddress src, std::uint32_t size) {
  if (!size || dest == src) return;
  std::vector<std::uint8_t> temporary(size);
  for (std::uint32_t i = 0; i < size; ++i) temporary[i] = read8(src + i);
  for (std::uint32_t i = 0; i < size; ++i) write8(dest + i, temporary[i]);
}

[[noreturn]] void AddressSpace::fault(GuestAddress address, std::size_t width,
                                      AccessKind access, FaultReason reason,
                                      const char* message) {
  throw MemoryFault(address, width, access, reason, message);
}

}  // namespace xenon::memory

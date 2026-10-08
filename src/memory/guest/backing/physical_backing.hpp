#pragma once

// Host-side backing of guest physical memory: the shared 512 MiB physical
// store and the optional 4 GiB direct guest aperture that aliases it.
// Private to xenon_memory; included through address_space_internal.hpp.

namespace xenon::memory {

class AddressSpace::PhysicalBacking {
 public:
  PhysicalBacking() = default;
  ~PhysicalBacking() { dispose(); }

  bool initialize() {
    if (data_) return true;
    shared_ = host_vm::create_shared(kPhysicalMemorySize);
    if (!shared_.valid()) return false;
    auto* mapping = host_vm::map_shared(shared_, 0, kPhysicalMemorySize,
                                        host_vm::Protection::ReadWrite);
    if (!mapping) {
      shared_ = {};
      return false;
    }
    data_ = static_cast<std::byte*>(mapping);
    freshly_created_ = true;
    return true;
  }

  void dispose() noexcept {
    if (data_) {
      (void)host_vm::unmap(data_, kPhysicalMemorySize);
      data_ = nullptr;
    }
    shared_ = {};
  }

  void reset() {
    if (!data_) return;
    // A newly-created shared object is already zero-filled by both the POSIX
    // and Windows host contracts. Avoid faulting in all 512 MiB merely to zero
    // it again on first AddressSpace initialization. Later explicit resets do
    // clear the live shared backing so every host alias observes the reset.
    if (freshly_created_) {
      freshly_created_ = false;
      return;
    }
    std::memset(data_, 0, kPhysicalMemorySize);
  }

  void discard_page(std::uint32_t page) noexcept {
    discard_range(page, 1u);
  }

  void discard_range(std::uint32_t first_page,
                     std::uint32_t page_count) noexcept {
    if (!data_ || !page_count || first_page >= kPhysicalPageCount ||
        std::uint64_t(first_page) + page_count > kPhysicalPageCount) {
      return;
    }
    auto* address = data_ + std::size_t(first_page) * kBasePageSize;
    const auto size = std::size_t(page_count) * kBasePageSize;
    std::memset(address, 0, size);
  }

  std::byte* data() noexcept { return data_; }
  const std::byte* data() const noexcept { return data_; }
  const host_vm::SharedMemory& shared() const noexcept { return shared_; }

 private:
  host_vm::SharedMemory shared_{};
  std::byte* data_{};
  bool freshly_created_{};
};

class AddressSpace::GuestAperture {
 public:
  static constexpr std::uint64_t kSize = std::uint64_t{1} << 32u;
  static constexpr std::uint32_t kInvalidPage = 0xFFFFFFFFu;

  GuestAperture() = default;
  ~GuestAperture() { dispose(); }

  bool initialize(const host_vm::SharedMemory& shared) {
    if (active()) return true;
    const auto host_capabilities = host_vm::capabilities();
    // The direct aperture aliases guest pages individually. A host may support
    // fixed mappings in general while still requiring a mapping granularity
    // larger than the Xbox 360's 4 KiB base page (Apple Silicon commonly uses
    // larger host pages). In that case compact translation remains the safe,
    // fully functional path instead of letting aperture setup fail piecemeal.
    if (sizeof(void*) < 8u || !shared.valid() ||
        !host_capabilities.supports_fixed_mapping_granularity(kBasePageSize) ||
        kSize > (std::numeric_limits<std::size_t>::max)()) {
      return false;
    }
    auto* reservation = host_vm::reserve_fixed_shared_mapping_region(
        static_cast<std::size_t>(kSize));
    if (!reservation) return false;
    base_ = static_cast<std::byte*>(reservation);
    shared_ = &shared;
    mapped_physical_pages_.assign(kPageCount, kInvalidPage);
    if (!reset()) {
      dispose();
      return false;
    }
    return true;
  }

  void dispose() noexcept {
    if (base_) {
      host_vm::release_fixed_shared_mapping_region(
          base_, static_cast<std::size_t>(kSize));
      base_ = nullptr;
    }
    shared_ = nullptr;
    mapped_physical_pages_.clear();
  }

  [[nodiscard]] bool active() const noexcept {
    return base_ && shared_ && shared_->valid();
  }

  [[nodiscard]] std::byte* base() const noexcept { return base_; }

  [[nodiscard]] bool mapping_matches(std::uint32_t guest_page,
                                     std::uint32_t physical_page) const noexcept {
    return active() && guest_page < mapped_physical_pages_.size() &&
           mapped_physical_pages_[guest_page] == physical_page;
  }

  [[nodiscard]] bool page_mapped(std::uint32_t guest_page) const noexcept {
    return active() && guest_page < mapped_physical_pages_.size() &&
           mapped_physical_pages_[guest_page] != kInvalidPage;
  }

  [[nodiscard]] bool can_map_page(std::uint32_t guest_page) const noexcept {
    if (!active() || guest_page >= kPageCount) return false;
    if (!host_vm::fixed_shared_mapping_requires_page_views()) return true;

    // Windows placeholder replacement must create one section view per 4 KiB
    // mapping. Keep the large permanent physical alias windows on compact
    // translation there rather than materializing hundreds of thousands of
    // view objects. Ordinary virtual/XEX pages can still use the aperture.
    const auto address = guest_page << kPageShift;
    return !(address >= kGpuWritebackBase && address <= kGpuWritebackEnd) &&
           !(address >= kPhysical64KBase && address <= kPhysical16MEnd) &&
           !(address >= kPhysical4KBase && address <= kPhysical4KHeapEnd);
  }

  bool map_run(std::uint32_t guest_page, std::uint32_t physical_page,
               std::uint32_t page_count) noexcept {
    if (!active() || !page_count || guest_page >= kPageCount ||
        physical_page >= kPhysicalPageCount ||
        std::uint64_t(guest_page) + page_count > kPageCount ||
        std::uint64_t(physical_page) + page_count > kPhysicalPageCount) {
      return false;
    }
    for (std::uint32_t i = 0; i < page_count; ++i) {
      if (!can_map_page(guest_page + i)) return false;
    }

    auto* target = base_ + std::size_t(guest_page) * kBasePageSize;
    const auto offset = std::size_t(physical_page) * kBasePageSize;
    const auto size = std::size_t(page_count) * kBasePageSize;

    if (!host_vm::fixed_shared_mapping_requires_page_views()) {
      if (!host_vm::map_shared_fixed(*shared_, target, offset, size,
                                     host_vm::Protection::ReadWrite)) {
        return false;
      }
    } else {
      std::uint32_t mapped = 0u;
      for (; mapped < page_count; ++mapped) {
        if (!host_vm::map_shared_fixed(
                *shared_, target + std::size_t(mapped) * kBasePageSize,
                offset + std::size_t(mapped) * kBasePageSize, kBasePageSize,
                host_vm::Protection::ReadWrite)) {
          break;
        }
      }
      if (mapped != page_count) {
        if (mapped) {
          (void)host_vm::restore_reservation(
              target, std::size_t(mapped) * kBasePageSize);
        }
        return false;
      }
    }

    for (std::uint32_t i = 0; i < page_count; ++i) {
      mapped_physical_pages_[guest_page + i] = physical_page + i;
    }
    return true;
  }

  bool clear_run(std::uint32_t guest_page,
                 std::uint32_t page_count) noexcept {
    if (!active() || !page_count || guest_page >= kPageCount ||
        std::uint64_t(guest_page) + page_count > kPageCount) {
      return false;
    }
    auto* target = base_ + std::size_t(guest_page) * kBasePageSize;
    const auto size = std::size_t(page_count) * kBasePageSize;
    if (!host_vm::restore_reservation(target, size)) return false;
    std::fill_n(mapped_physical_pages_.begin() + guest_page, page_count,
                kInvalidPage);
    return true;
  }

  bool reset() noexcept {
    if (!active()) return false;
    if (!host_vm::restore_reservation(base_, static_cast<std::size_t>(kSize))) {
      return false;
    }
    std::fill(mapped_physical_pages_.begin(), mapped_physical_pages_.end(),
              kInvalidPage);

    const auto map_static = [&](GuestAddress guest_base,
                                std::uint32_t physical_base,
                                std::uint32_t size) noexcept {
      return map_run(guest_base >> kPageShift,
                     physical_base >> kPageShift,
                     size >> kPageShift);
    };

    if (host_vm::fixed_shared_mapping_requires_page_views()) {
      // See can_map_page(): Windows keeps the permanent physical aliases on
      // compact translation to avoid a view object per 4 KiB alias page.
      return true;
    }

    const auto gpu_size = kGpuWritebackEnd - kGpuWritebackBase + 1u;
    const auto physical64k_size = kPhysical64KEnd - kPhysical64KBase + 1u;
    const auto physical16m_size = kPhysical16MEnd - kPhysical16MBase + 1u;
    const auto physical4k_region_size =
        kPhysical4KHeapEnd - kPhysical4KBase + 1u;
    const auto physical4k_size = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        physical4k_region_size,
        std::uint64_t{kPhysicalMemorySize} - kPhysical4KViewOffset));

    return map_static(kGpuWritebackBase, 0u, gpu_size) &&
           map_static(kPhysical64KBase, 0u,
                      std::min(physical64k_size, kPhysicalMemorySize)) &&
           map_static(kPhysical16MBase, 0u,
                      std::min(physical16m_size, kPhysicalMemorySize)) &&
           map_static(kPhysical4KBase, kPhysical4KViewOffset,
                      physical4k_size);
  }

 private:
  std::byte* base_{};
  const host_vm::SharedMemory* shared_{};
  std::vector<std::uint32_t> mapped_physical_pages_{};
};

}  // namespace xenon::memory

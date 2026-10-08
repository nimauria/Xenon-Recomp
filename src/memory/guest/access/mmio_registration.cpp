#include "memory/guest/address_space_internal.hpp"

namespace xenon::memory {

bool AddressSpace::add_mmio_range(GuestAddress base, std::uint32_t size,
                                  MmioRead read, MmioWrite write,
                                  std::string name) {
  std::lock_guard lock(mutex_);
  if (!size || std::uint64_t{base} + size >
                   std::uint64_t{std::numeric_limits<GuestAddress>::max()} +
                       1u) {
    return false;
  }

  const auto insert_at = std::lower_bound(
      mmio_ranges_.begin(), mmio_ranges_.end(), base,
      [](const MmioRangeRef& range, GuestAddress value) {
        return range->base < value;
      });
  if (insert_at != mmio_ranges_.end() &&
      overlaps(base, size, (*insert_at)->base, (*insert_at)->size)) {
    return false;
  }
  if (insert_at != mmio_ranges_.begin()) {
    const auto& previous = *std::prev(insert_at);
    if (overlaps(base, size, previous->base, previous->size)) return false;
  }

  auto range = std::make_shared<MmioRange>(
      MmioRange{base, size, std::move(read), std::move(write), std::move(name)});
  mmio_ranges_.insert(insert_at, std::move(range));
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

}  // namespace xenon::memory

#include "xenon/xam/guest_enumerator.hpp"

namespace xenon::xam {

GuestEnumeratorObject::GuestEnumeratorObject(std::uint32_t item_size,
                                             std::uint32_t items_per_enumerate,
                                             std::vector<std::vector<std::byte>> items)
    : KernelObject(kernel::ObjectType::Enumerator),
      item_size_(item_size),
      items_per_enumerate_(items_per_enumerate == 0u ? 1u : items_per_enumerate),
      items_(std::move(items)) {}

std::size_t GuestEnumeratorObject::next(std::vector<std::byte>& out) {
  out.clear();
  std::size_t copied = 0;
  while (copied < items_per_enumerate_ && cursor_ < items_.size()) {
    const auto& item = items_[cursor_++];
    out.insert(out.end(), item.begin(), item.end());
    ++copied;
  }
  return copied;
}

}  // namespace xenon::xam

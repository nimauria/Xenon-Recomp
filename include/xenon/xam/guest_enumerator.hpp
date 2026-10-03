#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "xenon/kernel/object.hpp"

namespace xenon::xam {

// Kernel object backing an XAM enumerator handle (XamContentCreateEnumerator,
// XamUserCreate{Achievement,Stats}Enumerator, ...). Real Xbox 360 XAM backs
// every one of these with a genuine kernel object (xenia's XStaticEnumerator<T>)
// closed through the ordinary NtClose/CloseHandle path - not a bespoke
// XAM-side close call - so this is a real kernel::KernelObject rather than a
// private lookup table, and gets correct close-on-NtClose behavior for free
// from kernel::HandleTable::close(), exactly like Event/Semaphore/Mutant.
//
// Items are pre-serialized into the exact on-guest byte layout the producer's
// struct uses (XCONTENT_DATA, etc.) at creation time, so XamEnumerate can stay
// completely producer-agnostic: it only ever copies raw bytes.
class GuestEnumeratorObject : public kernel::KernelObject {
 public:
  GuestEnumeratorObject(std::uint32_t item_size, std::uint32_t items_per_enumerate,
                        std::vector<std::vector<std::byte>> items);

  [[nodiscard]] std::uint32_t item_size() const noexcept { return item_size_; }
  [[nodiscard]] std::uint32_t items_per_enumerate() const noexcept {
    return items_per_enumerate_;
  }
  [[nodiscard]] std::size_t remaining() const noexcept { return items_.size() - cursor_; }

  // Copies the next batch (bounded by items_per_enumerate(), matching real
  // XStaticUntypedEnumerator::WriteItems()'s per-call cap) into `out`,
  // advancing the cursor. Returns the number of items copied; 0 means no
  // items remained (caller reports X_ERROR_NO_MORE_FILES).
  std::size_t next(std::vector<std::byte>& out);

 private:
  std::uint32_t item_size_;
  std::uint32_t items_per_enumerate_;
  std::vector<std::vector<std::byte>> items_;
  std::size_t cursor_{0};
};

}  // namespace xenon::xam

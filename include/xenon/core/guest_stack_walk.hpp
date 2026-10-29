#pragma once

// Diagnostic walk of an Xbox 360 guest stack, used by fault reports.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "xenon/memory/address_space.hpp"

namespace xenon::core {

struct GuestStackFrame {
  std::uint32_t frame{};           // this frame's stack pointer (r1 after its stwu)
  std::uint32_t return_address{};  // where this frame's function returns to
};

// Follows the back chain from `stack_pointer`. Xbox 360 titles use the MSVC
// PowerPC convention: a prologue saves LR at -8 from the CALLER's stack
// pointer (`mflr r12; stw r12,-8(r1)`, often inside __savegprlr) before
// `stwu r1,-size(r1)` stores that caller pointer at 0(r1). So a frame F
// returns to the word at [F] - 8, not [F + 8] as in the SysV/PPC64 ABIs.
// Read-only and best effort: it stops at an unreadable word, a back chain
// that does not strictly ascend, or `max_frames`, and never faults.
[[nodiscard]] std::vector<GuestStackFrame> walk_guest_stack(memory::AddressSpace& memory,
                                                            std::uint32_t stack_pointer,
                                                            std::size_t max_frames = 16u);

}  // namespace xenon::core

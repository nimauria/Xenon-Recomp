// walk_guest_stack(): the Xbox 360 back-chain walk fault reports use.
//
// Regression: the fault report read each return address at frame+8 (the
// SysV/PPC64 slot), so AC6's first fault printed stack=[0x2,0x0,0x0,...]
// instead of its callers. Xbox 360 prologues save LR at -8 from the caller's
// stack pointer: `mflr r12; stw r12,-8(r1); stwu r1,-size(r1)`.

#include <cassert>
#include <cstdint>
#include <iostream>

#include "xenon/core/guest_stack_walk.hpp"
#include "xenon/memory/address_space.hpp"

using namespace xenon;

namespace {

// Builds the frames of `outer -> middle -> inner` the way their prologues
// would, with distinguishable junk in the frame+8 slots.
void build_three_frames(memory::AddressSpace& memory, memory::GuestAddress top) {
  const std::uint32_t outer = top - 0x100u;
  const std::uint32_t middle = outer - 0x90u;
  const std::uint32_t inner = middle - 0xB0u;
  memory.write32_be(top - 8u, 0x82000010u);     // outer's return address, in its caller's frame
  memory.write32_be(outer, top);                // outer's back chain
  memory.write32_be(outer - 8u, 0x82000020u);   // middle returns into outer
  memory.write32_be(middle, outer);
  memory.write32_be(inner - 0u, middle);
  memory.write32_be(middle - 8u, 0x82000030u);  // inner returns into middle
  memory.write32_be(top, 0u);                   // end of chain
  for (const auto frame : {outer, middle, inner}) memory.write32_be(frame + 8u, 0x2u);
}

}  // namespace

int main() {
  memory::AddressSpace memory(memory::GuestTranslationMode::Compact);
  assert(memory.initialize());
  memory::GuestAddress stack{};
  assert(memory.allocate(0x4000u, 0x1000u, memory::kReadWrite, false, stack));
  const memory::GuestAddress top = stack + 0x3000u;
  build_three_frames(memory, top);
  const std::uint32_t inner = top - 0x100u - 0x90u - 0xB0u;

  const auto frames = core::walk_guest_stack(memory, inner);
  assert(frames.size() == 3u);
  assert(frames[0].frame == inner && frames[0].return_address == 0x82000030u);
  assert(frames[1].return_address == 0x82000020u);
  assert(frames[2].return_address == 0x82000010u);
  for (const auto& frame : frames)
    assert(frame.return_address != 0x2u && "the frame+8 slot is not the Xbox 360 LR slot");

  // A back chain that does not ascend, or an unmapped stack, ends the walk
  // without faulting.
  memory.write32_be(inner, inner);
  assert(core::walk_guest_stack(memory, inner).empty());
  assert(core::walk_guest_stack(memory, 0x00001000u).empty());
  assert(core::walk_guest_stack(memory, inner, 0u).empty());

  std::cout << "Guest stack walk tests passed\n";
  return 0;
}

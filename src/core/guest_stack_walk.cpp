#include "xenon/core/guest_stack_walk.hpp"

namespace xenon::core {

std::vector<GuestStackFrame> walk_guest_stack(memory::AddressSpace& memory,
                                              std::uint32_t stack_pointer,
                                              std::size_t max_frames) {
  std::vector<GuestStackFrame> frames;
  auto frame = stack_pointer;
  while (frame != 0u && frames.size() < max_frames) {
    std::uint32_t caller_frame = 0u;
    std::uint32_t return_address = 0u;
    try {
      caller_frame = memory.read32_be(frame);
      if (caller_frame <= frame) break;  // the chain must strictly ascend
      return_address = memory.read32_be(caller_frame - 8u);
    } catch (const memory::MemoryFault&) {
      break;
    }
    frames.push_back({frame, return_address});
    frame = caller_frame;
  }
  return frames;
}

}  // namespace xenon::core

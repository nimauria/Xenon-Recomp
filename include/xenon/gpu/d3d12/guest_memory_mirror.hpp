#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xenon/gpu/d3d12/command_queue.hpp"
#include "xenon/gpu/d3d12/memory.hpp"
#include "xenon/memory/address_space.hpp"

namespace xenon::gpu::d3d12 {

class GuestMemoryMirror {
 public:
  GuestMemoryMirror() = default;
  ~GuestMemoryMirror();
  GuestMemoryMirror(const GuestMemoryMirror&) = delete;
  GuestMemoryMirror& operator=(const GuestMemoryMirror&) = delete;

  [[nodiscard]] bool initialize(ID3D12Device* device, CommandQueue& queue,
                                memory::AddressSpace& memory);
  void reset() noexcept;
  [[nodiscard]] bool synchronize();
  [[nodiscard]] ID3D12Resource* resource() const noexcept {
    return mirror_.resource();
  }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  static constexpr std::uint32_t kUploadSize = 16u * 1024u * 1024u;

  memory::AddressSpace* memory_{};
  CommandQueue* queue_{};
  Buffer mirror_{};
  Buffer upload_{};
  std::uint64_t synchronized_epoch_{};
  std::vector<memory::DirtyPhysicalRange> dirty_ranges_{};
  bool generic_read_state_{};
  std::string error_{};
};

}  // namespace xenon::gpu::d3d12

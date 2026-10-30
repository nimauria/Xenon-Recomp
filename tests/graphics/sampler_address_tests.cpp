// Xenos texture clamp modes and border colours in host terms
// (xenon/gpu/sampler_address.hpp), shared by the D3D12 and Vulkan backends.

#include <cassert>
#include <cstdint>
#include <iostream>

#include "xenon/gpu/sampler_address.hpp"

namespace gpu = xenon::gpu;

namespace {

gpu::SamplerAddress map(std::uint8_t clamp) {
  gpu::SamplerAddress address{};
  assert(gpu::xenos_sampler_address(clamp, address));
  return address;
}

// Regression: only clamp modes 0-3 were mapped. The field is 3 bits, and AC6's
// menus use the halfway and border modes; every one fell to a black border.
void test_every_xenos_clamp_mode_maps() {
  assert(map(0) == gpu::SamplerAddress::Wrap);
  assert(map(1) == gpu::SamplerAddress::Mirror);
  assert(map(2) == gpu::SamplerAddress::ClampToEdge);
  assert(map(3) == gpu::SamplerAddress::MirrorOnceToEdge);
  assert(map(4) == gpu::SamplerAddress::ClampToEdge);
  assert(map(5) == gpu::SamplerAddress::MirrorOnceToEdge);
  assert(map(6) == gpu::SamplerAddress::ClampToBorder);
  assert(map(7) == gpu::SamplerAddress::MirrorOnceToEdge);
  gpu::SamplerAddress unused{};
  assert(!gpu::xenos_sampler_address(8, unused));
}

void test_border_colours() {
  assert(!gpu::xenos_border_is_white(0));  // ABGR black
  assert(gpu::xenos_border_is_white(1));   // ABGR white
  assert(!gpu::xenos_border_is_white(2));  // ACBYCR black
  assert(!gpu::xenos_border_is_white(3));  // ACBCRY black
}

}  // namespace

int main() {
  test_every_xenos_clamp_mode_maps();
  test_border_colours();
  std::cout << "Sampler address tests passed\n";
  return 0;
}

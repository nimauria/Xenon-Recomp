// GPU scan-out against the CPU reference: the scan-out shader reads the front
// buffer from the guest-memory mirror, untiles, byte-swaps, converts,
// swizzles and Catmull-Rom filters it on the GPU. For the same guest bytes it
// must produce what prepare_presentation_frame() computes on the CPU.

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xenon/gpu/backend_capabilities.hpp"
#include "xenon/gpu/edram.hpp"
#include "xenon/gpu/presentation.hpp"
#include "xenon/gpu/texture.hpp"
#include "xenon/memory/address_space.hpp"

#if defined(XENON_TEST_VULKAN)
#include "xenon/gpu/vulkan/backend.hpp"
#include "../../support/vulkan_probe.hpp"
#endif
#if defined(XENON_TEST_D3D12)
#include "xenon/gpu/d3d12/backend.hpp"
#endif

namespace {

using namespace xenon::gpu;

struct Case {
  std::string_view name;
  std::uint8_t format;
  std::uint32_t width;
  std::uint32_t height;
  bool tiled;
  Endian endian;
  std::uint16_t swizzle;
  std::uint32_t target_width;
  std::uint32_t target_height;
};

std::uint16_t half(float value) {
  // Values used here are exact in half precision, or special.
  if (value != value) return 0x7E00u;
  if (value > 65504.0f) return 0x7C00u;
  if (value == 0.0f) return 0u;
  int exponent = 0;
  float mantissa = value;
  while (mantissa >= 2.0f) { mantissa /= 2.0f; ++exponent; }
  while (mantissa < 1.0f) { mantissa *= 2.0f; --exponent; }
  return static_cast<std::uint16_t>(((exponent + 15) << 10) |
                                    static_cast<std::uint16_t>((mantissa - 1.0f) * 1024.0f));
}

std::vector<std::byte> make_linear(const Case& c, const TextureLayout& layout) {
  const auto bpb = layout.format.bytes_per_block();
  std::vector<std::byte> linear(std::size_t(c.width) * c.height * bpb);
  std::uint32_t state = 0x1234567u ^ c.format;
  for (std::uint32_t y = 0; y < c.height; ++y) {
    for (std::uint32_t x = 0; x < c.width; ++x) {
      auto* texel = linear.data() + (std::size_t(y) * c.width + x) * bpb;
      if (c.format == 32) {
        // 16_16_16_16_FLOAT: a gradient, values above 1, a NaN and an infinity.
        const float values[4] = {float(x) / float(c.width), float(y) / float(c.height),
                                 (x + y) % 7 == 0 ? 3.0f : 0.25f,
                                 x == 3 && y == 2 ? 1e9f : (x == 5 ? std::numeric_limits<float>::quiet_NaN() : 1.0f)};
        for (int i = 0; i < 4; ++i) {
          const auto h = half(values[i]);
          std::memcpy(texel + i * 2, &h, 2);
        }
        continue;
      }
      // Smooth content with some hard edges, so the filter has both.
      for (std::uint32_t i = 0; i < bpb; ++i) {
        state = state * 1664525u + 1013904223u;
        const auto smooth = static_cast<std::uint8_t>((x * 9u + y * 5u + i * 40u) & 0xFFu);
        texel[i] = std::byte(((x / 8u + y / 6u) % 3u == 0u) ? std::uint8_t(state >> 24) : smooth);
      }
    }
  }
  return linear;
}

void run_case(auto& backend, std::string_view backend_name, xenon::memory::AddressSpace& memory,
              Edram& edram, const Case& c) {
  constexpr std::uint32_t kBase = 0x00400000u;
  TextureDescriptor descriptor{};
  descriptor.base_address = kBase;
  descriptor.width = c.width;
  descriptor.height = c.height;
  descriptor.pitch = c.width;
  descriptor.format = c.format;
  descriptor.endian = c.endian;
  descriptor.swizzle = c.swizzle;
  descriptor.dimension = TextureDimension::TwoDOrStacked;
  descriptor.tiled = c.tiled;
  descriptor.valid = true;
  const auto layout = build_texture_layout(descriptor);
  assert(layout.valid);

  std::vector<std::byte> physical(kBase + layout.base_extent_bytes);
  const auto linear = make_linear(c, layout);
  std::string error;
  assert(encode_texture(descriptor, linear, physical, &error));
  const auto guest = std::span<const std::byte>(physical).subspan(kBase);

  // One guest memory for every case, as a session has: each case's CPU writes
  // must reach the GPU through the mirror's dirty tracking.
  assert(memory.write_physical(kBase, guest));
  backend.begin_submission(memory, edram);

  PresentationFrame frame{};
  frame.texture = descriptor;
  const auto expected =
      prepare_presentation_frame(frame, guest, c.target_width, c.target_height, true, kBase);
  assert(expected.valid);
  PreparedPresentationFrame actual{};
  if (!backend.capture_scanout(frame, c.target_width, c.target_height, actual)) {
    std::cerr << backend_name << ' ' << c.name << ": capture failed: " << backend.last_error()
              << '\n';
    assert(false);
  }
  backend.end_submission();
  assert(actual.valid && actual.width == expected.width && actual.height == expected.height);
  assert(actual.rgba8.size() == expected.rgba8.size());

  int worst = 0;
  std::size_t worst_index = 0;
  for (std::size_t i = 0; i < actual.rgba8.size(); ++i) {
    const int difference = std::abs(int(actual.rgba8[i]) - int(expected.rgba8[i]));
    if (difference > worst) {
      worst = difference;
      worst_index = i;
    }
  }
  // Float evaluation order may differ by an ulp on the GPU; a result that
  // rounds the other way is one step off. Anything more is a real mismatch.
  if (worst > 1) {
    const auto pixel = worst_index / 4u;
    std::cerr << backend_name << ' ' << c.name << ": pixel (" << pixel % actual.width << ','
              << pixel / actual.width << ") channel " << worst_index % 4u << " is "
              << int(actual.rgba8[worst_index]) << ", CPU reference "
              << int(expected.rgba8[worst_index]) << '\n';
    assert(false);
  }
  std::cout << backend_name << ' ' << c.name << ": matches the CPU reference (max diff "
            << worst << ")\n";
}

void run_all(auto& backend, std::string_view name) {
  const Case cases[] = {
      // AC6's front buffer: tiled k_8_8_8_8, 8-in-32 swap, red/blue swizzle,
      // upscaled 1.5x as 720p to 1080p is.
      {"8_8_8_8 tiled 1.5x", 6, 64, 40, true, Endian::Swap8In32, 0x60A, 96, 60},
      {"8_8_8_8 letterboxed", 6, 64, 40, true, Endian::Swap8In32, 0x688, 100, 100},
      {"2_10_10_10 linear downscale", 7, 64, 32, false, Endian::Swap8In32, 0x688, 40, 20},
      {"5_6_5 tiled 1:1", 4, 32, 32, true, Endian::Swap8In16, 0x688, 32, 32},
      {"16_16_16_16_FLOAT tiled 2x", 32, 32, 32, true, Endian::Swap8In32, 0x688, 64, 64},
      {"8 linear .x011", 2, 40, 24, false, Endian::None, 0xB60, 60, 36},
  };
  xenon::memory::AddressSpace memory;
  assert(memory.initialize());
  Edram edram;
  edram.reset();
  for (const auto& c : cases) run_case(backend, name, memory, edram, c);
}

}  // namespace

int main() {
#if defined(XENON_TEST_VULKAN)
  if (!xenon::test::vulkan_device_present()) {
    std::cout << "Vulkan: no Vulkan device on this machine; skipped\n";
  } else {
    xenon::gpu::vulkan::Backend backend;
    if (!backend.initialize({.enable_validation = false})) {
      std::cerr << "Vulkan backend initialization failed: " << backend.error() << '\n';
      assert(false);
    }
    run_all(backend, "Vulkan");
  }
#endif

#if defined(XENON_TEST_D3D12)
  if (!xenon::gpu::discover_backend_capabilities()[1].runtime_available) {
    std::cout << "D3D12: no hardware adapter on this machine; skipped\n";
  } else {
    xenon::gpu::d3d12::Backend backend;
    if (!backend.initialize({.enable_debug_layer = false, .allow_software_adapter = false})) {
      std::cerr << "D3D12 backend initialization failed: " << backend.error() << '\n';
      assert(false);
    }
    run_all(backend, "D3D12");
  }
#endif
  return 0;
}

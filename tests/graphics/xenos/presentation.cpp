#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include "xenon/gpu/presentation.hpp"
#include "xenon/gpu/texture.hpp"

using namespace xenon::gpu;

namespace {
TextureDescriptor rgba8_descriptor(std::uint32_t base, std::uint32_t width,
                                   std::uint32_t height, bool tiled) {
  TextureDescriptor d{};
  d.base_address = base;
  d.width = width;
  d.height = height;
  d.depth = 1;
  d.pitch = width;
  d.format = 6;  // Xenos 8_8_8_8.
  d.dimension = TextureDimension::TwoDOrStacked;
  d.mip_min_level = 0;
  d.mip_max_level = 0;
  d.tiled = tiled;
  d.valid = true;
  return d;
}

void test_rgba8_scale_and_letterbox(bool tiled) {
  constexpr std::uint32_t base = 0x1000;
  auto descriptor = rgba8_descriptor(base, 2, 1, tiled);
  const auto layout = build_texture_layout(descriptor);
  assert(layout.valid);
  std::vector<std::byte> memory(base + layout.base_extent_bytes + 0x1000);
  const std::uint8_t pixels[] = {
      255, 0, 0, 255,  // red
      0, 0, 255, 255,  // blue
  };
  std::vector<std::byte> linear(sizeof(pixels));
  std::memcpy(linear.data(), pixels, sizeof(pixels));
  std::string error;
  assert(encode_texture(descriptor, linear, memory, &error));

  PresentationFrame frame{};
  frame.texture = descriptor;
  const auto prepared = prepare_presentation_frame(frame, memory, 4, 4, true);
  assert(prepared.valid);
  const auto snapshot = std::span<const std::byte>(memory).subspan(
      base, static_cast<std::size_t>(layout.base_extent_bytes));
  const auto prepared_snapshot =
      prepare_presentation_frame(frame, snapshot, 4, 4, true, base);
  assert(prepared_snapshot.valid);
  assert(prepared_snapshot.rgba8 == prepared.rgba8);
  assert(prepared.width == 4 && prepared.height == 4);
  assert(prepared.row_pitch == 16);
  // 2:1 source inside a square target -> one black row above and below.
  for (std::uint32_t x = 0; x < 4; ++x) {
    const auto* top = reinterpret_cast<const std::uint8_t*>(
        prepared.rgba8.data() + x * 4u);
    const auto* bottom = reinterpret_cast<const std::uint8_t*>(
        prepared.rgba8.data() + 3u * prepared.row_pitch + x * 4u);
    assert(top[0] == 0 && top[1] == 0 && top[2] == 0 && top[3] == 0);
    assert(bottom[0] == 0 && bottom[1] == 0 && bottom[2] == 0 && bottom[3] == 0);
  }
  const auto* left = reinterpret_cast<const std::uint8_t*>(
      prepared.rgba8.data() + prepared.row_pitch);
  const auto* right = reinterpret_cast<const std::uint8_t*>(
      prepared.rgba8.data() + prepared.row_pitch + 3u * 4u);
  assert(left[0] > left[2]);
  assert(right[2] > right[0]);
  assert(left[3] == 255 && right[3] == 255);
}

void test_visible_crop_and_stretch() {
  constexpr std::uint32_t base = 0x2000;
  auto descriptor = rgba8_descriptor(base, 4, 1, false);
  const auto layout = build_texture_layout(descriptor);
  std::vector<std::byte> memory(base + layout.base_extent_bytes + 0x1000);
  const std::uint8_t pixels[] = {
      10, 20, 30, 255, 40, 50, 60, 255,
      70, 80, 90, 255, 100, 110, 120, 255,
  };
  std::vector<std::byte> linear(sizeof(pixels));
  std::memcpy(linear.data(), pixels, sizeof(pixels));
  assert(encode_texture(descriptor, linear, memory));
  PresentationFrame frame{};
  frame.texture = descriptor;
  frame.visible_width = 2;
  frame.visible_height = 1;
  const auto prepared = prepare_presentation_frame(frame, memory, 2, 2, false);
  assert(prepared.valid);
  const auto* first = reinterpret_cast<const std::uint8_t*>(prepared.rgba8.data());
  assert(first[0] == 10 && first[1] == 20 && first[2] == 30 && first[3] == 255);
  const auto* last = reinterpret_cast<const std::uint8_t*>(
      prepared.rgba8.data() + prepared.row_pitch + 4u);
  assert(last[0] == 40 && last[1] == 50 && last[2] == 60 && last[3] == 255);
}

void test_565_conversion() {
  constexpr std::uint32_t base = 0x3000;
  TextureDescriptor descriptor{};
  descriptor.base_address = base;
  descriptor.width = descriptor.height = descriptor.depth = 1;
  descriptor.pitch = 1;
  descriptor.format = 4;  // 5_6_5 -> B5G6R5 host storage.
  descriptor.dimension = TextureDimension::TwoDOrStacked;
  descriptor.valid = true;
  const auto layout = build_texture_layout(descriptor);
  std::vector<std::byte> memory(base + layout.base_extent_bytes + 0x1000);
  // R=31, G=0, B=0 in the host-compatible packed representation.
  const std::uint16_t red = 0xF800u;
  std::vector<std::byte> linear(sizeof(red));
  std::memcpy(linear.data(), &red, sizeof(red));
  assert(encode_texture(descriptor, linear, memory));
  PresentationFrame frame{};
  frame.texture = descriptor;
  const auto prepared = prepare_presentation_frame(frame, memory, 1, 1, true);
  assert(prepared.valid);
  const auto* pixel = reinterpret_cast<const std::uint8_t*>(prepared.rgba8.data());
  assert(pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255);
}
}  // namespace

int main() {
  test_rgba8_scale_and_letterbox(false);
  test_rgba8_scale_and_letterbox(true);
  test_visible_crop_and_stretch();
  test_565_conversion();
  std::cout << "presentation tests passed\n";
  return 0;
}

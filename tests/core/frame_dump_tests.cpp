#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

#include "xenon/core/frame_dump.hpp"

namespace {
std::uint32_t read32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return bytes[offset] | (bytes[offset + 1] << 8u) | (bytes[offset + 2] << 16u) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24u);
}
}  // namespace

int main() {
  // 2x2 RGBA8 with a padded row pitch: red, green / blue, white.
  xenon::gpu::PreparedPresentationFrame frame{};
  frame.width = 2;
  frame.height = 2;
  frame.row_pitch = 12;
  frame.valid = true;
  const std::uint8_t pixels[] = {255, 0, 0, 255, 0, 255, 0, 255, 9, 9, 9, 9,
                                 0, 0, 255, 255, 255, 255, 255, 255, 9, 9, 9, 9};
  for (auto value : pixels) frame.rgba8.push_back(static_cast<std::byte>(value));

  const auto path = std::filesystem::temp_directory_path() / "xenon_frame_dump_test.bmp";
  assert(xenon::core::write_frame_bmp(path, frame));
  std::ifstream file(path, std::ios::binary);
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                        std::istreambuf_iterator<char>());
  file.close();
  std::filesystem::remove(path);

  assert(bytes.size() == 54u + 16u);
  assert(bytes[0] == 'B' && bytes[1] == 'M');
  assert(read32(bytes, 2) == 70u && read32(bytes, 10) == 54u);
  assert(read32(bytes, 18) == 2u);
  assert(read32(bytes, 22) == static_cast<std::uint32_t>(-2));  // top-down rows
  assert(bytes[28] == 32u);
  // BGRA, first row first, row padding dropped.
  const std::uint8_t expected[] = {0, 0, 255, 255, 0, 255, 0, 255,
                                   255, 0, 0, 255, 255, 255, 255, 255};
  for (std::size_t i = 0; i < sizeof(expected); ++i) assert(bytes[54 + i] == expected[i]);

  // A capture that failed is not written.
  frame.valid = false;
  assert(!xenon::core::write_frame_bmp(path, frame));
  assert(!std::filesystem::exists(path));
  std::cout << "frame dump tests passed\n";
  return 0;
}

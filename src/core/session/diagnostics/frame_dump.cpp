#include "xenon/core/frame_dump.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <vector>

namespace xenon::core {
namespace {

void put16(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value));
  out.push_back(static_cast<std::uint8_t>(value >> 8u));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  put16(out, value);
  put16(out, value >> 16u);
}

}  // namespace

bool write_frame_bmp(const std::filesystem::path& path, const gpu::PreparedPresentationFrame& frame) {
  const auto width = frame.width;
  const auto height = frame.height;
  const auto pitch = frame.row_pitch ? frame.row_pitch : width * 4u;
  if (!frame.valid || width == 0u || height == 0u || pitch < width * 4u ||
      frame.rgba8.size() < static_cast<std::size_t>(pitch) * height) {
    return false;
  }
  constexpr std::uint32_t kHeaderBytes = 14u + 40u;
  const auto pixel_bytes = width * height * 4u;
  std::vector<std::uint8_t> out;
  out.reserve(kHeaderBytes + pixel_bytes);
  out.push_back('B');
  out.push_back('M');
  put32(out, kHeaderBytes + pixel_bytes);
  put32(out, 0u);
  put32(out, kHeaderBytes);
  put32(out, 40u);  // BITMAPINFOHEADER
  put32(out, width);
  put32(out, static_cast<std::uint32_t>(-static_cast<std::int32_t>(height)));  // top-down
  put16(out, 1u);
  put16(out, 32u);
  put32(out, 0u);  // BI_RGB
  put32(out, pixel_bytes);
  put32(out, 2835u);
  put32(out, 2835u);
  put32(out, 0u);
  put32(out, 0u);
  for (std::uint32_t y = 0; y < height; ++y) {
    const auto* row = frame.rgba8.data() + static_cast<std::size_t>(y) * pitch;
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto* pixel = row + x * 4u;
      out.push_back(std::to_integer<std::uint8_t>(pixel[2]));
      out.push_back(std::to_integer<std::uint8_t>(pixel[1]));
      out.push_back(std::to_integer<std::uint8_t>(pixel[0]));
      out.push_back(std::to_integer<std::uint8_t>(pixel[3]));
    }
  }
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) return false;
  file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
  return static_cast<bool>(file);
}

}  // namespace xenon::core

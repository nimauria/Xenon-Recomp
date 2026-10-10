// XMA packet headers and the 64-byte hardware context layout.

#include "xenon/audio/xma.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace xenon::audio {
namespace {

[[nodiscard]] std::uint32_t load_be32(const std::byte* p) noexcept {
  return (std::uint32_t(std::to_integer<std::uint8_t>(p[0])) << 24u) |
         (std::uint32_t(std::to_integer<std::uint8_t>(p[1])) << 16u) |
         (std::uint32_t(std::to_integer<std::uint8_t>(p[2])) << 8u) |
         std::uint32_t(std::to_integer<std::uint8_t>(p[3]));
}

void store_be32(std::byte* p, std::uint32_t v) noexcept {
  p[0] = std::byte((v >> 24u) & 0xFFu);
  p[1] = std::byte((v >> 16u) & 0xFFu);
  p[2] = std::byte((v >> 8u) & 0xFFu);
  p[3] = std::byte(v & 0xFFu);
}

}  // namespace

XmaPacketHeader parse_xma_packet_header(
    std::span<const std::byte> packet) noexcept {
  XmaPacketHeader out{};
  if (packet.size() < 4) return out;
  const auto b0 = std::to_integer<std::uint8_t>(packet[0]);
  const auto b1 = std::to_integer<std::uint8_t>(packet[1]);
  const auto b2 = std::to_integer<std::uint8_t>(packet[2]);
  out.frame_count = static_cast<std::uint8_t>(b0 >> 2u);
  out.first_frame_offset_bits = static_cast<std::uint16_t>(
      (((b0 & 0x3u) << 13u) | (std::uint16_t(b1) << 5u) | (b2 >> 3u)) + 32u);
  out.metadata = static_cast<std::uint8_t>(b2 & 0x7u);
  out.skip_count = std::to_integer<std::uint8_t>(packet[3]);
  return out;
}

XmaContextData XmaContextData::decode(
    std::span<const std::byte, kXmaContextBytes> bytes) noexcept {
  std::array<std::uint32_t, 16> w{};
  for (std::size_t i = 0; i < w.size(); ++i) w[i] = load_be32(bytes.data() + i * 4u);

  XmaContextData d{};
  d.input_buffer_0_packet_count = static_cast<std::uint16_t>(w[0] & 0xFFFu);
  d.loop_count = static_cast<std::uint8_t>((w[0] >> 12u) & 0xFFu);
  d.input_buffer_0_valid = ((w[0] >> 20u) & 1u) != 0;
  d.input_buffer_1_valid = ((w[0] >> 21u) & 1u) != 0;
  d.output_buffer_block_count = static_cast<std::uint8_t>((w[0] >> 22u) & 0x1Fu);
  d.output_buffer_write_offset = static_cast<std::uint8_t>((w[0] >> 27u) & 0x1Fu);

  d.input_buffer_1_packet_count = static_cast<std::uint16_t>(w[1] & 0xFFFu);
  d.loop_subframe_end = static_cast<std::uint8_t>((w[1] >> 12u) & 0x3u);
  d.reserved_dword_1_a = static_cast<std::uint8_t>((w[1] >> 14u) & 0x7u);
  d.loop_subframe_skip = static_cast<std::uint8_t>((w[1] >> 17u) & 0x7u);
  d.subframe_decode_count = static_cast<std::uint8_t>((w[1] >> 20u) & 0xFu);
  d.output_buffer_padding = static_cast<std::uint8_t>((w[1] >> 24u) & 0x7u);
  d.sample_rate = static_cast<std::uint8_t>((w[1] >> 27u) & 0x3u);
  d.is_stereo = ((w[1] >> 29u) & 1u) != 0;
  d.output_buffer_valid = ((w[1] >> 31u) & 1u) != 0;

  d.input_buffer_read_offset = w[2] & 0x03FFFFFFu;
  d.error_status = static_cast<std::uint8_t>((w[2] >> 26u) & 0x1Fu);
  d.error_set = ((w[2] >> 31u) & 1u) != 0;
  d.loop_start = w[3] & 0x03FFFFFFu;
  d.parser_error_status = static_cast<std::uint8_t>((w[3] >> 26u) & 0x1Fu);
  d.parser_error_set = ((w[3] >> 31u) & 1u) != 0;
  d.loop_end = w[4] & 0x03FFFFFFu;
  d.packet_metadata = static_cast<std::uint8_t>((w[4] >> 26u) & 0x1Fu);
  d.current_buffer = ((w[4] >> 31u) & 1u) != 0;
  d.input_buffer_0_ptr = w[5];
  d.input_buffer_1_ptr = w[6];
  d.output_buffer_ptr = w[7];
  d.work_buffer_ptr = w[8];
  d.output_buffer_read_offset = static_cast<std::uint8_t>(w[9] & 0x1Fu);
  d.stop_when_done = ((w[9] >> 30u) & 1u) != 0;
  d.interrupt_when_done = ((w[9] >> 31u) & 1u) != 0;
  for (std::size_t i = 0; i < d.reserved.size(); ++i) d.reserved[i] = w[10 + i];
  return d;
}

void XmaContextData::encode(
    std::span<std::byte, kXmaContextBytes> bytes) const noexcept {
  std::array<std::uint32_t, 16> w{};
  w[0] = (input_buffer_0_packet_count & 0xFFFu) |
         (std::uint32_t(loop_count) << 12u) |
         (std::uint32_t(input_buffer_0_valid) << 20u) |
         (std::uint32_t(input_buffer_1_valid) << 21u) |
         ((std::uint32_t(output_buffer_block_count) & 0x1Fu) << 22u) |
         ((std::uint32_t(output_buffer_write_offset) & 0x1Fu) << 27u);
  w[1] = (input_buffer_1_packet_count & 0xFFFu) |
         ((std::uint32_t(loop_subframe_end) & 0x3u) << 12u) |
         ((std::uint32_t(reserved_dword_1_a) & 0x7u) << 14u) |
         ((std::uint32_t(loop_subframe_skip) & 0x7u) << 17u) |
         ((std::uint32_t(subframe_decode_count) & 0xFu) << 20u) |
         ((std::uint32_t(output_buffer_padding) & 0x7u) << 24u) |
         ((std::uint32_t(sample_rate) & 0x3u) << 27u) |
         (std::uint32_t(is_stereo) << 29u) |
         (std::uint32_t(output_buffer_valid) << 31u);
  w[2] = (input_buffer_read_offset & 0x03FFFFFFu) |
         ((std::uint32_t(error_status) & 0x1Fu) << 26u) |
         (std::uint32_t(error_set) << 31u);
  w[3] = (loop_start & 0x03FFFFFFu) |
         ((std::uint32_t(parser_error_status) & 0x1Fu) << 26u) |
         (std::uint32_t(parser_error_set) << 31u);
  w[4] = (loop_end & 0x03FFFFFFu) |
         ((std::uint32_t(packet_metadata) & 0x1Fu) << 26u) |
         (std::uint32_t(current_buffer) << 31u);
  w[5] = input_buffer_0_ptr;
  w[6] = input_buffer_1_ptr;
  w[7] = output_buffer_ptr;
  w[8] = work_buffer_ptr;
  w[9] = (std::uint32_t(output_buffer_read_offset) & 0x1Fu) |
         (std::uint32_t(stop_when_done) << 30u) |
         (std::uint32_t(interrupt_when_done) << 31u);
  for (std::size_t i = 0; i < reserved.size(); ++i) w[10 + i] = reserved[i];
  for (std::size_t i = 0; i < w.size(); ++i) store_be32(bytes.data() + i * 4u, w[i]);
}

}  // namespace xenon::audio

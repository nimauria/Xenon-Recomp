#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "xenon/audio/xma.hpp"
#include "xenon/memory/types.hpp"

using namespace xenon;
using namespace xenon::audio;

namespace {

constexpr std::size_t kPacketBits = kXmaPacketBytes * 8u;

// A packet header: frame count, first-frame offset (bits after the 32-bit
// header; 0x7FFF = no frame starts here), metadata 0, skip count.
void write_packet_header(std::vector<std::byte>& stream, std::size_t packet,
                         std::uint16_t first_frame_raw, std::uint8_t skip = 0) {
  auto* p = stream.data() + packet * kXmaPacketBytes;
  p[0] = std::byte((1u << 2u) | ((first_frame_raw >> 13u) & 0x3u));
  p[1] = std::byte((first_frame_raw >> 5u) & 0xFFu);
  p[2] = std::byte((first_frame_raw & 0x1Fu) << 3u);
  p[3] = std::byte{skip};
}

void write_bits(std::vector<std::byte>& stream, std::size_t bit, unsigned count,
                std::uint32_t value) {
  for (unsigned i = 0; i < count; ++i) {
    const auto set = (value >> (count - 1u - i)) & 1u;
    const auto at = bit + i;
    const auto mask = std::byte(1u << (7u - (at & 7u)));
    stream[at >> 3u] = set ? (stream[at >> 3u] | mask) : (stream[at >> 3u] & ~mask);
  }
}

// Regression (AC6 audio dropouts, XMA parser error 1 / decode error 8 in
// pairs on the streaming contexts): after an input-buffer swap the context
// read offset is bit 32, which is the tail of a frame begun in the previous
// buffer, not a frame. Decoding must resume at the packet's first frame.
void test_frame_start_skips_a_continuation() {
  std::vector<std::byte> stream(3u * kXmaPacketBytes);
  // Packet 0: 100 bits of continuation, then a 300-bit frame at bit 132.
  write_packet_header(stream, 0, 100);
  write_bits(stream, 32, 15, 0x0003);  // what the tail looks like as a "length"
  write_bits(stream, 132, 15, 300);
  // Packet 1 is continuation only; packet 2's first frame is at bit 32 + 40.
  write_packet_header(stream, 1, 0x7FFF);
  write_packet_header(stream, 2, 40);

  assert(xma_frame_start_bit(stream, 32) == 132u);
  assert(xma_frame_start_bit(stream, 132) == 132u);
  assert(xma_frame_start_bit(stream, 432) == 432u);  // a later frame in the same packet
  assert(xma_frame_start_bit(stream, kPacketBits) == 2u * kPacketBits + 72u);
  assert(xma_frame_start_bit(stream, kPacketBits + 32u) == 2u * kPacketBits + 72u);
  assert(xma_frame_start_bit(stream, 3u * kPacketBits) == 3u * kPacketBits);

  assert(gather_xma_frame(stream, 32).status == XmaFrameGather::Invalid);
  const auto frame = gather_xma_frame(stream, 132);
  assert(frame.status == XmaFrameGather::Complete && frame.frame_bits == 300u);
  assert(frame.final_packet == 0u && frame.final_source_bit == 432u);
}

// A frame that runs off the end of the current buffer waits for the other
// buffer instead of failing; with the continuation present it is gathered
// across the packet header, following the skip count.
void test_frame_across_buffers() {
  std::vector<std::byte> one(kXmaPacketBytes);
  write_packet_header(one, 0, static_cast<std::uint16_t>(kPacketBits - 32u - 200u));
  const auto start = kPacketBits - 200u;
  write_bits(one, start, 15, 1000);
  write_bits(one, start + 15u, 32, 0x1ABCDEF5u);
  assert(gather_xma_frame(one, start).status == XmaFrameGather::NeedsMoreInput);

  // Continuation in packet 2 (skip 1 jumps over another stream's packet 1).
  std::vector<std::byte> three(3u * kXmaPacketBytes);
  std::copy(one.begin(), one.end(), three.begin());
  write_packet_header(three, 0, static_cast<std::uint16_t>(kPacketBits - 32u - 200u), 1);
  write_packet_header(three, 1, 0);
  write_packet_header(three, 2, 800);
  write_bits(three, kPacketBits + 32u, 32, 0xFFFFFFFFu);  // must not be copied
  write_bits(three, 2u * kPacketBits + 32u, 8, 0xA5);
  const auto frame = gather_xma_frame(three, start);
  assert(frame.status == XmaFrameGather::Complete && frame.frame_bits == 1000u);
  assert(frame.final_packet == 2u && frame.final_source_bit == 2u * kPacketBits + 32u + 800u);
  // Bit 200 of the frame is the first continuation bit: 0xA5 from packet 2.
  std::uint32_t continuation = 0;
  for (unsigned i = 0; i < 8; ++i) {
    const auto at = 200u + i;
    continuation = (continuation << 1u) |
                   ((std::to_integer<unsigned>(frame.packed_bits[at >> 3u]) >> (7u - (at & 7u))) & 1u);
  }
  assert(continuation == 0xA5u);
}

// The 15-bit frame length itself can straddle two packets; it is read across
// the next packet's header, not through it.
void test_frame_length_across_packets() {
  std::vector<std::byte> stream(2u * kXmaPacketBytes);
  write_packet_header(stream, 0, static_cast<std::uint16_t>(kPacketBits - 32u - 6u));
  write_packet_header(stream, 1, 100);
  const std::uint32_t length = 500;  // 15 bits: first 6 in packet 0, last 9 in packet 1
  write_bits(stream, kPacketBits - 6u, 6, length >> 9u);
  write_bits(stream, kPacketBits + 32u, 9, length & 0x1FFu);
  const auto frame = gather_xma_frame(stream, kPacketBits - 6u);
  assert(frame.status == XmaFrameGather::Complete && frame.frame_bits == length);
  assert(frame.final_packet == 1u && frame.final_source_bit == kPacketBits + 32u + 494u);
}

}  // namespace

int main() {
  test_frame_start_skips_a_continuation();
  test_frame_across_buffers();
  test_frame_length_across_packets();

  {
    std::array<std::byte, kXmaPacketBytes> packet{};
    // frame count 3, 15-bit first-frame offset value 0x123 -> +32 bits,
    // metadata 5, skip count 2.
    const std::uint16_t raw_offset = 0x0123;
    packet[0] = std::byte((3u << 2u) | ((raw_offset >> 13u) & 0x3u));
    packet[1] = std::byte((raw_offset >> 5u) & 0xFFu);
    packet[2] = std::byte(((raw_offset & 0x1Fu) << 3u) | 5u);
    packet[3] = std::byte{2};
    const auto header = parse_xma_packet_header(packet);
    assert(header.frame_count == 3);
    assert(header.first_frame_offset_bits == raw_offset + 32u);
    assert(header.metadata == 5);
    assert(header.skip_count == 2);
  }

  {
    XmaContextData input{};
    input.input_buffer_0_packet_count = 123;
    input.loop_count = 7;
    input.input_buffer_0_valid = true;
    input.output_buffer_block_count = 31;
    input.output_buffer_write_offset = 4;
    input.input_buffer_1_packet_count = 42;
    input.loop_subframe_end = 3;
    input.reserved_dword_1_a = 5;
    input.loop_subframe_skip = 4;
    input.subframe_decode_count = 2;
    input.output_buffer_padding = 3;
    input.sample_rate = 3;
    input.is_stereo = true;
    input.output_buffer_valid = true;
    input.input_buffer_read_offset = 0x12345;
    input.error_status = 7;
    input.error_set = true;
    input.loop_start = 0x23456;
    input.parser_error_status = 9;
    input.parser_error_set = true;
    input.loop_end = 0x34567;
    input.packet_metadata = 17;
    input.input_buffer_0_ptr = 0x00123000;
    input.output_buffer_ptr = 0x00456000;
    input.output_buffer_read_offset = 2;
    std::array<std::byte, kXmaContextBytes> bytes{};
    input.encode(bytes);
    const auto output = XmaContextData::decode(bytes);
    assert(output.input_buffer_0_packet_count == input.input_buffer_0_packet_count);
    assert(output.loop_count == input.loop_count);
    assert(output.input_buffer_0_valid);
    assert(output.output_buffer_block_count == 31);
    assert(output.output_buffer_write_offset == 4);
    assert(output.input_buffer_1_packet_count == 42);
    assert(output.loop_subframe_end == 3);
    assert(output.reserved_dword_1_a == 5);
    assert(output.loop_subframe_skip == 4);
    assert(output.subframe_decode_count == 2);
    assert(output.output_buffer_padding == 3);
    assert(output.sample_rate == 3 && output.is_stereo);
    assert(output.input_buffer_read_offset == 0x12345);
    assert(output.error_status == 7 && output.error_set);
    assert(output.loop_start == 0x23456 && output.loop_end == 0x34567);
    assert(output.parser_error_status == 9 && output.parser_error_set);
    assert(output.packet_metadata == 17);
    assert(output.output_buffer_read_offset == 2);

    // Hardware word 1 regression: loop-end is bits 12-13, bits 14-16 are
    // reserved, skip is bits 17-19, SDC bits 20-23, padding bits 24-26.
    const auto word1 = (std::uint32_t(std::to_integer<std::uint8_t>(bytes[4])) << 24u) |
                       (std::uint32_t(std::to_integer<std::uint8_t>(bytes[5])) << 16u) |
                       (std::uint32_t(std::to_integer<std::uint8_t>(bytes[6])) << 8u) |
                       std::uint32_t(std::to_integer<std::uint8_t>(bytes[7]));
    assert(((word1 >> 12u) & 0x3u) == 3u);
    assert(((word1 >> 14u) & 0x7u) == 5u);
    assert(((word1 >> 17u) & 0x7u) == 4u);
    assert(((word1 >> 20u) & 0xFu) == 2u);
    assert(((word1 >> 24u) & 0x7u) == 3u);
  }

  memory::AddressSpace memory(memory::GuestTranslationMode::Compact);
  assert(memory.initialize());
  XmaDecoder decoder(memory);
  std::string error;
  assert(decoder.initialize(&error));
  const auto context = decoder.allocate_context();
  assert(context != 0);
  assert(decoder.owns_context(context));
  assert(memory.get_physical_address(context) == decoder.context_array_physical());

  cpu::GuestAddress output_buffer{};
  assert(memory.allocate(31u * kXmaOutputBlockBytes, 256, memory::kReadWrite,
                         false, output_buffer));
  XmaContextInit init{};
  init.output_buffer = output_buffer;
  init.output_buffer_block_count = 31;
  init.subframe_decode_count = 4;
  init.channel_count = 1;
  init.sample_rate = 3;
  assert(decoder.initialize_context(context, init));
  XmaContextData initialized_state{};
  assert(decoder.read_context(context, initialized_state));
  assert(initialized_state.is_stereo);

  // XDK/XMA uses channel mode 0 for mono; this must not be rejected as a
  // zero-channel format.
  init.channel_count = 0;
  assert(decoder.initialize_context(context, init));
  assert(decoder.read_context(context, initialized_state));
  assert(!initialized_state.is_stereo);
  init.channel_count = 1;
  assert(decoder.initialize_context(context, init));

  // Regression: decoder/DMA-visible context writes must invalidate an active
  // Memory V2 reservation rather than mutating the backing RAM directly.
  std::uint32_t reserved_value{};
  const auto token = memory.reserve32(context, reserved_value);
  assert(token != 0);
  XmaContextData state{};
  assert(decoder.read_context(context, state));
  state.output_buffer_valid = true;
  assert(decoder.write_context(context, state, true));
  assert(!memory.store_conditional32(context, token, 0xAABBCCDDu));

  assert(decoder.decoded_samples(context) == 0);

  // The XMA hardware aperture must expose the physical context-array address
  // and rotating current-context register through Memory V2 MMIO. The APU
  // register file is little-endian: titles read it with lwbrx (read32_le),
  // and a plain big-endian load sees the bytes reversed.
  const auto array_physical = decoder.context_array_physical();
  assert(memory.read32_le(kXmaMmioBase + 0x1800u) == array_physical);
  assert(memory.read32_be(kXmaMmioBase + 0x1800u) ==
         ((array_physical >> 24u) | ((array_physical >> 8u) & 0xFF00u) |
          ((array_physical << 8u) & 0xFF0000u) | (array_physical << 24u)));
  assert(memory.read8(kXmaMmioBase + 0x1800u) == (array_physical & 0xFFu));
  assert(memory.read16_le(kXmaMmioBase + 0x1802u) == (array_physical >> 16u));
  const auto current0 = memory.read32_le(kXmaMmioBase + 0x1818u);
  const auto current1 = memory.read32_le(kXmaMmioBase + 0x1818u);
  assert(current0 < kXmaContextCount);
  assert(current1 == (current0 + 1u) % kXmaContextCount);

  // Completion is observable even for a no-input terminal context. The
  // interrupt flag is propagated to the platform-facing completion sink
  // without inventing a game-specific interrupt transport.
  std::atomic<unsigned> completions{0};
  std::atomic<bool> interrupt_requested{false};
  decoder.set_completion_sink([&](cpu::GuestAddress completed, bool interrupt) {
    assert(completed == context);
    interrupt_requested.store(interrupt, std::memory_order_release);
    completions.fetch_add(1, std::memory_order_release);
  });
  assert(decoder.read_context(context, state));
  state.output_buffer_valid = true;
  state.interrupt_when_done = true;
  state.stop_when_done = true;
  state.input_buffer_0_valid = false;
  state.input_buffer_1_valid = false;
  assert(decoder.write_context(context, state, false));

  // Regression (AC6 silence): the title's statically linked XMA code derives
  // the hardware index from the context's physical address and the array
  // register it read with lwbrx, then kicks with stwbrx. With big-endian
  // registers that index was ~62000 and the kick landed outside the kick
  // group, so nothing ever decoded. The same arithmetic must kick context 0.
  const auto hw_index =
      (memory.get_physical_address(context) - memory.read32_le(kXmaMmioBase + 0x1800u)) /
      kXmaContextBytes;
  assert(hw_index == 0u);
  memory.write32_le(kXmaMmioBase + 0x1940u + (hw_index >> 5u) * 4u, 1u << (hw_index & 31u));
  for (int i = 0; i < 200 && completions.load(std::memory_order_acquire) == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(completions.load(std::memory_order_acquire) == 1);
  assert(interrupt_requested.load(std::memory_order_acquire));
  assert(decoder.block_while_in_use(context));

  // Hardware clear resets validity and ring progress through the same Memory
  // V2 external-write path.
  assert(decoder.read_context(context, state));
  state.input_buffer_0_valid = true;
  state.input_buffer_1_valid = true;
  state.output_buffer_valid = true;
  state.output_buffer_read_offset = 3;
  state.output_buffer_write_offset = 4;
  assert(decoder.write_context(context, state, false));
  memory.write32_le(kXmaMmioBase + 0x1A80u, 1u);
  assert(decoder.read_context(context, state));
  assert(!state.input_buffer_0_valid && !state.input_buffer_1_valid);
  assert(!state.output_buffer_valid);
  assert(state.input_buffer_read_offset == 32u);
  assert(state.output_buffer_read_offset == 0u);
  assert(state.output_buffer_write_offset == 0u);

  assert(decoder.release_context(context));
  assert(!decoder.owns_context(context));
  decoder.shutdown();
  assert(memory.release(output_buffer));

  std::cout << "Audio XMA tests passed\n";
  return 0;
}

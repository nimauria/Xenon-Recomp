#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>

#include "xenon/audio/xma.hpp"
#include "xenon/memory/types.hpp"

using namespace xenon;
using namespace xenon::audio;

int main() {
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
  // and rotating current-context register through Memory V2 MMIO.
  assert(memory.read32_be(kXmaMmioBase + 0x1800u) == decoder.context_array_physical());
  const auto current0 = memory.read32_be(kXmaMmioBase + 0x1818u);
  const auto current1 = memory.read32_be(kXmaMmioBase + 0x1818u);
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

  // Context 0 is the first allocation: writing bit 0 to kick group 0 must
  // converge on the same enable path as XMAEnableContext.
  memory.write32_be(kXmaMmioBase + 0x1940u, 1u);
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
  memory.write32_be(kXmaMmioBase + 0x1A80u, 1u);
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

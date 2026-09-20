#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xenon/xbox/xex_lzx.hpp"

namespace {

std::vector<std::byte> make_pattern(std::size_t size, std::uint32_t seed) {
  std::vector<std::byte> data(size);
  std::uint32_t state = seed;
  for (auto& b : data) {
    state = state * 1664525u + 1013904223u;  // classic LCG, deterministic.
    b = static_cast<std::byte>((state >> 24) & 0xFFu);
  }
  return data;
}

void test_internal_self_test() {
  std::string error;
  const bool ok = xenon::xbox::lzx::debug_self_test(&error);
  assert(ok && error.empty());
}

void test_uncompressed_round_trip_small() {
  const auto input = make_pattern(100, 1u);
  const auto compressed = xenon::xbox::lzx::encode_uncompressed(input, 17);

  std::vector<std::byte> output(input.size());
  const auto result = xenon::xbox::lzx::decode(compressed, 17, {}, output);
  assert(result.ok);
  assert(output == input);
}

void test_uncompressed_round_trip_empty() {
  std::vector<std::byte> input;
  const auto compressed = xenon::xbox::lzx::encode_uncompressed(input, 15);
  std::vector<std::byte> output;
  const auto result = xenon::xbox::lzx::decode(compressed, 15, {}, output);
  assert(result.ok);
}

void test_uncompressed_round_trip_odd_size() {
  // Odd size exercises the trailing pad-byte-to-even-alignment path.
  const auto input = make_pattern(4097, 7u);
  const auto compressed = xenon::xbox::lzx::encode_uncompressed(input, 15);
  std::vector<std::byte> output(input.size());
  const auto result = xenon::xbox::lzx::decode(compressed, 15, {}, output);
  assert(result.ok);
  assert(output == input);
}

void test_uncompressed_round_trip_multi_chunk() {
  // Larger than one 32 KiB LZX chunk and not a clean multiple of it, so this
  // exercises multi-block chaining plus the periodic chunk-boundary
  // realignment logic.
  const auto input = make_pattern(0x8000u * 3u + 1234u, 42u);
  const auto compressed = xenon::xbox::lzx::encode_uncompressed(input, 18);
  std::vector<std::byte> output(input.size());
  const auto result = xenon::xbox::lzx::decode(compressed, 18, {}, output);
  assert(result.ok);
  assert(output == input);
}

void test_decode_rejects_invalid_window_bits() {
  std::vector<std::byte> compressed = {std::byte{0}, std::byte{0}};
  std::vector<std::byte> output(4);
  auto result = xenon::xbox::lzx::decode(compressed, 14, {}, output);
  assert(!result.ok);
  result = xenon::xbox::lzx::decode(compressed, 22, {}, output);
  assert(!result.ok);
}

void test_decode_rejects_truncated_stream() {
  std::vector<std::byte> output(1000);
  // Only 4 bytes of input for 1000 bytes of expected output: must fail
  // cleanly, never read past the buffer, never return ok with garbage.
  std::vector<std::byte> compressed = {std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
  const auto result = xenon::xbox::lzx::decode(compressed, 15, {}, output);
  assert(!result.ok);
  assert(!result.error.empty());
}

// Packs `value` (n bits, n <= 16) MSB-first into `bits`/`bit_count`, matching
// LZX's 16-bit-little-endian-word, MSB-first bit order, flushing whole words
// out to `out` as they fill.
void put_bits(std::vector<std::byte>& out, std::uint32_t& bit_buffer, int& bit_count,
             std::uint32_t value, int n) {
  bit_buffer = (bit_buffer << n) | (value & ((n < 32 ? (1u << n) : 0u) - 1u));
  bit_count += n;
  while (bit_count >= 16) {
    const auto word = (bit_buffer >> (bit_count - 16)) & 0xFFFFu;
    out.push_back(static_cast<std::byte>(word & 0xFFu));
    out.push_back(static_cast<std::byte>((word >> 8) & 0xFFu));
    bit_count -= 16;
  }
}

void test_decode_rejects_oversized_block_size() {
  // Uncompressed block (type=3) header claiming a block_size far larger than
  // the output buffer must be rejected rather than overrunning it.
  std::vector<std::byte> compressed;
  std::uint32_t bit_buffer = 0;
  int bit_count = 0;
  put_bits(compressed, bit_buffer, bit_count, 3u, 3);                 // block type = uncompressed
  put_bits(compressed, bit_buffer, bit_count, 0xFFu, 8);              // size high byte
  put_bits(compressed, bit_buffer, bit_count, 0xFFFFu, 16);           // size low 16 bits
  if (bit_count > 0) put_bits(compressed, bit_buffer, bit_count, 0u, 16 - bit_count);

  std::vector<std::byte> output(16);  // Far smaller than the claimed 0xFFFFFF block size.
  const auto result = xenon::xbox::lzx::decode(compressed, 15, {}, output);
  assert(!result.ok);
}

void test_decode_rejects_invalid_block_type() {
  std::vector<std::byte> compressed;
  std::uint32_t bit_buffer = 0;
  int bit_count = 0;
  put_bits(compressed, bit_buffer, bit_count, 0u, 3);   // block type 0 is not defined.
  put_bits(compressed, bit_buffer, bit_count, 0u, 8);
  put_bits(compressed, bit_buffer, bit_count, 16u, 16);
  if (bit_count > 0) put_bits(compressed, bit_buffer, bit_count, 0u, 16 - bit_count);

  std::vector<std::byte> output(16);
  const auto result = xenon::xbox::lzx::decode(compressed, 15, {}, output);
  assert(!result.ok);
}

void test_decode_handles_zero_length_output() {
  std::vector<std::byte> compressed = {std::byte{0}, std::byte{0}};
  std::vector<std::byte> output;  // zero-size output: decode() must return ok
                                  // immediately without touching the stream.
  const auto result = xenon::xbox::lzx::decode(compressed, 15, {}, output);
  assert(result.ok);
}

void test_reference_buffer_extends_history() {
  // Build an uncompressed-block stream whose *content* is itself only valid
  // once concatenated after a reference buffer: encode_uncompressed() never
  // emits matches, so this specifically exercises that decode() accepts (and
  // ignores, for uncompressed blocks) a non-empty reference buffer without
  // corrupting output - the real match-into-reference path is exercised by
  // the loader-level XEXP tests in xex_loader_tests.cpp against a
  // Huffman-coded fixture is out of scope here (decode-only, no production
  // encoder), but this proves the reference-buffer plumbing itself is inert
  // when no match ever needs it.
  const auto reference = make_pattern(256, 99u);
  const auto input = make_pattern(300, 5u);
  const auto compressed = xenon::xbox::lzx::encode_uncompressed(input, 15);
  std::vector<std::byte> output(input.size());
  const auto result = xenon::xbox::lzx::decode(compressed, 15, reference, output);
  assert(result.ok);
  assert(output == input);
}

void test_huffman_literal_block_round_trip_verbatim() {
  const auto input = make_pattern(4000, 3u);
  const auto compressed = xenon::xbox::lzx::encode_literal_huffman_block(input, 17u, /*aligned=*/false);
  std::vector<std::byte> output(input.size());
  const auto result = xenon::xbox::lzx::decode(compressed, 17u, {}, output);
  assert(result.ok);
  assert(output == input);
}

void test_huffman_literal_block_round_trip_aligned() {
  const auto input = make_pattern(4000, 4u);
  const auto compressed = xenon::xbox::lzx::encode_literal_huffman_block(input, 17u, /*aligned=*/true);
  std::vector<std::byte> output(input.size());
  const auto result = xenon::xbox::lzx::decode(compressed, 17u, {}, output);
  assert(result.ok);
  assert(output == input);
}

void test_huffman_literal_block_round_trip_multi_chunk() {
  // Mirrors the loader-level retail-shaped-pipeline fixture: split a large
  // body into independently-encoded <=0x8000-byte Huffman blocks alternating
  // VERBATIM/ALIGNED, concatenated into one bitstream and decoded in one
  // decode() call (exercising the chunk-realignment interaction between
  // blocks, not just a single self-contained block).
  const auto input = make_pattern(0x8000u * 4u, 11u);
  std::vector<std::byte> compressed;
  constexpr std::size_t kChunk = 0x8000u;
  bool aligned = false;
  for (std::size_t offset = 0; offset < input.size(); offset += kChunk) {
    const auto len = std::min(kChunk, input.size() - offset);
    const auto block = xenon::xbox::lzx::encode_literal_huffman_block(
        std::span<const std::byte>(input).subspan(offset, len), 17u, aligned, /*first_block=*/offset == 0u);
    compressed.insert(compressed.end(), block.begin(), block.end());
    aligned = !aligned;
  }
  std::vector<std::byte> output(input.size());
  const auto result = xenon::xbox::lzx::decode(compressed, 17u, {}, output);
  assert(result.ok);
  assert(output == input);
}

}  // namespace

int main() {
  test_internal_self_test();
  test_uncompressed_round_trip_small();
  test_uncompressed_round_trip_empty();
  test_uncompressed_round_trip_odd_size();
  test_uncompressed_round_trip_multi_chunk();
  test_decode_rejects_invalid_window_bits();
  test_decode_rejects_truncated_stream();
  test_decode_rejects_oversized_block_size();
  test_decode_rejects_invalid_block_type();
  test_decode_handles_zero_length_output();
  test_reference_buffer_extends_history();
  test_huffman_literal_block_round_trip_verbatim();
  test_huffman_literal_block_round_trip_aligned();
  test_huffman_literal_block_round_trip_multi_chunk();
  return 0;
}

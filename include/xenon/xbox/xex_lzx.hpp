#pragma once

// Real LZX decompressor for XEX_COMPRESSION_NORMAL (and, with a reference
// buffer supplied, the LZXDELTA variant XEXP title-update patches use for
// XEX_COMPRESSION_DELTA). This is an original implementation written against
// the published LZX bitstream format (as used by Microsoft Cabinet/WIM and
// documented for interoperability in the [MS-PATCH] Open Specification,
// "LZX DELTA Compression and Decompression") - it is not derived from any
// GPL/LGPL LZX implementation.
//
// Scope: decode only (XEX files are always distributed pre-compressed; Xenon
// never needs to produce LZX streams outside of test fixtures, which use the
// spec's "uncompressed block" encoding - see xex_lzx_encode_uncompressed()
// below).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace xenon::xbox::lzx {

// window_bits must be in [15, 21], matching the Xbox 360 XEX window sizes
// (32 KiB .. 2 MiB). Values outside this range are rejected.
[[nodiscard]] bool valid_window_bits(std::uint32_t window_bits) noexcept;

struct DecodeResult {
  bool ok{false};
  std::string error;
};

// Decodes `compressed` (a raw, concatenated LZX bitstream - i.e. already
// de-chunked from the outer XEX xex2_compressed_block_info container) into
// exactly `output.size()` bytes.
//
// `reference` is the LZXDELTA "reference data" window: when non-empty, match
// distances that reach past the start of the output-produced-so-far resolve
// into the tail of `reference` instead of failing, exactly as the XEXP patch
// format requires (title-update streams are compressed against the base XEX
// image as reference data). Pass an empty span for plain
// XEX_COMPRESSION_NORMAL decoding.
[[nodiscard]] DecodeResult decode(std::span<const std::byte> compressed,
                                  std::uint32_t window_bits,
                                  std::span<const std::byte> reference,
                                  std::span<std::byte> output);

// Encodes `input` as a single, spec-legal LZX "uncompressed block" stream
// (block type 3: R0/R1/R2 reset to 1, raw bytes follow, chunk-realigned every
// 0x8000 output bytes exactly like real LZX streams). This is intentionally
// the only encode path Xenon ships: it exists purely to build deterministic,
// spec-conformant round-trip test fixtures for decode() without requiring a
// full Huffman-optimizing encoder that production code will never call.
[[nodiscard]] std::vector<std::byte> encode_uncompressed(
    std::span<const std::byte> input, std::uint32_t window_bits);

// Encodes `input` as a single, spec-legal LZX block using *real* canonical-
// Huffman-coded literals (block type 1 "verbatim", or block type 2
// "aligned" when `aligned` is true - the aligned-offset tree is present but
// empty/unused either way, since this encoder never emits a match). Every
// one of the 256 literal symbols is assigned an equal code length of 8 bits
// via the pretree-transmitted delta encoding exactly as a real LZX stream
// would (not a synthetic/uncompressed passthrough - see
// encode_uncompressed() above for that), which happens to make the
// canonical code identical to the byte value itself (first_code[8] == 0 for
// a 256-symbol/all-length-8 tree), but the bitstream produced is
// nonetheless real, spec-legal Huffman-coded LZX: decode() reconstructs it
// via the exact same pretree/canonical-Huffman machinery it uses for any
// other Huffman-coded stream. `input.size()` must be small enough to fit in
// one 32 KiB output chunk (no inter-chunk realignment is emitted). This
// exists purely to build deterministic Huffman-path test fixtures for
// decode() - see docs/xbox/XEX_LOADER_V2.md. `window_bits` must match the
// value decode() will be called with (it determines how many position-slot
// match symbols the main tree reserves, which must be signalled - as
// unused, length 0 - even though this encoder never emits a match).
//
// `first_block` must be true only for the first block of a stream (or any
// stand-alone single-block stream) and false for every subsequent block
// concatenated after it. LZX code lengths are delta-coded against the
// *previous* block's lengths, which persist across blocks within one
// stream (reset_interval=0, matching XEX usage) - not against zero every
// time - so a block that isn't first must signal "no change" (delta 0)
// for the literal range instead of "set to length 8" (delta 9), since the
// previous block already left it at length 8.
[[nodiscard]] std::vector<std::byte> encode_literal_huffman_block(
    std::span<const std::byte> input, std::uint32_t window_bits, bool aligned, bool first_block = true);

// Exercises the canonical-Huffman table builder/decoder and the position-
// slot table generator directly (they have no other externally reachable
// entry point, since decode() only uses them internally against real LZX
// bitstreams). Returns true iff every internal check passes; used only by
// tests/xbox/xex_lzx_tests.cpp.
[[nodiscard]] bool debug_self_test(std::string* error = nullptr);

struct DeltaPatchResult {
  bool ok{false};
  std::string error;
};

// Applies a XEXP "xex2_delta_patch" record chain onto `dest` in place. This
// is the real per-record LZXDELTA framing XEXP title updates use for
// header-region deltas (XEX_HEADER_DELTA_PATCH_DESCRIPTOR's embedded
// xex2_delta_patch at descriptor offset 0x4C) - distinct from the
// xex2_compressed_block_info chunk-table framing XEX_COMPRESSION_NORMAL/
// DELTA body compression uses (see decode() above).
//
// Each 12-byte record (big-endian old_addr, new_addr, uncompressed_len,
// compressed_len) is followed by `compressed_len` bytes of payload, except
// for two sentinel compressed_len values that carry no payload:
//   0 - "fill with zero": `uncompressed_len` zero bytes are written at
//       dest[new_addr].
//   1 - "copy": `uncompressed_len` bytes are copied from dest[old_addr] to
//       dest[new_addr] verbatim (not LZX-compressed).
// Any other compressed_len is a real LZX-compressed chunk: dest[old_addr,
// old_addr+uncompressed_len) is snapshotted and used as the LZXDELTA
// reference window, and the decoded output is written to dest[new_addr,
// new_addr+uncompressed_len). A record whose four fields are all zero
// terminates the chain early (matches the real format's explicit
// terminator). `records` is walked until exhausted or a terminator record
// is hit; every offset/length is bounds-checked against `dest` and
// `records` before use, so malformed/truncated chains fail cleanly.
[[nodiscard]] DeltaPatchResult apply_delta_patch_records(
    std::span<const std::byte> records, std::uint32_t window_bits,
    std::span<std::byte> dest);

}  // namespace xenon::xbox::lzx

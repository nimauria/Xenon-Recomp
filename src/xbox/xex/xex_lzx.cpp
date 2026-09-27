#include "xenon/xbox/xex_lzx.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace xenon::xbox::lzx {
namespace {

constexpr std::uint32_t kMinWindowBits = 15;
constexpr std::uint32_t kMaxWindowBits = 21;
constexpr std::uint32_t kChunkSize = 0x8000u;  // Output realignment granularity.
constexpr int kNumChars = 256;                 // Literal symbols in the main tree.
constexpr int kNumPretreeSymbols = 20;
constexpr int kLengthTreeSize = 249;
constexpr int kAlignedTreeSize = 8;
constexpr int kMaxCodeLength = 16;  // LZX bounds all transmitted code lengths to 16.
constexpr int kMinMatchLength = 2;

// ---------------------------------------------------------------------------
// Bit reader. LZX packs bits into 16-bit little-endian words and reads them
// MSB-first within each word (this is the one place LZX's bit order differs
// from the more common LSB-first convention used by DEFLATE).
// ---------------------------------------------------------------------------
class BitReader {
 public:
  explicit BitReader(std::span<const std::byte> data) : data_(data) {}

  [[nodiscard]] bool ensure(int n) noexcept {
    while (bit_count_ < n) {
      std::uint32_t word = 0;
      if (byte_pos_ + 2u <= data_.size()) {
        word = static_cast<std::uint32_t>(std::to_integer<unsigned>(data_[byte_pos_])) |
               (static_cast<std::uint32_t>(std::to_integer<unsigned>(data_[byte_pos_ + 1u])) << 8);
      } else if (byte_pos_ < data_.size()) {
        word = static_cast<std::uint32_t>(std::to_integer<unsigned>(data_[byte_pos_]));
      } else {
        out_of_data_ = true;
        // Zero-pad past end of stream; callers must treat out_of_data() as a
        // hard failure rather than trust bits read this way.
      }
      byte_pos_ += 2u;
      buffer_ = (buffer_ << 16) | word;
      bit_count_ += 16;
    }
    return !out_of_data_;
  }

  [[nodiscard]] std::uint32_t peek(int n) noexcept {
    if (n == 0) return 0u;
    (void)ensure(n);
    return (buffer_ >> (bit_count_ - n)) & ((1u << n) - 1u);
  }

  void consume(int n) noexcept { bit_count_ -= n; }

  [[nodiscard]] std::uint32_t read(int n) noexcept {
    const auto v = peek(n);
    consume(n);
    return v;
  }

  // Discards any bits left over from a partially-consumed 16-bit input word,
  // matching LZX's per-32KiB-chunk (and post block-header, for uncompressed
  // blocks) realignment requirement.
  void align16() noexcept { bit_count_ -= (bit_count_ % 16); }

  // For uncompressed blocks: after align16(), rewind byte_pos_ so raw byte
  // reads resume exactly where the bit-buffer's unread words begin, then
  // drop the buffer entirely (uncompressed data bypasses the bit machinery).
  void flush_to_byte_boundary() noexcept {
    align16();
    byte_pos_ -= static_cast<std::size_t>(bit_count_ / 16) * 2u;
    bit_count_ = 0;
    buffer_ = 0;
  }

  [[nodiscard]] bool read_raw_bytes(std::span<std::byte> out) noexcept {
    if (byte_pos_ + out.size() > data_.size()) {
      out_of_data_ = true;
      return false;
    }
    std::memcpy(out.data(), data_.data() + byte_pos_, out.size());
    byte_pos_ += out.size();
    return true;
  }

  [[nodiscard]] bool out_of_data() const noexcept { return out_of_data_; }

 private:
  std::span<const std::byte> data_;
  std::size_t byte_pos_{0};
  std::uint64_t buffer_{0};
  int bit_count_{0};
  bool out_of_data_{false};
};

// ---------------------------------------------------------------------------
// Canonical Huffman decode table, built from an array of per-symbol code
// lengths (0 = symbol unused) exactly as LZX transmits them.
// ---------------------------------------------------------------------------
class HuffmanTable {
 public:
  [[nodiscard]] bool build(std::span<const std::uint8_t> lengths) {
    symbols_by_length_.clear();
    first_code_.fill(0u);
    count_by_length_.fill(0u);
    first_symbol_index_.fill(0u);

    std::array<std::uint32_t, kMaxCodeLength + 1> count{};
    for (const auto len : lengths) {
      if (len > kMaxCodeLength) return false;
      if (len > 0) count[len] += 1u;
    }
    std::uint32_t code = 0;
    std::array<std::uint32_t, kMaxCodeLength + 1> first_code{};
    for (int len = 1; len <= kMaxCodeLength; ++len) {
      code = (code + count[static_cast<std::size_t>(len - 1)]) << 1;
      first_code[static_cast<std::size_t>(len)] = code;
    }
    // Verify the code set is a valid (complete or under-full, never
    // over-subscribed) prefix code; an over-subscribed code means the
    // transmitted lengths are corrupt. `code` holds first_code[kMaxCodeLength]
    // at this point, so the highest code actually assigned at the deepest
    // level is first_code[kMaxCodeLength] + count[kMaxCodeLength] - 1.
    if (static_cast<std::uint64_t>(code) + count[static_cast<std::size_t>(kMaxCodeLength)] >
        (1u << kMaxCodeLength)) {
      return false;
    }

    // Order symbols by (length, symbol index) so each length's symbols occupy
    // a contiguous run - this lets decode() do a simple canonical lookup.
    symbols_by_length_.resize(lengths.size());
    std::array<std::uint32_t, kMaxCodeLength + 1> cursor{};
    std::uint32_t running = 0;
    for (int len = 1; len <= kMaxCodeLength; ++len) {
      first_symbol_index_[static_cast<std::size_t>(len)] = running;
      cursor[static_cast<std::size_t>(len)] = running;
      running += count[static_cast<std::size_t>(len)];
    }
    for (std::size_t sym = 0; sym < lengths.size(); ++sym) {
      const auto len = lengths[sym];
      if (len == 0) continue;
      symbols_by_length_[cursor[len]++] = static_cast<std::uint32_t>(sym);
    }
    first_code_ = first_code;
    for (int len = 1; len <= kMaxCodeLength; ++len) {
      count_by_length_[static_cast<std::size_t>(len)] = count[static_cast<std::size_t>(len)];
    }
    return true;
  }

  // Bit-by-bit canonical decode: correctness-first: shortest legal code for
  // the current bit position is always found within kMaxCodeLength steps.
  [[nodiscard]] bool decode(BitReader& reader, std::uint32_t& out_symbol) const {
    std::uint32_t code = 0;
    for (int len = 1; len <= kMaxCodeLength; ++len) {
      code = (code << 1) | reader.read(1);
      const auto count = count_by_length_[static_cast<std::size_t>(len)];
      if (count != 0u) {
        const auto base = first_code_[static_cast<std::size_t>(len)];
        if (code >= base && code - base < count) {
          const auto index = first_symbol_index_[static_cast<std::size_t>(len)] + (code - base);
          out_symbol = symbols_by_length_[index];
          return true;
        }
      }
      if (reader.out_of_data()) return false;
    }
    return false;
  }

 private:
  std::vector<std::uint32_t> symbols_by_length_;
  std::array<std::uint32_t, kMaxCodeLength + 1> first_code_{};
  std::array<std::uint32_t, kMaxCodeLength + 1> count_by_length_{};
  std::array<std::uint32_t, kMaxCodeLength + 1> first_symbol_index_{};
};

// ---------------------------------------------------------------------------
// Position slot table (distance base/extra-footer-bits per slot), derived
// recursively rather than hand-transcribed: base[0]=0, base[1]=1,
// footer[0]=footer[1]=0, and for n>=2: footer[n] = min(17, n/2 - 1),
// base[n] = base[n-1] + (1 << footer[n-1]). This reproduces the standard LZX
// position-slot table exactly (verified against the well-known slot 0..50
// values) without risking a hand-transcription error in a 50-row table.
// ---------------------------------------------------------------------------
struct PositionSlots {
  static constexpr int kMaxSlots = 51;
  std::array<std::uint32_t, kMaxSlots> base{};
  std::array<std::uint32_t, kMaxSlots> footer_bits{};

  PositionSlots() {
    footer_bits[0] = 0;
    footer_bits[1] = 0;
    base[0] = 0;
    base[1] = 1;
    for (int n = 2; n < kMaxSlots; ++n) {
      const int raw = n / 2 - 1;
      footer_bits[static_cast<std::size_t>(n)] = static_cast<std::uint32_t>(std::min(17, raw));
      base[static_cast<std::size_t>(n)] =
          base[static_cast<std::size_t>(n - 1)] + (1u << footer_bits[static_cast<std::size_t>(n - 1)]);
    }
  }
};

const PositionSlots& position_slots() {
  static const PositionSlots slots;
  return slots;
}

int num_position_slots_for_window(std::uint32_t window_bits) {
  const auto window_size = 1u << window_bits;
  const auto& slots = position_slots();
  for (int n = 0; n < PositionSlots::kMaxSlots; ++n) {
    if (slots.base[static_cast<std::size_t>(n)] >= window_size) return n;
  }
  return PositionSlots::kMaxSlots - 1;
}

// ---------------------------------------------------------------------------
// Decoder state persists across blocks within one stream (tree lengths are
// delta-coded from the previous block; R0..R2 persist until the stream
// ends since XEX uses reset_interval=0).
// ---------------------------------------------------------------------------
struct DecoderState {
  std::vector<std::uint8_t> main_lengths;
  std::vector<std::uint8_t> length_lengths;
  std::array<std::uint32_t, 3> repeated_offsets{1u, 1u, 1u};
  int num_position_slots{0};
  int main_tree_size{0};
};

// Each of the tree's N length slots carries its own delta history across
// blocks (and across the literal/remainder/length-tree passes, since they
// all index into the same persistent state arrays): "previous code size" in
// the delta formula below always means *this same slot's* length as of the
// last time it was set (0 before the first block ever touches it), not a
// neighboring slot. This is what lets a block cheaply re-signal "my tree is
// identical to the previous block's" via an all-delta-zero pass, and it is
// why `lengths` must be a long-lived vector rather than reset per block.
std::uint8_t apply_delta(std::uint8_t previous, std::uint32_t delta_symbol) {
  int new_length = (static_cast<int>(previous) - static_cast<int>(delta_symbol) + 17) % 17;
  if (new_length < 0) new_length += 17;
  return static_cast<std::uint8_t>(new_length);
}

bool read_pretree_and_lengths(BitReader& reader, std::vector<std::uint8_t>& lengths,
                              std::size_t begin, std::size_t end) {
  std::array<std::uint8_t, kNumPretreeSymbols> pretree_lengths{};
  for (auto& len : pretree_lengths) {
    len = static_cast<std::uint8_t>(reader.read(4));
  }
  HuffmanTable pretree;
  if (!pretree.build(pretree_lengths)) return false;

  std::size_t i = begin;
  while (i < end) {
    std::uint32_t symbol = 0;
    if (!pretree.decode(reader, symbol)) return false;

    if (symbol == 17u) {
      const auto extra = reader.read(4);
      auto run = 4u + extra;
      while (run-- > 0u && i < end) lengths[i++] = 0u;
    } else if (symbol == 18u) {
      const auto extra = reader.read(5);
      auto run = 20u + extra;
      while (run-- > 0u && i < end) lengths[i++] = 0u;
    } else if (symbol == 19u) {
      const auto extra = reader.read(1);
      auto run = 4u + extra;
      std::uint32_t second = 0;
      if (!pretree.decode(reader, second)) return false;
      // "Same delta" run: every covered slot is shifted by the same delta
      // symbol, applied against that slot's own previous length - not
      // forced to a single shared resulting value.
      while (run-- > 0u && i < end) {
        lengths[i] = apply_delta(lengths[i], second);
        ++i;
      }
    } else {
      lengths[i] = apply_delta(lengths[i], symbol);
      ++i;
    }
    if (reader.out_of_data()) return false;
  }
  return true;
}

std::uint32_t delta_read_be32(std::span<const std::byte> bytes, std::size_t offset) {
  return (static_cast<std::uint32_t>(std::to_integer<unsigned>(bytes[offset])) << 24) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned>(bytes[offset + 1])) << 16) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned>(bytes[offset + 2])) << 8) |
         static_cast<std::uint32_t>(std::to_integer<unsigned>(bytes[offset + 3]));
}

std::uint16_t delta_read_be16(std::span<const std::byte> bytes, std::size_t offset) {
  return static_cast<std::uint16_t>((static_cast<std::uint32_t>(std::to_integer<unsigned>(bytes[offset])) << 8) |
                                    static_cast<std::uint32_t>(std::to_integer<unsigned>(bytes[offset + 1])));
}

}  // namespace

bool valid_window_bits(std::uint32_t window_bits) noexcept {
  return window_bits >= kMinWindowBits && window_bits <= kMaxWindowBits;
}

DecodeResult decode(std::span<const std::byte> compressed, std::uint32_t window_bits,
                    std::span<const std::byte> reference, std::span<std::byte> output) {
  DecodeResult result{};
  if (!valid_window_bits(window_bits)) {
    result.error = "LZX window_bits out of range.";
    return result;
  }

  DecoderState state{};
  state.num_position_slots = num_position_slots_for_window(window_bits);
  state.main_tree_size = kNumChars + state.num_position_slots * 8;
  state.main_lengths.assign(static_cast<std::size_t>(state.main_tree_size), 0u);
  state.length_lengths.assign(static_cast<std::size_t>(kLengthTreeSize), 0u);

  BitReader reader(compressed);
  const auto& slots = position_slots();

  // Combined addressable history: `reference` (LZXDELTA base image, possibly
  // empty) immediately followed by the bytes already produced in `output`.
  // Matches whose distance reaches past what's been produced so far resolve
  // into the tail of `reference`, which is exactly the XEXP delta-patch
  // semantics (LZXDELTA "reference data").
  const auto copy_match = [&](std::size_t dest_pos, std::uint32_t distance,
                             std::uint32_t length) -> bool {
    for (std::uint32_t i = 0; i < length; ++i) {
      const std::size_t absolute = reference.size() + dest_pos + i;
      if (absolute < distance) return false;  // Distance reaches before all history.
      const std::size_t source_absolute = absolute - distance;
      std::byte value{};
      if (source_absolute < reference.size()) {
        value = reference[source_absolute];
      } else {
        const auto output_index = source_absolute - reference.size();
        if (output_index >= dest_pos + i) return false;  // Never read unwritten output.
        value = output[output_index];
      }
      if (dest_pos + i >= output.size()) return false;
      output[dest_pos + i] = value;
    }
    return true;
  };

  std::size_t out_pos = 0;
  std::size_t next_chunk_boundary = kChunkSize;

  while (out_pos < output.size()) {
    const auto block_type = reader.read(3);
    if (reader.out_of_data()) {
      result.error = "LZX stream truncated reading block header.";
      return result;
    }
    const std::uint32_t block_size = (reader.read(8) << 16) | reader.read(16);
    if (block_size == 0u || out_pos + block_size > output.size()) {
      result.error = "LZX block size is invalid or overruns the output buffer.";
      return result;
    }

    if (block_type == 3u) {
      // Uncompressed block.
      reader.flush_to_byte_boundary();
      std::array<std::byte, 4> r0{}, r1{}, r2{};
      if (!reader.read_raw_bytes(r0) || !reader.read_raw_bytes(r1) || !reader.read_raw_bytes(r2)) {
        result.error = "LZX uncompressed block header is truncated.";
        return result;
      }
      const auto le32 = [](const std::array<std::byte, 4>& b) {
        return static_cast<std::uint32_t>(std::to_integer<unsigned>(b[0])) |
               (static_cast<std::uint32_t>(std::to_integer<unsigned>(b[1])) << 8) |
               (static_cast<std::uint32_t>(std::to_integer<unsigned>(b[2])) << 16) |
               (static_cast<std::uint32_t>(std::to_integer<unsigned>(b[3])) << 24);
      };
      state.repeated_offsets = {le32(r0), le32(r1), le32(r2)};
      if (!reader.read_raw_bytes(output.subspan(out_pos, block_size))) {
        result.error = "LZX uncompressed block payload is truncated.";
        return result;
      }
      out_pos += block_size;
      if (block_size % 2u != 0u) {
        std::array<std::byte, 1> pad{};
        (void)reader.read_raw_bytes(pad);  // Padding byte; ignored if absent at EOF.
      }
    } else if (block_type == 1u || block_type == 2u) {
      const bool aligned = block_type == 2u;
      HuffmanTable aligned_tree;
      if (aligned) {
        std::array<std::uint8_t, kAlignedTreeSize> aligned_lengths{};
        for (auto& len : aligned_lengths) len = static_cast<std::uint8_t>(reader.read(3));
        if (!aligned_tree.build(aligned_lengths)) {
          result.error = "LZX aligned-offset tree is invalid.";
          return result;
        }
      }

      if (!read_pretree_and_lengths(reader, state.main_lengths, 0, kNumChars) ||
          !read_pretree_and_lengths(reader, state.main_lengths,
                                    static_cast<std::size_t>(kNumChars),
                                    static_cast<std::size_t>(state.main_tree_size)) ||
          !read_pretree_and_lengths(reader, state.length_lengths, 0,
                                    static_cast<std::size_t>(kLengthTreeSize))) {
        result.error = "LZX tree transmission is malformed.";
        return result;
      }

      HuffmanTable main_tree, length_tree;
      if (!main_tree.build(state.main_lengths) || !length_tree.build(state.length_lengths)) {
        result.error = "LZX main/length tree has an invalid code-length set.";
        return result;
      }

      const auto block_end = out_pos + block_size;
      while (out_pos < block_end) {
        std::uint32_t symbol = 0;
        if (!main_tree.decode(reader, symbol)) {
          result.error = "LZX main-tree symbol decode failed (truncated/corrupt stream).";
          return result;
        }

        if (symbol < static_cast<std::uint32_t>(kNumChars)) {
          if (out_pos >= output.size()) {
            result.error = "LZX literal overruns the output buffer.";
            return result;
          }
          output[out_pos++] = static_cast<std::byte>(symbol);
        } else {
          const auto match_symbol = symbol - static_cast<std::uint32_t>(kNumChars);
          const auto slot = match_symbol / 8u;
          const auto length_header = match_symbol % 8u;

          std::uint32_t length = 0;
          if (length_header == 7u) {
            std::uint32_t length_symbol = 0;
            if (!length_tree.decode(reader, length_symbol)) {
              result.error = "LZX length-tree symbol decode failed.";
              return result;
            }
            length = static_cast<std::uint32_t>(kMinMatchLength) + 7u + length_symbol;
          } else {
            length = static_cast<std::uint32_t>(kMinMatchLength) + length_header;
          }

          std::uint32_t distance = 0;
          if (slot == 0u) {
            distance = state.repeated_offsets[0];
          } else if (slot == 1u) {
            distance = state.repeated_offsets[1];
            std::swap(state.repeated_offsets[0], state.repeated_offsets[1]);
          } else if (slot == 2u) {
            distance = state.repeated_offsets[2];
            std::swap(state.repeated_offsets[0], state.repeated_offsets[2]);
          } else {
            if (slot >= static_cast<std::uint32_t>(PositionSlots::kMaxSlots)) {
              result.error = "LZX position slot out of range.";
              return result;
            }
            const auto footer_bits = slots.footer_bits[slot];
            std::uint32_t extra = 0;
            if (aligned && footer_bits >= 3u) {
              extra = reader.read(static_cast<int>(footer_bits) - 3) << 3;
              std::uint32_t aligned_symbol = 0;
              if (!aligned_tree.decode(reader, aligned_symbol)) {
                result.error = "LZX aligned-offset symbol decode failed.";
                return result;
              }
              extra |= aligned_symbol;
            } else {
              extra = reader.read(static_cast<int>(footer_bits));
            }
            const auto raw_distance = slots.base[slot] + extra;
            distance = raw_distance - 2u;
            state.repeated_offsets[2] = state.repeated_offsets[1];
            state.repeated_offsets[1] = state.repeated_offsets[0];
            state.repeated_offsets[0] = distance;
          }

          if (reader.out_of_data()) {
            result.error = "LZX stream truncated decoding a match.";
            return result;
          }
          if (out_pos + length > output.size() ||
              !copy_match(out_pos, distance, length)) {
            result.error = "LZX match references data before available history.";
            return result;
          }
          out_pos += length;
        }

        while (out_pos >= next_chunk_boundary && next_chunk_boundary <= output.size()) {
          reader.align16();
          next_chunk_boundary += kChunkSize;
        }
      }
    } else {
      result.error = "LZX block type is invalid.";
      return result;
    }

    while (out_pos >= next_chunk_boundary && next_chunk_boundary <= output.size()) {
      reader.align16();
      next_chunk_boundary += kChunkSize;
    }
  }

  result.ok = true;
  return result;
}

std::vector<std::byte> encode_uncompressed(std::span<const std::byte> input,
                                           std::uint32_t window_bits) {
  (void)window_bits;
  std::vector<std::byte> out;
  // A minimal bit-writer mirroring BitReader's MSB-first/16-bit-word
  // convention, sufficient for block headers (3 + 24 bits) and 16-bit
  // realignment; uncompressed-block payload/R0-R2 are written as raw bytes.
  std::uint32_t bit_buffer = 0;
  int bit_count = 0;
  auto flush_word_if_ready = [&] {
    while (bit_count >= 16) {
      const auto word = (bit_buffer >> (bit_count - 16)) & 0xFFFFu;
      out.push_back(static_cast<std::byte>(word & 0xFFu));
      out.push_back(static_cast<std::byte>((word >> 8) & 0xFFu));
      bit_count -= 16;
    }
  };
  auto put_bits = [&](std::uint32_t value, int n) {
    bit_buffer = (bit_buffer << n) | (value & ((n < 32 ? (1u << n) : 0u) - 1u));
    bit_count += n;
    flush_word_if_ready();
  };
  auto align16_write = [&] {
    if (bit_count % 16 != 0) {
      put_bits(0u, 16 - (bit_count % 16));
    }
  };

  std::size_t offset = 0;
  while (offset < input.size() || out.empty()) {
    const auto remaining = input.size() - offset;
    const std::uint32_t block_size =
        static_cast<std::uint32_t>(std::min<std::size_t>(remaining, kChunkSize));
    put_bits(3u, 3);                       // block type = uncompressed
    put_bits((block_size >> 16) & 0xFFu, 8);
    put_bits(block_size & 0xFFFFu, 16);
    align16_write();

    const std::array<std::uint32_t, 3> r = {1u, 1u, 1u};
    for (const auto value : r) {
      out.push_back(static_cast<std::byte>(value & 0xFFu));
      out.push_back(static_cast<std::byte>((value >> 8) & 0xFFu));
      out.push_back(static_cast<std::byte>((value >> 16) & 0xFFu));
      out.push_back(static_cast<std::byte>((value >> 24) & 0xFFu));
    }
    for (std::uint32_t i = 0; i < block_size; ++i) {
      out.push_back(input[offset + i]);
    }
    if (block_size % 2u != 0u) {
      out.push_back(std::byte{0});
    }
    offset += block_size;
    if (block_size == 0u) break;
  }
  return out;
}

namespace {

// MSB-first, 16-bit-word bit writer - the exact inverse of BitReader's
// convention (see BitReader above), used only by
// encode_literal_huffman_block().
class BitWriter {
 public:
  void put_bits(std::uint32_t value, int n) {
    for (int i = n - 1; i >= 0; --i) {
      buffer_ = static_cast<std::uint8_t>((buffer_ << 1) | ((value >> i) & 1u));
      if (++bit_count_ == 8) {
        pending_bytes_.push_back(buffer_);
        buffer_ = 0;
        bit_count_ = 0;
      }
    }
  }

  // Flushes to a whole number of 16-bit words (padding with zero bits),
  // matching BitReader::align16()'s granularity.
  void align16() {
    while (bit_count_ != 0) put_bits(0u, 1);
    while (pending_bytes_.size() % 2u != 0u) pending_bytes_.push_back(0u);
  }

  [[nodiscard]] std::vector<std::byte> take() {
    align16();
    std::vector<std::byte> out;
    out.reserve(pending_bytes_.size());
    // BitReader assembles each 16-bit word as bytes[pos] | (bytes[pos+1]<<8)
    // then consumes it MSB-first; put_bits() above packs bits MSB-first into
    // individual bytes in stream order, so byte order must be swapped in
    // pairs to match BitReader's little-endian word assembly.
    for (std::size_t i = 0; i + 1 < pending_bytes_.size(); i += 2) {
      out.push_back(static_cast<std::byte>(pending_bytes_[i + 1]));
      out.push_back(static_cast<std::byte>(pending_bytes_[i]));
    }
    return out;
  }

 private:
  std::vector<std::uint8_t> pending_bytes_;
  std::uint8_t buffer_{0};
  int bit_count_{0};
};

}  // namespace

std::vector<std::byte> encode_literal_huffman_block(std::span<const std::byte> input,
                                                     std::uint32_t window_bits, bool aligned,
                                                     bool first_block) {
  BitWriter w;
  const auto num_slots = num_position_slots_for_window(window_bits);
  const auto main_tree_size = kNumChars + num_slots * 8;

  w.put_bits(aligned ? 2u : 1u, 3);                        // block type
  w.put_bits((static_cast<std::uint32_t>(input.size()) >> 16) & 0xFFu, 8);
  w.put_bits(static_cast<std::uint32_t>(input.size()) & 0xFFFFu, 16);

  if (aligned) {
    for (int i = 0; i < kAlignedTreeSize; ++i) w.put_bits(0u, 3);  // Unused (no matches emitted).
  }

  // Pretree alphabet: symbol 0 -> delta 0 ("no change" from this slot's
  // persistent previous length), symbol 9 -> delta 9. Against a previous
  // length of 0 (first_block: every slot starts at 0), delta 9 resolves to
  // (0 - 9 + 17) % 17 == 8 (see apply_delta() above) - "set this slot's code
  // length to 8". A 1-bit code for each (canonical order: symbol 0 gets code
  // 0, symbol 9 gets code 1) is a valid, complete prefix code over those two
  // symbols with every other pretree symbol unused.
  const auto emit_pretree_header_and_pass = [&](int used_slot_count, int total_slot_count) {
    std::array<std::uint8_t, kNumPretreeSymbols> pretree_lengths{};
    pretree_lengths[0] = 1;
    pretree_lengths[9] = 1;
    for (const auto len : pretree_lengths) w.put_bits(len, 4);
    for (int i = 0; i < total_slot_count; ++i) {
      w.put_bits(i < used_slot_count ? 1u : 0u, 1);
    }
  };

  // Main tree, literal range [0,256): every literal symbol gets length 8 -
  // with exactly 256 equal-length symbols, first_code[8] == 0, so the
  // canonical code for literal value V is simply V itself (8 raw bits),
  // which is what the data-writing loop below relies on. Code LENGTHS
  // persist across blocks (reset_interval=0), so only the first block of a
  // stream actually needs to *set* length 8 via delta symbol 9 - every
  // later block must signal delta symbol 0 ("no change") to keep it there,
  // exactly as a real encoder would once the tree has already converged.
  emit_pretree_header_and_pass(first_block ? kNumChars : 0, kNumChars);
  // Main tree, match range [256, main_tree_size): entirely unused (no
  // matches are ever emitted by this literal-only encoder) in every block,
  // so always delta 0 regardless of first_block.
  emit_pretree_header_and_pass(0, main_tree_size - kNumChars);
  // Length tree: entirely unused (no length-7-header matches are emitted).
  emit_pretree_header_and_pass(0, kLengthTreeSize);

  for (const auto b : input) {
    w.put_bits(static_cast<std::uint32_t>(std::to_integer<unsigned>(b)), 8);
  }

  return w.take();
}

bool debug_self_test(std::string* error) {
  const auto fail = [&](const char* message) {
    if (error) *error = message;
    return false;
  };

  // 1) Position-slot table: verify the recursively-derived slot count matches
  // the well-known LZX window_bits -> num_position_slots table (30, 32, 34,
  // 36, 38, 42, 50 for window_bits 15..21).
  {
    static constexpr std::array<int, 7> kExpectedSlots = {30, 32, 34, 36, 38, 42, 50};
    for (std::uint32_t bits = 15; bits <= 21; ++bits) {
      const auto got = num_position_slots_for_window(bits);
      if (got != kExpectedSlots[bits - 15]) return fail("position slot count mismatch");
    }
    const auto& slots = position_slots();
    if (slots.base[0] != 0u || slots.base[1] != 1u) return fail("position slot base[0..1] mismatch");
    if (slots.base[30] != 32768u) return fail("position slot base[30] mismatch");
    if (slots.base[50] != 2097152u) return fail("position slot base[50] mismatch");
  }

  // 2) apply_delta(): the pretree delta formula, checked against hand
  // computed values.
  {
    if (apply_delta(0, 0) != 0) return fail("apply_delta(0,0) mismatch");
    if (apply_delta(5, 0) != 5) return fail("apply_delta(5,0) mismatch");
    if (apply_delta(0, 1) != 16) return fail("apply_delta(0,1) mismatch");   // (0-1+17)%17 = 16
    if (apply_delta(3, 5) != 15) return fail("apply_delta(3,5) mismatch");  // (3-5+17)%17 = 15
  }

  // 3) Canonical Huffman table: 4 symbols, all length 2 -> codes 00/01/10/11
  // assigned in ascending symbol order. Bitstream {0x00, 0x1B} is the 16-bit
  // LE word 0x1B00 = 0b0001'1011'0000'0000, whose top 8 bits are exactly the
  // four 2-bit codes concatenated MSB-first, matching LZX's bit order.
  {
    const std::array<std::uint8_t, 4> lengths = {2, 2, 2, 2};
    HuffmanTable table;
    if (!table.build(lengths)) return fail("HuffmanTable::build rejected a valid length set");

    const std::array<std::byte, 2> stream = {std::byte{0x00}, std::byte{0x1B}};
    BitReader reader(stream);
    for (std::uint32_t expected = 0; expected < 4; ++expected) {
      std::uint32_t symbol = 0;
      if (!table.decode(reader, symbol)) return fail("HuffmanTable::decode failed unexpectedly");
      if (symbol != expected) return fail("HuffmanTable::decode produced the wrong symbol");
    }
  }

  // 4) An over-subscribed length set (two symbols both claiming the single
  // length-1 code space alongside more codes than fit) must be rejected.
  {
    const std::array<std::uint8_t, 3> bad_lengths = {1, 1, 1};  // 3 codes can't fit in 2^1.
    HuffmanTable table;
    if (table.build(bad_lengths)) return fail("HuffmanTable::build accepted an over-subscribed code");
  }

  return true;
}

DeltaPatchResult apply_delta_patch_records(std::span<const std::byte> records,
                                           std::uint32_t window_bits,
                                           std::span<std::byte> dest) {
  DeltaPatchResult result{};
  constexpr std::size_t kRecordHeaderSize = 12u;

  std::size_t cursor = 0u;
  while (cursor + kRecordHeaderSize <= records.size()) {
    const auto old_addr = delta_read_be32(records, cursor + 0u);
    const auto new_addr = delta_read_be32(records, cursor + 4u);
    const auto uncompressed_len = delta_read_be16(records, cursor + 8u);
    const auto compressed_len = delta_read_be16(records, cursor + 10u);

    if (old_addr == 0u && new_addr == 0u && uncompressed_len == 0u && compressed_len == 0u) {
      result.ok = true;
      return result;
    }

    const auto fits_dest = [&](std::uint32_t addr, std::uint32_t length) {
      return static_cast<std::uint64_t>(addr) + static_cast<std::uint64_t>(length) <= dest.size();
    };

    if (compressed_len == 0u) {
      // Sentinel: fill with zero, no payload bytes follow.
      if (!fits_dest(new_addr, uncompressed_len)) {
        result.error = "XEXP delta-patch fill record is out of bounds.";
        return result;
      }
      std::memset(dest.data() + new_addr, 0, uncompressed_len);
      cursor += kRecordHeaderSize;
    } else if (compressed_len == 1u) {
      // Sentinel: verbatim copy old_addr -> new_addr, no payload bytes
      // follow. Snapshot first since the ranges may overlap.
      if (!fits_dest(old_addr, uncompressed_len) || !fits_dest(new_addr, uncompressed_len)) {
        result.error = "XEXP delta-patch copy record is out of bounds.";
        return result;
      }
      std::vector<std::byte> snapshot(dest.begin() + old_addr, dest.begin() + old_addr + uncompressed_len);
      std::memcpy(dest.data() + new_addr, snapshot.data(), snapshot.size());
      cursor += kRecordHeaderSize;
    } else {
      // Real LZX-compressed chunk.
      if (cursor + kRecordHeaderSize + compressed_len > records.size()) {
        result.error = "XEXP delta-patch record payload is truncated.";
        return result;
      }
      if (!fits_dest(old_addr, uncompressed_len) || !fits_dest(new_addr, uncompressed_len)) {
        result.error = "XEXP delta-patch LZX record is out of bounds.";
        return result;
      }
      std::vector<std::byte> reference(dest.begin() + old_addr, dest.begin() + old_addr + uncompressed_len);
      std::vector<std::byte> output(static_cast<std::size_t>(uncompressed_len), std::byte{0});
      const auto payload = records.subspan(cursor + kRecordHeaderSize, compressed_len);
      const auto decoded = decode(payload, window_bits, reference, output);
      if (!decoded.ok) {
        result.error = "XEXP delta-patch LZX record failed: " + decoded.error;
        return result;
      }
      std::memcpy(dest.data() + new_addr, output.data(), output.size());
      cursor += kRecordHeaderSize + compressed_len;
    }
  }

  result.ok = true;
  return result;
}

}  // namespace xenon::xbox::lzx

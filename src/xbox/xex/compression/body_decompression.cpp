// Encrypted/compressed body to effective image: basic, LZX and delta.

#include <algorithm>
#include <array>
#include <iomanip>
#include <limits>
#include <sstream>

#include "xbox/xex/xex_loader_internal.hpp"
#include "xenon/xbox/xex_crypto.hpp"
#include "xenon/xbox/xex_lzx.hpp"

namespace xenon::xbox::detail {
namespace {

// XEX_COMPRESSION_BASIC: a sequence of (data_size, zero_size) pairs. The
// block table itself lives in the XEX *header* (inside the
// xex2_opt_file_format_info optional header, `header_bytes` /
// `basic_info_offset`), immediately after the info_size/encryption/
// compression fields; the bytes it describes are read from the decrypted
// *body* (`body`): `data_size` bytes copied verbatim, followed by
// `zero_size` zero bytes, repeating until the block table is exhausted.
bool decompress_basic(std::span<const std::byte> header_bytes, std::size_t basic_info_offset,
                      std::size_t basic_info_size, std::span<const std::byte> body,
                      std::vector<std::byte>& out, std::string* error) {
  if (basic_info_size < 8u) {
    if (error) *error = "XEX basic-compression info block is too small.";
    return false;
  }
  const auto block_count = (basic_info_size - 8u) / 8u;
  std::size_t src = 0u;
  out.clear();
  for (std::size_t i = 0u; i < block_count; ++i) {
    const auto entry_offset = basic_info_offset + 8u + i * 8u;
    if (!range_valid(entry_offset, 8u, header_bytes.size())) {
      if (error) *error = "XEX basic-compression block table is truncated.";
      return false;
    }
    const auto data_size = read_be32(header_bytes, entry_offset);
    const auto zero_size = read_be32(header_bytes, entry_offset + 4u);
    if (!range_valid(src, data_size, body.size())) {
      if (error) *error = "XEX basic-compression block overruns the file body.";
      return false;
    }
    const auto span = body.subspan(src, data_size);
    out.insert(out.end(), span.begin(), span.end());
    out.insert(out.end(), static_cast<std::size_t>(zero_size), std::byte{0});
    src += data_size;
  }
  return true;
}

// XEX_COMPRESSION_NORMAL: each block's payload is a sequence of
// 2-byte-length-prefixed chunks (0 = end of block) whose concatenated
// payload is one continuous raw LZX bitstream.
bool degather_compressed_blocks(std::span<const std::byte> body, std::vector<std::byte>& out_bitstream,
                                std::string* error) {
  out_bitstream.clear();
  return walk_compressed_block_chain(
      body,
      [&](std::span<const std::byte> payload) {
        std::size_t chunk_cursor = 0u;
        while (true) {
          if (!range_valid(chunk_cursor, 2u, payload.size())) {
            if (error) *error = "XEX compressed-block chunk table is truncated.";
            return false;
          }
          const auto chunk_size = read_be16(payload, chunk_cursor);
          chunk_cursor += 2u;
          if (chunk_size == 0u) break;
          if (!range_valid(chunk_cursor, chunk_size, payload.size())) {
            if (error) *error = "XEX compressed-block chunk overruns its block.";
            return false;
          }
          const auto chunk = payload.subspan(chunk_cursor, chunk_size);
          out_bitstream.insert(out_bitstream.end(), chunk.begin(), chunk.end());
          chunk_cursor += chunk_size;
        }
        return true;
      },
      error);
}

}  // namespace

// The xex2_compressed_block_info outer chain (4-byte big-endian block_size +
// 20-byte SHA1 digest, block_size counting the header itself, terminated by
// a bare 4-byte zero block_size) is shared, byte-for-byte, by both
// XEX_COMPRESSION_NORMAL and XEX_COMPRESSION_DELTA bodies - only the
// *payload interpretation* differs between them (see degather_compressed_
// blocks() vs apply_image_delta() below). This walks the chain once,
// validating each block's hash over its payload, and invokes `on_payload`
// with each validated payload in order; `on_payload` returns false (and
// must set *error itself) to abort the walk.
bool walk_compressed_block_chain(std::span<const std::byte> body,
                                 const std::function<bool(std::span<const std::byte>)>& on_payload,
                                 std::string* error) {
  std::size_t cursor = 0u;
  while (true) {
    // The chain terminator is a bare 4-byte zero block_size field - it does
    // not need a full 24-byte header (hash + payload) to follow, so only
    // require those 4 bytes until we know there is an actual block here.
    if (!range_valid(cursor, 4u, body.size())) {
      if (error) *error = "XEX compressed-block chain is truncated.";
      return false;
    }
    const auto block_size = read_be32(body, cursor);
    if (block_size == 0u) break;  // Terminator block.
    if (block_size < 24u || !range_valid(cursor, block_size, body.size())) {
      if (error) *error = "XEX compressed-block size is invalid.";
      return false;
    }
    std::array<std::byte, 20> stored_hash{};
    read_array(body, cursor + 4u, stored_hash);

    const auto payload = body.subspan(cursor + 24u, block_size - 24u);
    const auto computed_hash = crypto::sha1(payload);
    if (!std::equal(computed_hash.begin(), computed_hash.end(), stored_hash.begin())) {
      if (error) *error = "XEX compressed-block SHA1 digest mismatch (corrupt or truncated data).";
      return false;
    }

    if (!on_payload(payload)) return false;

    cursor += block_size;
  }
  return true;
}

// Applies encryption then compression to reconstruct the effective image,
// then verifies it against security_info's own first-page digest
// (verify_first_page_digest() above) - real cryptographic proof that both
// the correct key and the correct decompression were used, for every
// compression type uniformly (not just Normal/Delta's incidental per-block
// SHA1 checks). A wrong key is rejected here rather than merely happening
// to fail PE parsing later, closing the gap where XEX_COMPRESSION_NONE/
// BASIC bodies previously had no integrity check at all. On failure with
// the retail key, retries once with the devkit key before giving up.
bool decompress_body(std::span<const std::byte> bytes, std::uint32_t header_size,
                     const XexSecurityInfo& security, const FileFormatInfo& format,
                     std::span<const std::byte> reference_image, const XexDeltaPatchDescriptor& delta_patch,
                     std::vector<std::byte>& out_effective, std::string* error) {
  if (header_size > bytes.size()) {
    if (error) *error = "XEX header_size exceeds the file size.";
    return false;
  }
  const auto encrypted_body = bytes.subspan(header_size);
  const std::uint32_t target_size =
      security.image_size != 0u ? security.image_size : static_cast<std::uint32_t>(encrypted_body.size());

  const auto attempt = [&](const crypto::AesKey& wrapping_key, std::string& attempt_error) -> bool {
    std::vector<std::byte> plain;
    if (format.encryption == XexEncryptionType::None) {
      plain.assign(encrypted_body.begin(), encrypted_body.end());
    } else {
      if (encrypted_body.size() % 16u != 0u) {
        attempt_error = "XEX encrypted body is not AES-block aligned.";
        return false;
      }
      crypto::AesKey encrypted_image_key{};
      for (std::size_t i = 0; i < 16; ++i) encrypted_image_key[i] = security.encrypted_image_key[i];
      const auto image_key = crypto::unwrap_image_key(wrapping_key, encrypted_image_key);
      plain.resize(encrypted_body.size());
      if (!crypto::aes128_cbc_decrypt(image_key, encrypted_body, plain)) {
        attempt_error = "XEX AES-CBC decryption failed.";
        return false;
      }
    }

    std::vector<std::byte> decoded_effective;
    switch (format.compression) {
      case XexCompressionType::None: {
        if (plain.size() < target_size) {
          attempt_error = "XEX uncompressed body is smaller than the declared image size.";
          return false;
        }
        decoded_effective.assign(plain.begin(), plain.begin() + target_size);
        break;
      }
      case XexCompressionType::Basic: {
        if (!decompress_basic(bytes, format.info_offset, format.info_size, plain, decoded_effective,
                              &attempt_error)) {
          return false;
        }
        break;
      }
      case XexCompressionType::Normal: {
        std::uint32_t window_bits = 0;
        for (auto size = format.window_size; size > 1u; size >>= 1u) ++window_bits;
        if (!lzx::valid_window_bits(window_bits)) {
          attempt_error = "XEX LZX window size is invalid.";
          return false;
        }
        std::vector<std::byte> bitstream;
        if (!degather_compressed_blocks(plain, bitstream, &attempt_error)) return false;
        decoded_effective.assign(static_cast<std::size_t>(target_size), std::byte{0});
        const auto decoded = lzx::decode(bitstream, window_bits, reference_image, decoded_effective);
        if (!decoded.ok) {
          attempt_error = "XEX LZX decompression failed: " + decoded.error;
          return false;
        }
        break;
      }
      case XexCompressionType::Delta: {
        // XEXP image/data delta: real xex2_delta_patch record-chain
        // framing (apply_image_delta() above), not XEX_COMPRESSION_NORMAL's
        // chunk-table-over-a-continuous-bitstream framing - the two share
        // only the outer xex2_compressed_block_info container.
        std::uint32_t window_bits = 0;
        for (auto size = format.window_size; size > 1u; size >>= 1u) ++window_bits;
        if (!lzx::valid_window_bits(window_bits)) {
          attempt_error = "XEX LZX window size is invalid.";
          return false;
        }
        if (!apply_image_delta(plain, window_bits, reference_image, delta_patch, target_size,
                               decoded_effective, &attempt_error)) {
          return false;
        }
        break;
      }
      default:
        attempt_error = "XEX compression type is not supported.";
        return false;
    }

    if (!verify_first_page_digest(security, decoded_effective, attempt_error)) {
      return false;
    }
    out_effective = std::move(decoded_effective);
    return true;
  };

  std::string retail_error;
  if (attempt(crypto::retail_key(), retail_error)) return true;
  if (format.encryption == XexEncryptionType::None) {
    if (error) *error = retail_error;
    return false;
  }
  std::string devkit_error;
  if (attempt(crypto::devkit_key(), devkit_error)) return true;
  if (error) {
    *error = "XEX body decode failed with both retail and devkit keys (retail: " + retail_error +
             "; devkit: " + devkit_error + ").";
  }
  return false;
}

}  // namespace xenon::xbox::detail

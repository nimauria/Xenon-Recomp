#include <array>
#include <cassert>
#include <cstddef>
#include <string>
#include <vector>

#include "xenon/xbox/xex_crypto.hpp"

namespace {

using xenon::xbox::crypto::AesBlock;
using xenon::xbox::crypto::AesKey;

AesKey make_key(std::initializer_list<unsigned> bytes) {
  AesKey key{};
  std::size_t i = 0;
  for (auto b : bytes) key[i++] = static_cast<std::byte>(b);
  return key;
}

AesBlock make_block(std::initializer_list<unsigned> bytes) {
  AesBlock block{};
  std::size_t i = 0;
  for (auto b : bytes) block[i++] = static_cast<std::byte>(b);
  return block;
}

// FIPS-197 Appendix B / C.1 AES-128 known-answer vector.
void test_fips197_known_vector() {
  const auto key =
      make_key({0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f});
  const auto plaintext =
      make_block({0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff});
  const auto expected_ciphertext =
      make_block({0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a});

  AesBlock ciphertext{};
  xenon::xbox::crypto::aes128_encrypt_block(key, plaintext, ciphertext);
  assert(ciphertext == expected_ciphertext);

  AesBlock decrypted{};
  xenon::xbox::crypto::aes128_decrypt_block(key, ciphertext, decrypted);
  assert(decrypted == plaintext);
}

// NIST SP800-38A F.2.1 AES-128-CBC vector (key + IV + one 16-byte block).
// Our aes128_cbc_decrypt always uses a zero IV (the XEX convention), so this
// vector is adapted: encrypt-then-decrypt round trip with our own
// aes128_cbc_encrypt, plus a hand-computed single-block zero-IV case that
// reduces to plain ECB (verified against the FIPS-197 vector above).
void test_cbc_round_trip() {
  const auto key = make_key({0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c});
  std::vector<std::byte> plaintext(64);
  for (std::size_t i = 0; i < plaintext.size(); ++i) {
    plaintext[i] = static_cast<std::byte>(i * 7u + 3u);
  }
  std::vector<std::byte> ciphertext(plaintext.size());
  xenon::xbox::crypto::aes128_cbc_encrypt(key, plaintext, ciphertext);

  std::vector<std::byte> decrypted(plaintext.size());
  const bool ok = xenon::xbox::crypto::aes128_cbc_decrypt(key, ciphertext, decrypted);
  assert(ok);
  assert(decrypted == plaintext);

  // Zero-IV CBC of a single block must equal plain ECB decryption.
  AesBlock single_cipher{};
  for (std::size_t i = 0; i < 16; ++i) single_cipher[i] = ciphertext[i];
  AesBlock ecb_plain{};
  xenon::xbox::crypto::aes128_decrypt_block(key, single_cipher, ecb_plain);
  for (std::size_t i = 0; i < 16; ++i) assert(ecb_plain[i] == decrypted[i]);
}

void test_cbc_rejects_misaligned_input() {
  const AesKey key{};
  std::vector<std::byte> input(15);
  std::vector<std::byte> output(15);
  assert(!xenon::xbox::crypto::aes128_cbc_decrypt(key, input, output));
}

void test_unwrap_image_key_round_trip() {
  // Simulate how a XEX's security_info.encrypted_image_key is produced:
  // AES-ECB-encrypt the real per-title image key with the fixed retail key.
  const auto& wrapping_key = xenon::xbox::crypto::retail_key();
  const auto real_image_key =
      make_key({0xde, 0xad, 0xbe, 0xef, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c});

  AesBlock wrapped{};
  xenon::xbox::crypto::aes128_encrypt_block(wrapping_key, real_image_key, wrapped);

  const auto unwrapped = xenon::xbox::crypto::unwrap_image_key(wrapping_key, wrapped);
  assert(unwrapped == real_image_key);

  // Unwrapping with the wrong fixed key must not silently succeed.
  const auto wrong_unwrap = xenon::xbox::crypto::unwrap_image_key(xenon::xbox::crypto::devkit_key(), wrapped);
  assert(wrong_unwrap != real_image_key);
}

// FIPS-180-1 SHA-1 known-answer vectors.
void test_sha1_known_vectors() {
  {
    const std::string input = "abc";
    std::vector<std::byte> bytes(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) bytes[i] = static_cast<std::byte>(input[i]);
    const auto digest = xenon::xbox::crypto::sha1(bytes);
    const auto expected = make_block({0xA9, 0x99, 0x3E, 0x36, 0x47, 0x06, 0x81, 0x6A, 0xBA, 0x3E, 0x25, 0x71, 0x78, 0x50, 0xC2, 0x6C});
    // SHA-1 digest is 20 bytes; compare the first 16 via AesBlock helper then
    // the trailing 4 explicitly.
    for (std::size_t i = 0; i < 16; ++i) assert(digest[i] == expected[i]);
    const std::array<unsigned char, 4> tail = {0x9C, 0xD0, 0xD8, 0x9D};
    for (std::size_t i = 0; i < 4; ++i) assert(digest[16 + i] == static_cast<std::byte>(tail[i]));
  }
  {
    // SHA1("") = da39a3ee5e6b4b0d3255bfef95601890afd80709
    std::vector<std::byte> empty;
    const auto digest = xenon::xbox::crypto::sha1(empty);
    const auto expected = make_block({0xDA, 0x39, 0xA3, 0xEE, 0x5E, 0x6B, 0x4B, 0x0D, 0x32, 0x55, 0xBF, 0xEF, 0x95, 0x60, 0x18, 0x90});
    for (std::size_t i = 0; i < 16; ++i) assert(digest[i] == expected[i]);
    const std::array<unsigned char, 4> tail = {0xAF, 0xD8, 0x07, 0x09};
    for (std::size_t i = 0; i < 4; ++i) assert(digest[16 + i] == static_cast<std::byte>(tail[i]));
  }
}

}  // namespace

int main() {
  test_fips197_known_vector();
  test_cbc_round_trip();
  test_cbc_rejects_misaligned_input();
  test_unwrap_image_key_round_trip();
  test_sha1_known_vectors();
  return 0;
}

#include "xenon/xbox/xex_crypto.hpp"

#include <array>
#include <cstring>
#include <vector>

namespace xenon::xbox::crypto {
namespace {

// ---------------------------------------------------------------------------
// AES-128 (FIPS-197). Straightforward table-based implementation: this is a
// well-known, deterministic algorithm and is validated against the standard
// FIPS-197 known-answer test vectors in tests/xbox/xex_crypto_tests.cpp.
// S-box and inverse S-box are built at static-init time from a direct GF(2^8)
// multiplicative-inverse computation (no hand-transcribed lookup table).
// ---------------------------------------------------------------------------

struct SBoxTables {
  std::array<std::uint8_t, 256> sbox{};
  std::array<std::uint8_t, 256> inv_sbox{};
};

std::uint8_t gf_mul(std::uint8_t a, std::uint8_t b) {
  std::uint8_t result = 0;
  for (int i = 0; i < 8; ++i) {
    if (b & 1) result ^= a;
    const bool hi = (a & 0x80) != 0;
    a = static_cast<std::uint8_t>(a << 1);
    if (hi) a = static_cast<std::uint8_t>(a ^ 0x1B);
    b = static_cast<std::uint8_t>(b >> 1);
  }
  return result;
}

std::uint8_t gf_inverse(std::uint8_t value) {
  if (value == 0) return 0;
  // GF(2^8) has 255 non-zero elements; a^254 == a^-1 for a != 0.
  std::uint8_t result = 1;
  std::uint8_t base = value;
  std::uint32_t exponent = 254;
  while (exponent != 0) {
    if (exponent & 1) result = gf_mul(result, base);
    base = gf_mul(base, base);
    exponent >>= 1;
  }
  return result;
}

const SBoxTables& sbox_tables() {
  static const SBoxTables tables = [] {
    SBoxTables t{};
    for (int i = 0; i < 256; ++i) {
      const auto inv = gf_inverse(static_cast<std::uint8_t>(i));
      // Affine transform: b_i = inv_i ^ inv_(i+4) ^ inv_(i+5) ^ inv_(i+6) ^
      // inv_(i+7) ^ c_i, with c = 0x63, bits taken modulo 8 (rotations).
      std::uint8_t x = inv;
      std::uint8_t rot1 = static_cast<std::uint8_t>((x << 1) | (x >> 7));
      std::uint8_t rot2 = static_cast<std::uint8_t>((x << 2) | (x >> 6));
      std::uint8_t rot3 = static_cast<std::uint8_t>((x << 3) | (x >> 5));
      std::uint8_t rot4 = static_cast<std::uint8_t>((x << 4) | (x >> 4));
      std::uint8_t s = static_cast<std::uint8_t>(x ^ rot1 ^ rot2 ^ rot3 ^ rot4 ^ 0x63);
      t.sbox[static_cast<std::size_t>(i)] = s;
    }
    for (int i = 0; i < 256; ++i) {
      t.inv_sbox[t.sbox[static_cast<std::size_t>(i)]] = static_cast<std::uint8_t>(i);
    }
    return t;
  }();
  return tables;
}

constexpr int kNb = 4;   // words per state block
constexpr int kNk = 4;   // words in AES-128 key
constexpr int kNr = 10;  // AES-128 rounds

using RoundKeys = std::array<std::uint8_t, 4 * kNb * (kNr + 1)>;

std::uint8_t rcon(int i) {
  static constexpr std::array<std::uint8_t, 11> kRcon = {
      0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36};
  return kRcon[static_cast<std::size_t>(i)];
}

RoundKeys expand_key(const AesKey& key) {
  const auto& sbox = sbox_tables().sbox;
  RoundKeys w{};
  std::memcpy(w.data(), key.data(), 16);

  std::array<std::uint8_t, 4> temp{};
  for (int i = kNk; i < kNb * (kNr + 1); ++i) {
    const std::uint8_t* prev = &w[static_cast<std::size_t>((i - 1) * 4)];
    temp = {prev[0], prev[1], prev[2], prev[3]};
    if (i % kNk == 0) {
      // RotWord + SubWord + Rcon.
      const std::uint8_t t0 = temp[0];
      temp[0] = static_cast<std::uint8_t>(sbox[temp[1]] ^ rcon(i / kNk));
      temp[1] = sbox[temp[2]];
      temp[2] = sbox[temp[3]];
      temp[3] = sbox[t0];
    }
    const std::uint8_t* earlier = &w[static_cast<std::size_t>((i - kNk) * 4)];
    std::uint8_t* dest = &w[static_cast<std::size_t>(i * 4)];
    for (int b = 0; b < 4; ++b) {
      dest[b] = static_cast<std::uint8_t>(earlier[b] ^ temp[static_cast<std::size_t>(b)]);
    }
  }
  return w;
}

using State = std::array<std::uint8_t, 16>;  // column-major, state[c*4+r]

void add_round_key(State& state, const RoundKeys& w, int round) {
  for (int c = 0; c < kNb; ++c) {
    for (int r = 0; r < 4; ++r) {
      state[static_cast<std::size_t>(c * 4 + r)] ^=
          w[static_cast<std::size_t>(round * kNb * 4 + c * 4 + r)];
    }
  }
}

void sub_bytes(State& state, const std::array<std::uint8_t, 256>& box) {
  for (auto& byte : state) byte = box[byte];
}

void shift_rows(State& state) {
  State src = state;
  for (int r = 1; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      state[static_cast<std::size_t>(c * 4 + r)] =
          src[static_cast<std::size_t>(((c + r) % 4) * 4 + r)];
    }
  }
}

void inv_shift_rows(State& state) {
  State src = state;
  for (int r = 1; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      state[static_cast<std::size_t>(((c + r) % 4) * 4 + r)] =
          src[static_cast<std::size_t>(c * 4 + r)];
    }
  }
}

void mix_columns(State& state) {
  for (int c = 0; c < 4; ++c) {
    std::uint8_t* col = &state[static_cast<std::size_t>(c * 4)];
    const std::uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
    col[0] = static_cast<std::uint8_t>(gf_mul(a0, 2) ^ gf_mul(a1, 3) ^ a2 ^ a3);
    col[1] = static_cast<std::uint8_t>(a0 ^ gf_mul(a1, 2) ^ gf_mul(a2, 3) ^ a3);
    col[2] = static_cast<std::uint8_t>(a0 ^ a1 ^ gf_mul(a2, 2) ^ gf_mul(a3, 3));
    col[3] = static_cast<std::uint8_t>(gf_mul(a0, 3) ^ a1 ^ a2 ^ gf_mul(a3, 2));
  }
}

void inv_mix_columns(State& state) {
  for (int c = 0; c < 4; ++c) {
    std::uint8_t* col = &state[static_cast<std::size_t>(c * 4)];
    const std::uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
    col[0] = static_cast<std::uint8_t>(gf_mul(a0, 0x0e) ^ gf_mul(a1, 0x0b) ^
                                       gf_mul(a2, 0x0d) ^ gf_mul(a3, 0x09));
    col[1] = static_cast<std::uint8_t>(gf_mul(a0, 0x09) ^ gf_mul(a1, 0x0e) ^
                                       gf_mul(a2, 0x0b) ^ gf_mul(a3, 0x0d));
    col[2] = static_cast<std::uint8_t>(gf_mul(a0, 0x0d) ^ gf_mul(a1, 0x09) ^
                                       gf_mul(a2, 0x0e) ^ gf_mul(a3, 0x0b));
    col[3] = static_cast<std::uint8_t>(gf_mul(a0, 0x0b) ^ gf_mul(a1, 0x0d) ^
                                       gf_mul(a2, 0x09) ^ gf_mul(a3, 0x0e));
  }
}

State load_state(const AesBlock& block) {
  State state{};
  for (std::size_t i = 0; i < 16; ++i) {
    state[i] = static_cast<std::uint8_t>(block[i]);
  }
  return state;
}

void store_state(const State& state, AesBlock& block) {
  for (std::size_t i = 0; i < 16; ++i) {
    block[i] = static_cast<std::byte>(state[i]);
  }
}

}  // namespace

const AesKey& retail_key() noexcept {
  static constexpr AesKey key = {
      std::byte{0x20}, std::byte{0xB1}, std::byte{0x85}, std::byte{0xA5},
      std::byte{0x9D}, std::byte{0x28}, std::byte{0xFD}, std::byte{0xC3},
      std::byte{0x40}, std::byte{0x58}, std::byte{0x3F}, std::byte{0xBB},
      std::byte{0x08}, std::byte{0x96}, std::byte{0xBF}, std::byte{0x91}};
  return key;
}

const AesKey& devkit_key() noexcept {
  static constexpr AesKey key{};  // all-zero
  return key;
}

void aes128_encrypt_block(const AesKey& key, const AesBlock& plaintext,
                          AesBlock& out_ciphertext) noexcept {
  const auto w = expand_key(key);
  const auto& sbox = sbox_tables().sbox;
  State state = load_state(plaintext);

  add_round_key(state, w, 0);
  for (int round = 1; round < kNr; ++round) {
    sub_bytes(state, sbox);
    shift_rows(state);
    mix_columns(state);
    add_round_key(state, w, round);
  }
  sub_bytes(state, sbox);
  shift_rows(state);
  add_round_key(state, w, kNr);

  store_state(state, out_ciphertext);
}

void aes128_decrypt_block(const AesKey& key, const AesBlock& ciphertext,
                          AesBlock& out_plaintext) noexcept {
  const auto w = expand_key(key);
  const auto& inv_sbox = sbox_tables().inv_sbox;
  State state = load_state(ciphertext);

  add_round_key(state, w, kNr);
  for (int round = kNr - 1; round >= 1; --round) {
    inv_shift_rows(state);
    sub_bytes(state, inv_sbox);
    add_round_key(state, w, round);
    inv_mix_columns(state);
  }
  inv_shift_rows(state);
  sub_bytes(state, inv_sbox);
  add_round_key(state, w, 0);

  store_state(state, out_plaintext);
}

bool aes128_cbc_decrypt(const AesKey& key, std::span<const std::byte> input,
                        std::span<std::byte> output) noexcept {
  if (input.size() % 16u != 0u || input.size() != output.size()) {
    return false;
  }
  AesBlock iv{};  // XEX image bodies use a fixed zero IV.
  const auto rk = expand_key(key);
  const auto& inv_sbox = sbox_tables().inv_sbox;
  for (std::size_t offset = 0; offset < input.size(); offset += 16u) {
    AesBlock cipher_block{};
    std::memcpy(cipher_block.data(), input.data() + offset, 16);
    AesBlock plain_block{};

    State state = load_state(cipher_block);
    add_round_key(state, rk, kNr);
    for (int round = kNr - 1; round >= 1; --round) {
      inv_shift_rows(state);
      sub_bytes(state, inv_sbox);
      add_round_key(state, rk, round);
      inv_mix_columns(state);
    }
    inv_shift_rows(state);
    sub_bytes(state, inv_sbox);
    add_round_key(state, rk, 0);
    store_state(state, plain_block);

    for (std::size_t i = 0; i < 16; ++i) {
      plain_block[i] ^= iv[i];
    }
    std::memcpy(output.data() + offset, plain_block.data(), 16);
    iv = cipher_block;
  }
  return true;
}

void aes128_cbc_encrypt(const AesKey& key, std::span<const std::byte> input,
                        std::span<std::byte> output) noexcept {
  AesBlock iv{};
  for (std::size_t offset = 0; offset + 16u <= input.size(); offset += 16u) {
    AesBlock block{};
    std::memcpy(block.data(), input.data() + offset, 16);
    for (std::size_t i = 0; i < 16; ++i) block[i] ^= iv[i];
    AesBlock cipher{};
    aes128_encrypt_block(key, block, cipher);
    std::memcpy(output.data() + offset, cipher.data(), 16);
    iv = cipher;
  }
}

AesKey unwrap_image_key(const AesKey& wrapping_key,
                        const AesKey& encrypted_image_key) noexcept {
  AesKey out{};
  aes128_decrypt_block(wrapping_key, encrypted_image_key, out);
  return out;
}

// ---------------------------------------------------------------------------
// SHA-1 (FIPS-180). Used for XEX compressed-block digests and delta-patch
// base-signature comparisons.
// ---------------------------------------------------------------------------

Sha1Digest sha1(std::span<const std::byte> data) noexcept {
  std::uint32_t h0 = 0x67452301u, h1 = 0xEFCDAB89u, h2 = 0x98BADCFEu,
                h3 = 0x10325476u, h4 = 0xC3D2E1F0u;

  std::vector<std::uint8_t> message(data.size());
  for (std::size_t i = 0; i < data.size(); ++i) {
    message[i] = static_cast<std::uint8_t>(data[i]);
  }
  const std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8u;
  message.push_back(0x80u);
  while (message.size() % 64u != 56u) message.push_back(0u);
  for (int shift = 56; shift >= 0; shift -= 8) {
    message.push_back(static_cast<std::uint8_t>((bit_length >> shift) & 0xFFu));
  }

  for (std::size_t chunk = 0; chunk < message.size(); chunk += 64u) {
    std::array<std::uint32_t, 80> w{};
    for (int i = 0; i < 16; ++i) {
      const std::size_t base = chunk + static_cast<std::size_t>(i * 4);
      w[static_cast<std::size_t>(i)] =
          (static_cast<std::uint32_t>(message[base]) << 24) |
          (static_cast<std::uint32_t>(message[base + 1]) << 16) |
          (static_cast<std::uint32_t>(message[base + 2]) << 8) |
          static_cast<std::uint32_t>(message[base + 3]);
    }
    for (int i = 16; i < 80; ++i) {
      const auto value = w[static_cast<std::size_t>(i - 3)] ^ w[static_cast<std::size_t>(i - 8)] ^
                         w[static_cast<std::size_t>(i - 14)] ^ w[static_cast<std::size_t>(i - 16)];
      w[static_cast<std::size_t>(i)] = (value << 1) | (value >> 31);
    }

    std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
    for (int i = 0; i < 80; ++i) {
      std::uint32_t f, k;
      if (i < 20) {
        f = (b & c) | ((~b) & d);
        k = 0x5A827999u;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      const std::uint32_t temp =
          ((a << 5) | (a >> 27)) + f + e + k + w[static_cast<std::size_t>(i)];
      e = d;
      d = c;
      c = (b << 30) | (b >> 2);
      b = a;
      a = temp;
    }
    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
  }

  Sha1Digest digest{};
  const std::array<std::uint32_t, 5> hs = {h0, h1, h2, h3, h4};
  for (int i = 0; i < 5; ++i) {
    digest[static_cast<std::size_t>(i * 4 + 0)] = static_cast<std::byte>((hs[static_cast<std::size_t>(i)] >> 24) & 0xFFu);
    digest[static_cast<std::size_t>(i * 4 + 1)] = static_cast<std::byte>((hs[static_cast<std::size_t>(i)] >> 16) & 0xFFu);
    digest[static_cast<std::size_t>(i * 4 + 2)] = static_cast<std::byte>((hs[static_cast<std::size_t>(i)] >> 8) & 0xFFu);
    digest[static_cast<std::size_t>(i * 4 + 3)] = static_cast<std::byte>(hs[static_cast<std::size_t>(i)] & 0xFFu);
  }
  return digest;
}

}  // namespace xenon::xbox::crypto

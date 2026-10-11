#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "xenon/core/export_registry.hpp"
#include "xenon/xbox/xboxkrnl_misc_exports.hpp"

// XeCrypt hash and stream-cipher exports titles use for save checksums and
// obfuscation: the incremental SHA-1 and SHA-256 functions, HMAC-SHA1, RC4,
// XeCryptBnQw_SwapDwQwLeBe and RtlComputeCrc32. These are standard
// algorithms (FIPS 180-4, RFC 2104, RC4, CRC-32/zlib); the guest-visible
// parts - argument order and the state structures a title allocates - are
// as xenia and rexglue-sdk lay them out. Each call reads its state from
// guest memory and writes it back, so a title may keep a state anywhere.
namespace xenon::xbox {
namespace {

using core::ExportCallContext;

std::uint32_t arg(ExportCallContext& ctx, int index) {
  return static_cast<std::uint32_t>(ctx.cpu.gpr[3 + index]);
}

std::vector<std::uint8_t> read_bytes(ExportCallContext& ctx, cpu::GuestAddress address,
                                     std::uint32_t length) {
  std::vector<std::uint8_t> bytes(address == 0u ? 0u : length);
  for (std::uint32_t i = 0; i < bytes.size(); ++i) bytes[i] = ctx.memory.read8(address + i);
  return bytes;
}

void write_bytes(ExportCallContext& ctx, cpu::GuestAddress address, const std::uint8_t* bytes,
                 std::size_t length) {
  for (std::size_t i = 0; i < length; ++i) {
    ctx.memory.write8(address + static_cast<std::uint32_t>(i), bytes[i]);
  }
}

std::uint32_t rotl(std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
std::uint32_t rotr(std::uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }

std::uint32_t load_be32(const std::uint8_t* p) {
  return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) |
         std::uint32_t{p[3]};
}

// A resumable Merkle-Damgard hash over 64-byte blocks with a 32-bit byte
// count, as XECRYPT_SHA_STATE and XECRYPT_SHA256_STATE keep it.
template <std::size_t Words>
struct HashState {
  std::uint32_t count{0};
  std::array<std::uint32_t, Words> h{};
  std::array<std::uint8_t, 64> buffer{};
};

void sha1_block(std::array<std::uint32_t, 5>& h, const std::uint8_t* block) {
  std::array<std::uint32_t, 80> w{};
  for (int i = 0; i < 16; ++i) w[i] = load_be32(block + i * 4);
  for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
  for (int i = 0; i < 80; ++i) {
    std::uint32_t f, k;
    if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
    else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
    else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
    const auto t = rotl(a, 5) + f + e + k + w[i];
    e = d; d = c; c = rotl(b, 30); b = a; a = t;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

constexpr std::array<std::uint32_t, 64> kSha256K{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

void sha256_block(std::array<std::uint32_t, 8>& h, const std::uint8_t* block) {
  std::array<std::uint32_t, 64> w{};
  for (int i = 0; i < 16; ++i) w[i] = load_be32(block + i * 4);
  for (int i = 16; i < 64; ++i) {
    const auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  auto v = h;
  for (int i = 0; i < 64; ++i) {
    const auto s1 = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
    const auto ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
    const auto t1 = v[7] + s1 + ch + kSha256K[i] + w[i];
    const auto s0 = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
    const auto maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
    const auto t2 = s0 + maj;
    v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
    v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
  }
  for (int i = 0; i < 8; ++i) h[i] += v[i];
}

template <std::size_t Words, typename Block>
void hash_update(HashState<Words>& s, const std::uint8_t* data, std::size_t length, Block block) {
  for (std::size_t i = 0; i < length; ++i) {
    s.buffer[s.count % 64u] = data[i];
    ++s.count;
    if (s.count % 64u == 0u) block(s.h, s.buffer.data());
  }
}

template <std::size_t Words, typename Block>
std::array<std::uint8_t, Words * 4> hash_final(HashState<Words> s, Block block) {
  const std::uint64_t bits = std::uint64_t{s.count} * 8u;
  const std::uint8_t pad = 0x80u;
  hash_update(s, &pad, 1, block);
  const std::uint8_t zero = 0u;
  while (s.count % 64u != 56u) hash_update(s, &zero, 1, block);
  std::array<std::uint8_t, 8> length{};
  for (int i = 0; i < 8; ++i) length[i] = static_cast<std::uint8_t>(bits >> (56 - i * 8));
  hash_update(s, length.data(), length.size(), block);
  std::array<std::uint8_t, Words * 4> digest{};
  for (std::size_t i = 0; i < Words; ++i) {
    for (int b = 0; b < 4; ++b) digest[i * 4 + b] = static_cast<std::uint8_t>(s.h[i] >> (24 - b * 8));
  }
  return digest;
}

// XECRYPT_SHA_STATE (0x58): count @0, state[5] @4, buffer[64] @0x18.
// XECRYPT_SHA256_STATE: count @0, state[8] @4, buffer[64] @0x24.
template <std::size_t Words>
HashState<Words> load_state(ExportCallContext& ctx, cpu::GuestAddress address) {
  HashState<Words> s{};
  s.count = ctx.memory.read32_be(address);
  for (std::size_t i = 0; i < Words; ++i) {
    s.h[i] = ctx.memory.read32_be(address + 4u + static_cast<std::uint32_t>(i * 4u));
  }
  const auto buffer = address + 4u + static_cast<std::uint32_t>(Words * 4u);
  for (std::uint32_t i = 0; i < 64u; ++i) s.buffer[i] = ctx.memory.read8(buffer + i);
  return s;
}

template <std::size_t Words>
void store_state(ExportCallContext& ctx, cpu::GuestAddress address, const HashState<Words>& s) {
  ctx.memory.write32_be(address, s.count);
  for (std::size_t i = 0; i < Words; ++i) {
    ctx.memory.write32_be(address + 4u + static_cast<std::uint32_t>(i * 4u), s.h[i]);
  }
  write_bytes(ctx, address + 4u + static_cast<std::uint32_t>(Words * 4u), s.buffer.data(), 64u);
}

constexpr std::array<std::uint32_t, 5> kSha1Init{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu,
                                                 0x10325476u, 0xC3D2E1F0u};
constexpr std::array<std::uint32_t, 8> kSha256Init{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u,
                                                   0xa54ff53au, 0x510e527fu, 0x9b05688cu,
                                                   0x1f83d9abu, 0x5be0cd19u};

std::array<std::uint8_t, 20> sha1(const std::vector<std::vector<std::uint8_t>>& parts) {
  HashState<5> s{};
  s.h = kSha1Init;
  for (const auto& part : parts) hash_update(s, part.data(), part.size(), sha1_block);
  return hash_final(s, sha1_block);
}

// XECRYPT_RC4_STATE (0x102): S[256] @0, i @0x100, j @0x101.
void rc4_key(ExportCallContext& ctx, cpu::GuestAddress state, const std::vector<std::uint8_t>& key) {
  std::array<std::uint8_t, 256> s{};
  for (int x = 0; x < 256; ++x) s[x] = static_cast<std::uint8_t>(x);
  std::uint32_t j = 0;
  for (int x = 0; x < 256 && !key.empty(); ++x) {
    j = (j + s[x] + key[x % key.size()]) & 0xFFu;
    std::swap(s[x], s[j]);
  }
  write_bytes(ctx, state, s.data(), s.size());
  ctx.memory.write8(state + 0x100u, 0u);
  ctx.memory.write8(state + 0x101u, 0u);
}

void rc4_crypt(ExportCallContext& ctx, cpu::GuestAddress state, cpu::GuestAddress data,
               std::uint32_t length) {
  auto s = read_bytes(ctx, state, 256u);
  std::uint8_t i = ctx.memory.read8(state + 0x100u);
  std::uint8_t j = ctx.memory.read8(state + 0x101u);
  for (std::uint32_t n = 0; n < length; ++n) {
    i = static_cast<std::uint8_t>(i + 1u);
    j = static_cast<std::uint8_t>(j + s[i]);
    std::swap(s[i], s[j]);
    const auto k = s[static_cast<std::uint8_t>(s[i] + s[j])];
    ctx.memory.write8(data + n, static_cast<std::uint8_t>(ctx.memory.read8(data + n) ^ k));
  }
  write_bytes(ctx, state, s.data(), s.size());
  ctx.memory.write8(state + 0x100u, i);
  ctx.memory.write8(state + 0x101u, j);
}

void hash_write_out(ExportCallContext& ctx, cpu::GuestAddress out, std::uint32_t out_size,
                    const std::uint8_t* digest, std::size_t digest_size) {
  if (out != 0u) write_bytes(ctx, out, digest, std::min<std::size_t>(digest_size, out_size));
}

struct CryptExport {
  std::uint32_t ordinal;
  const char* name;
  bool (*handler)(ExportCallContext&);
};

const CryptExport kExports[] = {
    // XeCryptShaInit(state)
    {0x18Fu, "XeCryptShaInit",
     [](ExportCallContext& ctx) {
       HashState<5> s{};
       s.h = kSha1Init;
       store_state(ctx, arg(ctx, 0), s);
       return true;
     }},
    // XeCryptShaUpdate(state, input, input_size)
    {0x190u, "XeCryptShaUpdate",
     [](ExportCallContext& ctx) {
       auto s = load_state<5>(ctx, arg(ctx, 0));
       const auto input = read_bytes(ctx, arg(ctx, 1), arg(ctx, 2));
       hash_update(s, input.data(), input.size(), sha1_block);
       store_state(ctx, arg(ctx, 0), s);
       return true;
     }},
    // XeCryptShaFinal(state, out, out_size): the first min(20, out_size)
    // digest bytes.
    {0x191u, "XeCryptShaFinal",
     [](ExportCallContext& ctx) {
       const auto digest = hash_final(load_state<5>(ctx, arg(ctx, 0)), sha1_block);
       hash_write_out(ctx, arg(ctx, 1), arg(ctx, 2), digest.data(), digest.size());
       return true;
     }},
    // XeCryptSha256Init/Update/Final, as the SHA-1 functions.
    {0x310u, "XeCryptSha256Init",
     [](ExportCallContext& ctx) {
       HashState<8> s{};
       s.h = kSha256Init;
       store_state(ctx, arg(ctx, 0), s);
       return true;
     }},
    {0x311u, "XeCryptSha256Update",
     [](ExportCallContext& ctx) {
       auto s = load_state<8>(ctx, arg(ctx, 0));
       const auto input = read_bytes(ctx, arg(ctx, 1), arg(ctx, 2));
       hash_update(s, input.data(), input.size(), sha256_block);
       store_state(ctx, arg(ctx, 0), s);
       return true;
     }},
    {0x312u, "XeCryptSha256Final",
     [](ExportCallContext& ctx) {
       const auto digest = hash_final(load_state<8>(ctx, arg(ctx, 0)), sha256_block);
       hash_write_out(ctx, arg(ctx, 1), arg(ctx, 2), digest.data(), digest.size());
       return true;
     }},
    // XeCryptHmacSha(key, key_size, in1, in1_size, in2, in2_size, in3,
    // in3_size, out, out_size): HMAC-SHA1 over the concatenated inputs
    // (RFC 2104; a key over 64 bytes is hashed first), the first
    // min(20, out_size) bytes written. out and out_size are the ninth and
    // tenth arguments, in the caller's parameter area at r1+0x54 and
    // r1+0x5C.
    {0x182u, "XeCryptHmacSha",
     [](ExportCallContext& ctx) {
       auto key = read_bytes(ctx, arg(ctx, 0), arg(ctx, 1));
       if (key.size() > 64u) {
         const auto hashed = sha1({key});
         key.assign(hashed.begin(), hashed.end());
       }
       std::vector<std::uint8_t> inner(64u, 0x36u), outer(64u, 0x5Cu);
       for (std::size_t i = 0; i < key.size(); ++i) {
         inner[i] ^= key[i];
         outer[i] ^= key[i];
       }
       const auto inner_digest =
           sha1({inner, read_bytes(ctx, arg(ctx, 2), arg(ctx, 3)),
                 read_bytes(ctx, arg(ctx, 4), arg(ctx, 5)), read_bytes(ctx, arg(ctx, 6), arg(ctx, 7))});
       const auto digest = sha1({outer, {inner_digest.begin(), inner_digest.end()}});
       const auto sp = static_cast<std::uint32_t>(ctx.cpu.gpr[1]);
       hash_write_out(ctx, ctx.memory.read32_be(sp + 0x54u), ctx.memory.read32_be(sp + 0x5Cu),
                      digest.data(), digest.size());
       return true;
     }},
    // XeCryptRc4Key(state, key, key_size). xenia and rexglue-sdk index the
    // key as key[x % 16], which is RC4 only for 16-byte keys; Xenon uses
    // the key's own length, as RC4 is defined.
    {0x18Bu, "XeCryptRc4Key",
     [](ExportCallContext& ctx) {
       rc4_key(ctx, arg(ctx, 0), read_bytes(ctx, arg(ctx, 1), arg(ctx, 2)));
       return true;
     }},
    // XeCryptRc4Ecb(state, data, size): en/decrypts in place.
    {0x18Cu, "XeCryptRc4Ecb",
     [](ExportCallContext& ctx) {
       rc4_crypt(ctx, arg(ctx, 0), arg(ctx, 1), arg(ctx, 2));
       return true;
     }},
    // XeCryptRc4(key, key_size, data, size): one-shot, with a state on the
    // host side.
    {0x18Du, "XeCryptRc4",
     [](ExportCallContext& ctx) {
       const auto key = read_bytes(ctx, arg(ctx, 0), arg(ctx, 1));
       std::array<std::uint8_t, 256> s{};
       for (int x = 0; x < 256; ++x) s[x] = static_cast<std::uint8_t>(x);
       std::uint32_t j = 0;
       for (int x = 0; x < 256 && !key.empty(); ++x) {
         j = (j + s[x] + key[x % key.size()]) & 0xFFu;
         std::swap(s[x], s[j]);
       }
       std::uint8_t i = 0, jj = 0;
       const auto data = arg(ctx, 2);
       for (std::uint32_t n = 0; n < arg(ctx, 3); ++n) {
         i = static_cast<std::uint8_t>(i + 1u);
         jj = static_cast<std::uint8_t>(jj + s[i]);
         std::swap(s[i], s[jj]);
         const auto k = s[static_cast<std::uint8_t>(s[i] + s[jj])];
         ctx.memory.write8(data + n, static_cast<std::uint8_t>(ctx.memory.read8(data + n) ^ k));
       }
       return true;
     }},
    // XeCryptBnQw_SwapDwQwLeBe(in, out, count): byte-swaps `count` 64-bit
    // words.
    {0x170u, "XeCryptBnQw_SwapDwQwLeBe",
     [](ExportCallContext& ctx) {
       const auto in = arg(ctx, 0);
       const auto out = arg(ctx, 1);
       const auto count = arg(ctx, 2);
       std::vector<std::uint64_t> words(count);
       for (std::uint32_t i = 0; i < count; ++i) words[i] = ctx.memory.read64_be(in + i * 8u);
       for (std::uint32_t i = 0; i < count; ++i) {
         auto w = words[i];
         std::uint64_t swapped = 0;
         for (int b = 0; b < 8; ++b) swapped = (swapped << 8u) | ((w >> (b * 8)) & 0xFFu);
         ctx.memory.write64_be(out + i * 8u, swapped);
       }
       return true;
     }},
    // RtlComputeCrc32(seed, buffer, length) -> CRC-32 (zlib, reflected
    // 0xEDB88320) continuing from `seed`.
    {0x27Bu, "RtlComputeCrc32",
     [](ExportCallContext& ctx) {
       std::uint32_t crc = ~arg(ctx, 0);
       const auto buffer = arg(ctx, 1);
       for (std::uint32_t n = 0; n < arg(ctx, 2); ++n) {
         crc ^= ctx.memory.read8(buffer + n);
         for (int b = 0; b < 8; ++b) crc = (crc >> 1u) ^ (0xEDB88320u & (0u - (crc & 1u)));
       }
       ctx.cpu.gpr[3] = ~crc;
       return true;
     }},
};

}  // namespace

bool register_xboxkrnl_crypt_exports(core::ExportRegistry& registry) {
  bool ok = true;
  for (const auto& entry : kExports) {
    core::ExportDescriptor desc{};
    desc.library = "xboxkrnl.exe";
    desc.name = entry.name;
    desc.ordinal = entry.ordinal;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = entry.handler;
    ok = registry.register_export(std::move(desc)) && ok;
  }
  return ok;
}

}  // namespace xenon::xbox

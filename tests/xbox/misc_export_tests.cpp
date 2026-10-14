// XeCryptSha/ExGetXConfigSetting/ExRegisterTitleTerminateNotification.

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <initializer_list>
#include <string>

#include "xenon/core/export_registry.hpp"
#include "xenon/memory/address_space.hpp"

namespace xenon::xbox {
bool register_xboxkrnl_misc_exports(core::ExportRegistry& registry);
bool register_xboxkrnl_crypt_exports(core::ExportRegistry& registry);
}  // namespace xenon::xbox

using namespace xenon;

namespace {

struct Fixture {
  memory::AddressSpace address_space{memory::GuestTranslationMode::Compact};
  core::ExportRegistry registry;

  Fixture() {
    assert(address_space.initialize());
    assert(xbox::register_xboxkrnl_misc_exports(registry));
  }

  core::ExportCallResult invoke(std::uint32_t ordinal, cpu::CpuState& cpu) {
    core::ExportCallContext ctx{cpu, address_space};
    return registry.invoke("xboxkrnl.exe", ordinal, ctx);
  }
};

std::array<std::uint8_t, 20> hash_single_buffer(Fixture& fx, const std::string& text) {
  memory::GuestAddress input{}, output{};
  assert(fx.address_space.allocate(std::max<std::size_t>(text.size(), 1u), 4u,
                                   memory::kReadWrite, false, input));
  assert(fx.address_space.allocate(20u, 4u, memory::kReadWrite, false, output));
  for (std::size_t i = 0; i < text.size(); ++i) {
    fx.address_space.write8(input + static_cast<memory::GuestAddress>(i),
                            static_cast<std::uint8_t>(text[i]));
  }

  cpu::CpuState cpu{};
  cpu.gpr[3] = input;
  cpu.gpr[4] = static_cast<std::uint32_t>(text.size());
  cpu.gpr[9] = output;
  cpu.gpr[10] = 20u;
  const auto result = fx.invoke(0x192u, cpu);
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == 0u);

  std::array<std::uint8_t, 20> digest{};
  for (std::size_t i = 0; i < digest.size(); ++i) {
    digest[i] = fx.address_space.read8(output + static_cast<memory::GuestAddress>(i));
  }
  return digest;
}

void test_xe_crypt_sha_deterministic_and_sensitive() {
  std::cout << "[TEST] XeCryptSha is deterministic and input-sensitive..." << std::endl;
  Fixture fx1, fx2, fx3;

  const auto digest_abc_1 = hash_single_buffer(fx1, "abc");
  const auto digest_abc_2 = hash_single_buffer(fx2, "abc");
  const auto digest_abd = hash_single_buffer(fx3, "abd");

  assert(digest_abc_1 == digest_abc_2);  // same input -> same digest
  assert(digest_abc_1 != digest_abd);    // different input -> different digest

  // A real SHA-1 digest is essentially never all-zero for a non-empty input.
  bool all_zero = true;
  for (auto b : digest_abc_1) {
    if (b != 0u) all_zero = false;
  }
  assert(!all_zero);

  std::cout << "  \xE2\x9C\x93 XeCryptSha is deterministic and sensitive to its input"
            << std::endl;
}

void test_xe_crypt_sha_multiple_inputs_concatenate() {
  std::cout << "[TEST] XeCryptSha(\"a\",\"b\",\"c\") == XeCryptSha(\"abc\")..." << std::endl;
  Fixture fx_split, fx_whole;

  const auto whole_digest = hash_single_buffer(fx_whole, "abc");

  memory::GuestAddress a{}, b{}, c{}, output{};
  assert(fx_split.address_space.allocate(1u, 4u, memory::kReadWrite, false, a));
  assert(fx_split.address_space.allocate(1u, 4u, memory::kReadWrite, false, b));
  assert(fx_split.address_space.allocate(1u, 4u, memory::kReadWrite, false, c));
  assert(fx_split.address_space.allocate(20u, 4u, memory::kReadWrite, false, output));
  fx_split.address_space.write8(a, static_cast<std::uint8_t>('a'));
  fx_split.address_space.write8(b, static_cast<std::uint8_t>('b'));
  fx_split.address_space.write8(c, static_cast<std::uint8_t>('c'));

  cpu::CpuState cpu{};
  cpu.gpr[3] = a;
  cpu.gpr[4] = 1u;
  cpu.gpr[5] = b;
  cpu.gpr[6] = 1u;
  cpu.gpr[7] = c;
  cpu.gpr[8] = 1u;
  cpu.gpr[9] = output;
  cpu.gpr[10] = 20u;
  const auto result = fx_split.invoke(0x192u, cpu);
  assert(result.handled && result.success);

  for (int i = 0; i < 20; ++i) {
    assert(fx_split.address_space.read8(output + static_cast<memory::GuestAddress>(i)) ==
           whole_digest[static_cast<std::size_t>(i)]);
  }

  std::cout << "  \xE2\x9C\x93 XeCryptSha(\"a\",\"b\",\"c\") matches XeCryptSha(\"abc\")"
            << std::endl;
}

// Only the first min(20, output_size) digest bytes are written.
void test_xe_crypt_sha_honours_the_output_size() {
  std::cout << "[TEST] XeCryptSha writes no more than output_size bytes..." << std::endl;
  Fixture fx;
  const auto full = hash_single_buffer(fx, "abc");
  memory::GuestAddress input{}, output{};
  assert(fx.address_space.allocate(4u, 4u, memory::kReadWrite, false, input));
  assert(fx.address_space.allocate(24u, 4u, memory::kReadWrite, false, output));
  for (std::uint32_t i = 0; i < 3u; ++i) fx.address_space.write8(input + i, "abc"[i]);
  for (std::uint32_t i = 0; i < 24u; ++i) fx.address_space.write8(output + i, 0xEEu);
  cpu::CpuState cpu{};
  cpu.gpr[3] = input;
  cpu.gpr[4] = 3u;
  cpu.gpr[9] = output;
  cpu.gpr[10] = 16u;
  assert(fx.invoke(0x192u, cpu).handled);
  for (std::uint32_t i = 0; i < 16u; ++i) assert(fx.address_space.read8(output + i) == full[i]);
  for (std::uint32_t i = 16u; i < 24u; ++i) {
    assert(fx.address_space.read8(output + i) == 0xEEu && "nothing past output_size");
  }
  std::cout << "  \xE2\x9C\x93 XeCryptSha truncates to output_size" << std::endl;
}

void test_ex_get_xconfig_setting_values_and_validation() {
  std::cout << "[TEST] ExGetXConfigSetting values and validation..." << std::endl;
  Fixture fx;

  memory::GuestAddress buffer{};
  memory::GuestAddress required_size_ptr{};
  assert(fx.address_space.allocate(4u, 4u, memory::kReadWrite, false, buffer));
  assert(fx.address_space.allocate(4u, 4u, memory::kReadWrite, false, required_size_ptr));

  // Query the English language default.
  cpu::CpuState cpu{};
  cpu.gpr[3] = 3u;
  cpu.gpr[4] = 9u;
  cpu.gpr[5] = buffer;
  cpu.gpr[6] = 4u;
  cpu.gpr[7] = required_size_ptr;
  assert(fx.invoke(0x10u, cpu).success);
  assert(cpu.gpr[3] == 0u);
  assert(fx.address_space.read16_be(required_size_ptr) == 4u);
  assert(fx.address_space.read32_be(buffer) == 1u);

  // Country is the one-byte setting.
  cpu = {};
  cpu.gpr[3] = 3u;
  cpu.gpr[4] = 0xEu;
  cpu.gpr[5] = buffer;
  cpu.gpr[6] = 1u;
  cpu.gpr[7] = required_size_ptr;
  assert(fx.invoke(0x10u, cpu).success && cpu.gpr[3] == 0u);
  assert(fx.address_space.read8(buffer) == 103u);
  assert(fx.address_space.read16_be(required_size_ptr) == 1u);

  // An undersized buffer reports BUFFER_TOO_SMALL. Real hardware does not
  // report the needed size on this path (only a call that would otherwise
  // succeed writes *required_size_ptr), so a stale sentinel must survive.
  fx.address_space.write16_be(required_size_ptr, 0xBEEFu);
  cpu = {};
  cpu.gpr[3] = 2u;
  cpu.gpr[4] = 2u;
  cpu.gpr[5] = buffer;
  cpu.gpr[6] = 3u;
  cpu.gpr[7] = required_size_ptr;
  assert(fx.invoke(0x10u, cpu).success);
  assert(cpu.gpr[3] == 0xC0000023u);
  assert(fx.address_space.read16_be(required_size_ptr) == 0xBEEFu);

  // A null buffer is valid only for a zero-sized size query; a nonzero size
  // with a null buffer is STATUS_INVALID_PARAMETER_3 and also leaves
  // *required_size_ptr untouched.
  fx.address_space.write16_be(required_size_ptr, 0xBEEFu);
  cpu = {};
  cpu.gpr[3] = 3u;
  cpu.gpr[4] = 9u;
  cpu.gpr[6] = 1u;
  cpu.gpr[7] = required_size_ptr;
  assert(fx.invoke(0x10u, cpu).success);
  assert(cpu.gpr[3] == 0xC00000F1u);
  assert(fx.address_space.read16_be(required_size_ptr) == 0xBEEFu);

  cpu = {};
  cpu.gpr[3] = 3u;
  cpu.gpr[4] = 9u;
  cpu.gpr[7] = required_size_ptr;
  assert(fx.invoke(0x10u, cpu).success && cpu.gpr[3] == 0u);
  assert(fx.address_space.read16_be(required_size_ptr) == 4u);

  // An unrecognized category/setting also leaves *required_size_ptr
  // untouched, since real hardware never resolves a setting_size for it.
  fx.address_space.write16_be(required_size_ptr, 0xBEEFu);
  cpu = {};
  cpu.gpr[3] = 0xFFFFu;
  cpu.gpr[4] = 1u;
  cpu.gpr[7] = required_size_ptr;
  assert(fx.invoke(0x10u, cpu).success && cpu.gpr[3] == 0xC00000EFu);
  assert(fx.address_space.read16_be(required_size_ptr) == 0xBEEFu);
  cpu = {};
  cpu.gpr[3] = 3u;
  cpu.gpr[4] = 0xFFFFu;
  cpu.gpr[7] = required_size_ptr;
  assert(fx.invoke(0x10u, cpu).success && cpu.gpr[3] == 0xC00000F0u);
  assert(fx.address_space.read16_be(required_size_ptr) == 0xBEEFu);

  std::cout << "  \xE2\x9C\x93 ExGetXConfigSetting returns researched defaults and statuses"
            << std::endl;
}

void test_ex_register_title_terminate_notification_succeeds() {
  std::cout << "[TEST] ExRegisterTitleTerminateNotification registration accepted..."
            << std::endl;
  Fixture fx;

  cpu::CpuState register_cpu{};
  register_cpu.gpr[4] = 1u;  // create
  assert(fx.invoke(0x15u, register_cpu).success);
  assert(register_cpu.gpr[3] == 0u);

  cpu::CpuState unregister_cpu{};
  unregister_cpu.gpr[4] = 0u;  // remove
  assert(fx.invoke(0x15u, unregister_cpu).success);
  assert(unregister_cpu.gpr[3] == 0u);

  std::cout << "  \xE2\x9C\x93 ExRegisterTitleTerminateNotification register/unregister succeed"
            << std::endl;
}

// XeCrypt hashes, HMAC, RC4 and RtlComputeCrc32 against published test
// vectors (FIPS 180-2 examples, RFC 2202, the common RC4 vectors, CRC-32
// check value), driven through the registry.
struct CryptFixture {
  memory::AddressSpace address_space{memory::GuestTranslationMode::Compact};
  core::ExportRegistry registry;
  memory::GuestAddress stack_top{};

  CryptFixture() {
    assert(address_space.initialize());
    assert(xbox::register_xboxkrnl_misc_exports(registry));
    assert(xbox::register_xboxkrnl_crypt_exports(registry));
    memory::GuestAddress stack{};
    assert(address_space.allocate(0x4000, 0x1000, memory::kReadWrite, false, stack));
    stack_top = stack + 0x4000u - 0x200u;
  }

  memory::GuestAddress bytes(const std::string& data, std::uint32_t extra = 0u) {
    memory::GuestAddress address{};
    assert(address_space.allocate(static_cast<std::uint32_t>(data.size()) + extra + 8u, 8u,
                                  memory::kReadWrite, false, address));
    for (std::size_t i = 0; i < data.size(); ++i) {
      address_space.write8(address + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(data[i]));
    }
    return address;
  }

  std::uint32_t call(std::uint32_t ordinal, std::initializer_list<std::uint32_t> args) {
    cpu::CpuState cpu{};
    cpu.gpr[1] = stack_top;
    int i = 0;
    for (const auto a : args) {
      if (i < 8) cpu.gpr[3 + i] = a;
      else address_space.write32_be(stack_top + 0x54u + static_cast<std::uint32_t>((i - 8) * 8), a);
      ++i;
    }
    core::ExportCallContext ctx{cpu, address_space};
    assert(registry.invoke("xboxkrnl.exe", ordinal, ctx).handled);
    return static_cast<std::uint32_t>(cpu.gpr[3]);
  }

  std::string hex(memory::GuestAddress address, std::uint32_t length) {
    static const char* digits = "0123456789abcdef";
    std::string text;
    for (std::uint32_t i = 0; i < length; ++i) {
      const auto b = address_space.read8(address + i);
      text.push_back(digits[b >> 4]);
      text.push_back(digits[b & 0xF]);
    }
    return text;
  }
};

void test_incremental_sha1_and_sha256() {
  std::cout << "[TEST] XeCryptSha{Init,Update,Final} and XeCryptSha256*..." << std::endl;
  CryptFixture f;
  memory::GuestAddress state{}, out{};
  assert(f.address_space.allocate(0x80u, 8u, memory::kReadWrite, false, state));
  assert(f.address_space.allocate(0x20u, 8u, memory::kReadWrite, false, out));

  // SHA-1("abc") from one update, and the two-block FIPS message split
  // across updates that straddle the block boundary.
  f.call(0x18Fu, {state});
  assert(f.address_space.read32_be(state + 4u) == 0x67452301u && "state[0] at +4");
  f.call(0x190u, {state, f.bytes("abc"), 3u});
  assert(f.address_space.read32_be(state) == 3u && "count of bytes at +0");
  f.call(0x191u, {state, out, 20u});
  assert(f.hex(out, 20u) == "a9993e364706816aba3e25717850c26c9cd0d89d");

  const std::string two_block = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  f.call(0x18Fu, {state});
  f.call(0x190u, {state, f.bytes(two_block.substr(0, 50)), 50u});
  f.call(0x190u, {state, f.bytes(two_block.substr(50)), static_cast<std::uint32_t>(two_block.size() - 50u)});
  f.call(0x191u, {state, out, 20u});
  assert(f.hex(out, 20u) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");

  // Final writes at most out_size bytes.
  for (std::uint32_t i = 0; i < 20u; ++i) f.address_space.write8(out + i, 0xEEu);
  f.call(0x191u, {state, out, 4u});
  assert(f.hex(out, 6u) == "84983e44eeee");

  f.call(0x310u, {state});
  f.call(0x311u, {state, f.bytes("ab"), 2u});
  f.call(0x311u, {state, f.bytes("c"), 1u});
  f.call(0x312u, {state, out, 32u});
  assert(f.hex(out, 32u) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  f.call(0x310u, {state});
  f.call(0x311u, {state, f.bytes(two_block), static_cast<std::uint32_t>(two_block.size())});
  f.call(0x312u, {state, out, 32u});
  assert(f.hex(out, 32u) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  std::cout << "  \xE2\x9C\x93 SHA-1 and SHA-256 vectors match" << std::endl;
}

void test_hmac_sha1_rfc2202() {
  std::cout << "[TEST] XeCryptHmacSha against RFC 2202..." << std::endl;
  CryptFixture f;
  memory::GuestAddress out{};
  assert(f.address_space.allocate(0x20u, 8u, memory::kReadWrite, false, out));
  f.call(0x182u, {f.bytes(std::string(20, '\x0b')), 20u, f.bytes("Hi There"), 8u, 0u, 0u, 0u, 0u, out, 20u});
  assert(f.hex(out, 20u) == "b617318655057264e28bc0b6fb378c8ef146be00");
  // A key longer than the block is hashed first; the data split across the
  // three inputs.
  f.call(0x182u, {f.bytes(std::string(80, '\xaa')), 80u, f.bytes("Test Using Larger"), 17u,
                  f.bytes(" Than Block-Size Key"), 20u, f.bytes(" - Hash Key First"), 17u, out, 20u});
  assert(f.hex(out, 20u) == "aa4ae5e15272d00e95705637ce8a3b55ed402112");
  std::cout << "  \xE2\x9C\x93 HMAC-SHA1 vectors match" << std::endl;
}

void test_rc4_crc32_and_qword_swap() {
  std::cout << "[TEST] XeCryptRc4*, RtlComputeCrc32, XeCryptBnQw_SwapDwQwLeBe..." << std::endl;
  CryptFixture f;
  auto data = f.bytes("Plaintext");
  f.call(0x18Du, {f.bytes("Key"), 3u, data, 9u});
  assert(f.hex(data, 9u) == "bbf316e8d940af0ad3");

  // Keyed state, then two Ecb calls continue the same key stream.
  memory::GuestAddress state{};
  assert(f.address_space.allocate(0x110u, 8u, memory::kReadWrite, false, state));
  f.call(0x18Bu, {state, f.bytes("Secret"), 6u});
  data = f.bytes("Attack at dawn");
  f.call(0x18Cu, {state, data, 6u});
  f.call(0x18Cu, {state, data + 6u, 8u});
  assert(f.hex(data, 14u) == "45a01f645fc35b383552544b9bf5");
  f.call(0x18Bu, {state, f.bytes("Secret"), 6u});
  f.call(0x18Cu, {state, data, 14u});
  assert(f.hex(data, 14u) == f.hex(f.bytes("Attack at dawn"), 14u) && "RC4 is its own inverse");

  assert(f.call(0x27Bu, {0u, f.bytes("123456789"), 9u}) == 0xCBF43926u);
  const auto partial = f.call(0x27Bu, {0u, f.bytes("1234"), 4u});
  assert(f.call(0x27Bu, {partial, f.bytes("56789"), 5u}) == 0xCBF43926u && "the seed continues it");
  assert(f.call(0x27Bu, {0x1234u, 0u, 0u}) == 0x1234u);

  const auto in = f.bytes("ABCDEFGHabcdefgh");
  const auto out = f.bytes(std::string(16, '\0'));
  f.call(0x170u, {in, out, 2u});
  assert(f.hex(out, 16u) == f.hex(f.bytes("HGFEDCBAhgfedcba"), 16u));
  std::cout << "  \xE2\x9C\x93 RC4, CRC-32 and qword swap match" << std::endl;
}

// XeCryptAesKey/Ecb/Cbc against FIPS-197 (C.1) and NIST SP 800-38A (F.2.1).
void test_aes_ecb_and_cbc() {
  std::cout << "[TEST] XeCryptAesKey/Ecb/Cbc against FIPS-197 and SP 800-38A..." << std::endl;
  CryptFixture f;
  const auto from_hex = [](const std::string& hex) {
    std::string bytes;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
      bytes.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return bytes;
  };
  memory::GuestAddress state{};
  assert(f.address_space.allocate(0x160u, 8u, memory::kReadWrite, false, state));

  f.call(0x159u, {state, f.bytes(from_hex("000102030405060708090a0b0c0d0e0f"))});
  assert(f.hex(state + 160u, 16u) == "13111d7fe3944a17f307a78b4d2b30c5" && "round key 10");
  assert(f.hex(state + 0xB0u, 16u) == "13111d7fe3944a17f307a78b4d2b30c5" &&
         "the inverse schedule starts with round key 10");
  assert(f.hex(state + 0x150u, 16u) == "000102030405060708090a0b0c0d0e0f");
  const auto block = f.bytes(from_hex("00112233445566778899aabbccddeeff"));
  const auto out = f.bytes(std::string(16, '\0'));
  f.call(0x15Au, {state, block, out, 1u});
  assert(f.hex(out, 16u) == "69c4e0d86a7b0430d8cdb78070b4c55a");
  f.call(0x15Au, {state, out, out, 0u});
  assert(f.hex(out, 16u) == "00112233445566778899aabbccddeeff" && "decrypt in place");

  f.call(0x159u, {state, f.bytes(from_hex("2b7e151628aed2a6abf7158809cf4f3c"))});
  const auto plain = f.bytes(from_hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"));
  const auto cipher = f.bytes(std::string(32, '\0'));
  const auto iv = f.bytes(from_hex("000102030405060708090a0b0c0d0e0f"));
  f.call(0x15Bu, {state, plain, 16u, cipher, iv, 1u});
  f.call(0x15Bu, {state, plain + 16u, 16u, cipher + 16u, iv, 1u});
  assert(f.hex(cipher, 32u) ==
         "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2" &&
         "the feed carries the chain across calls");
  assert(f.hex(iv, 16u) == "5086cb9b507219ee95db113a917678b2");
  const auto iv2 = f.bytes(from_hex("000102030405060708090a0b0c0d0e0f"));
  f.call(0x15Bu, {state, cipher, 32u, cipher, iv2, 0u});
  assert(f.hex(cipher, 32u) == f.hex(plain, 32u) && "decrypt in place");
  std::cout << "  \xE2\x9C\x93 AES vectors match" << std::endl;
}

}  // namespace

int main() {
  std::cout << "\n=== xboxkrnl Misc Export Correctness Tests ===" << std::endl;

  test_xe_crypt_sha_deterministic_and_sensitive();
  test_xe_crypt_sha_multiple_inputs_concatenate();
  test_xe_crypt_sha_honours_the_output_size();
  test_incremental_sha1_and_sha256();
  test_hmac_sha1_rfc2202();
  test_rc4_crc32_and_qword_swap();
  test_aes_ecb_and_cbc();
  test_ex_get_xconfig_setting_values_and_validation();
  test_ex_register_title_terminate_notification_succeeds();

  std::cout << "\n\xE2\x9C\x85 All tests passed!" << std::endl;
  return 0;
}

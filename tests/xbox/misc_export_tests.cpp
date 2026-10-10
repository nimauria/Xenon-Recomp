// XeCryptSha/ExGetXConfigSetting/ExRegisterTitleTerminateNotification.

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

#include "xenon/core/export_registry.hpp"
#include "xenon/memory/address_space.hpp"

namespace xenon::xbox {
bool register_xboxkrnl_misc_exports(core::ExportRegistry& registry);
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

}  // namespace

int main() {
  std::cout << "\n=== xboxkrnl Misc Export Correctness Tests ===" << std::endl;

  test_xe_crypt_sha_deterministic_and_sensitive();
  test_xe_crypt_sha_multiple_inputs_concatenate();
  test_ex_get_xconfig_setting_values_and_validation();
  test_ex_register_title_terminate_notification_succeeds();

  std::cout << "\n\xE2\x9C\x85 All tests passed!" << std::endl;
  return 0;
}

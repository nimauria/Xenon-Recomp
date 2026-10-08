// xboxkrnl XexCheckExecutablePrivilege (ordinal 0x194 / 404). Drives it
// through core::ExportRegistry::invoke() exactly as a guest thunk would.
//
// Real-world context: this is the next real export AC6's boot path calls
// once the gpr[12]-vs-gpr[0] save/restore-helper LR-source bug was fixed
// (real guest address 0x823d02cc) - a correctly-recognized-but-previously-
// unimplemented import, per src/core/session/execution/runtime_services.cpp's Trap path
// (STATUS_PROCEDURE_NOT_FOUND / 0xC0000005-style unresolved-import trap).
//
// Verified semantics (xenia-project/xenia's XexCheckExecutablePrivilege_entry
// / UserModule::GetOptHeader): `privilege` is a BIT POSITION into the XEX's
// XEX_HEADER_SYSTEM_FLAGS optional header (key 0x00030000, an inline 32-bit
// value - size_class = key & 0xFF = 0x00), not a mask; returns 1 iff that bit
// is set, 0 if clear or the header is altogether absent.

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <vector>

#include "xenon/core/export_registry.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xbox/xboxkrnl_xex_module_exports.hpp"
#include "xenon/xbox/xex_loader.hpp"

using namespace xenon;

namespace {

constexpr std::uint32_t kOrdinal = 0x194u;
constexpr std::uint32_t kSystemFlagsKey = 0x00030000u;

void append_be32(std::vector<std::byte>& bytes, std::uint32_t value) {
  const std::uint32_t be = ((value & 0xFFu) << 24) | ((value & 0xFF00u) << 8) |
                           ((value & 0xFF0000u) >> 8) | ((value & 0xFF000000u) >> 24);
  std::byte raw[4];
  std::memcpy(raw, &be, sizeof(raw));
  bytes.insert(bytes.end(), std::begin(raw), std::end(raw));
}

// Builds a minimal, real-layout XEX2 root header (magic/module_flags/
// header_size/image_size/security_info_offset/optional_header_count,
// followed by the optional header table) with zero or one optional header
// entries, matching xex_loader.hpp's documented layout exactly.
std::vector<std::byte> build_header(std::optional<std::uint32_t> system_flags_value) {
  std::vector<std::byte> bytes;
  const std::uint32_t count = system_flags_value.has_value() ? 1u : 0u;
  const std::uint32_t header_size = 0x18u + count * 8u;
  append_be32(bytes, 0x58455832u);  // magic "XEX2"
  append_be32(bytes, 0u);           // module_flags
  append_be32(bytes, header_size);  // header_size
  append_be32(bytes, 0x1000u);      // image_size
  append_be32(bytes, 0u);           // security_info_offset
  append_be32(bytes, count);        // optional_header_count
  if (system_flags_value) {
    append_be32(bytes, kSystemFlagsKey);
    append_be32(bytes, *system_flags_value);
  }
  return bytes;
}

struct Fixture {
  xbox::XexImage image{};
  core::ExportRegistry registry;

  explicit Fixture(std::optional<std::uint32_t> system_flags_value) {
    image.header_bytes = build_header(system_flags_value);
    assert(xbox::register_xboxkrnl_xex_module_exports(registry, image));
  }

  [[nodiscard]] core::ExportCallResult invoke(std::uint32_t privilege) {
    cpu::CpuState cpu{};
    cpu.gpr[3] = privilege;
    memory::AddressSpace address_space(memory::GuestTranslationMode::Compact);
    assert(address_space.initialize());
    core::ExportCallContext call{cpu, address_space, 0, 0};
    const auto result = registry.invoke("xboxkrnl", kOrdinal, call);
    last_r3 = static_cast<std::uint32_t>(cpu.gpr[3]);
    return result;
  }

  std::uint32_t last_r3{};
};

void test_ordinal_is_registered() {
  Fixture fixture(0u);
  assert(fixture.registry.contains("xboxkrnl.exe", kOrdinal));
  assert(fixture.registry.contains("xboxkrnl", "XexCheckExecutablePrivilege"));
}

void test_bit_set_returns_true() {
  // Privilege=6 -> mask 0x00000040, matching the real documented
  // XEX_SYSTEM_INSECURE_SOCKETS example from xenia's own implementation.
  Fixture fixture(0x00000040u);
  const auto result = fixture.invoke(6u);
  assert(result.handled && result.success);
  assert(fixture.last_r3 == 1u);
}

void test_bit_clear_returns_false() {
  Fixture fixture(0x00000040u);
  const auto result = fixture.invoke(5u);
  assert(result.handled && result.success);
  assert(fixture.last_r3 == 0u);
}

void test_missing_system_flags_header_returns_false() {
  Fixture fixture(std::nullopt);
  const auto result = fixture.invoke(6u);
  assert(result.handled && result.success);
  assert(fixture.last_r3 == 0u);
}

void test_high_bit_positions_do_not_overflow_the_shift() {
  // privilege values are masked to [0,31] before shifting - must not be UB.
  Fixture fixture(0x80000000u);
  const auto result = fixture.invoke(31u);
  assert(result.handled && result.success);
  assert(fixture.last_r3 == 1u);
}

}  // namespace

int main() {
  std::cout << "Testing xboxkrnl XexCheckExecutablePrivilege export...\n";

  test_ordinal_is_registered();
  test_bit_set_returns_true();
  test_bit_clear_returns_false();
  test_missing_system_flags_header_returns_false();
  test_high_bit_positions_do_not_overflow_the_shift();

  std::cout << "All XexCheckExecutablePrivilege export tests passed!\n";
  return 0;
}

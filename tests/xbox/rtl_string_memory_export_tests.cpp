// RtlCompareMemoryUlong/RtlFillMemoryUlong/RtlInitAnsiString/
// RtlFreeAnsiString/RtlInitUnicodeString. Drives each export through the
// real core::ExportRegistry exactly as a guest thunk would - AC6's boot
// path calls these directly (real guest addresses under 0x821Fxxxx),
// previously trapping with STATUS_PROCEDURE_NOT_FOUND (xam.xex ordinal
// pattern mirrored here for xboxkrnl.exe; see src/core/session/execution/runtime_services.cpp's Trap
// path).

#include <cassert>
#include <array>
#include <iostream>
#include <string_view>
#include <utility>

#include "xenon/core/export_registry.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xbox/export_metadata.hpp"

namespace xenon::xbox {
bool register_xboxkrnl_rtl_exports(core::ExportRegistry& registry);
}  // namespace xenon::xbox
#include "xenon/xbox/xboxkrnl_rtl_string_exports.hpp"

namespace {
constexpr std::int64_t kFiletimeUnixEpoch100ns = 116444736000000000LL;
}  // namespace

using namespace xenon;

namespace {

struct Fixture {
  memory::AddressSpace address_space{memory::GuestTranslationMode::Compact};
  core::ExportRegistry registry;

  Fixture() {
    assert(address_space.initialize());
    assert(xbox::register_xboxkrnl_rtl_exports(registry));
    // RtlFreeAnsiString lives with the rest of the string family.
    assert(xbox::register_xboxkrnl_rtl_string_exports(registry));
  }

  core::ExportCallResult invoke(std::uint32_t ordinal, cpu::CpuState& cpu) {
    core::ExportCallContext ctx{cpu, address_space};
    return registry.invoke("xboxkrnl.exe", ordinal, ctx);
  }
};

void test_export_metadata_matches_registered_rtl_subset() {
  constexpr std::array<std::pair<std::uint16_t, std::string_view>, 10> expected{{
      {0x12Cu, "RtlInitAnsiString"}, {0x12Du, "RtlInitUnicodeString"},
      {0x12Bu, "RtlImageXexHeaderField"}, {0x12Eu, "RtlInitializeCriticalSection"},
      {0x130u, "RtlLeaveCriticalSection"}, {0x125u, "RtlEnterCriticalSection"},
      {0x114u, "RtlAnsiStringToUnicodeString"}, {0x11Au, "RtlCompareMemory"},
      {0x11Bu, "RtlCompareMemoryUlong"}, {0x126u, "RtlFillMemoryUlong"},
  }};
  Fixture fx;
  for (const auto& [ordinal, name] : expected) {
    const auto* by_ordinal = xbox::lookup_export_metadata("XBOXKRNL.EXE", ordinal);
    const auto* by_name = xbox::lookup_export_metadata_by_name("xboxkrnl", name);
    assert(by_ordinal && by_name && by_ordinal == by_name);
    assert(by_ordinal->canonical_name == name);
    assert(by_ordinal->kind == xbox::ExportKind::Function);
  }
  for (const auto& [ordinal, name] : expected) {
    // Critical-section exports need a KernelProcess and are registered by the
    // session; the other sample entries are available in this fixture.
    if (name == "RtlInitializeCriticalSection" || name == "RtlLeaveCriticalSection" ||
        name == "RtlEnterCriticalSection") {
      continue;
    }
    const auto* descriptor = fx.registry.resolve("xboxkrnl.exe", ordinal);
    assert(descriptor && descriptor->name == name);
  }
  assert(xbox::lookup_export_metadata("xboxkrnl", 0x129u) == nullptr);
  assert(xbox::lookup_export_metadata_by_name("xboxkrnl", "RtlFillMemory") == nullptr);
}

void test_rtl_compare_memory_ulong() {
  std::cout << "[TEST] RtlCompareMemoryUlong..." << std::endl;
  Fixture fx;

  memory::GuestAddress a{}, b{};
  assert(fx.address_space.allocate(16u, 4u, memory::kReadWrite, false, a));
  assert(fx.address_space.allocate(16u, 4u, memory::kReadWrite, false, b));
  for (std::uint32_t i = 0; i < 16u; ++i) {
    fx.address_space.write8(a + i, static_cast<std::uint8_t>(i));
    fx.address_space.write8(b + i, static_cast<std::uint8_t>(i));
  }
  // Diverge at byte 9 (still inside the third ULONG, offset 8-11).
  fx.address_space.write8(b + 9u, 0xFFu);

  cpu::CpuState cpu{};
  cpu.gpr[3] = a;
  cpu.gpr[4] = b;
  cpu.gpr[5] = 16u;
  const auto result = fx.invoke(0x011Bu, cpu);
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == 8u);  // matched exactly the first two whole ULONGs

  std::cout << "  \xE2\x9C\x93 RtlCompareMemoryUlong matches real semantics" << std::endl;
}

void test_rtl_fill_memory_ulong() {
  std::cout << "[TEST] RtlFillMemoryUlong..." << std::endl;
  Fixture fx;

  memory::GuestAddress dest{};
  assert(fx.address_space.allocate(12u, 4u, memory::kReadWrite, false, dest));
  fx.address_space.write8(dest + 11u, 0xAAu);  // trailing byte past whole ULONGs

  cpu::CpuState cpu{};
  cpu.gpr[3] = dest;
  cpu.gpr[4] = 11u;  // only 2 whole ULONGs (8 bytes); byte 8-10 left untouched
  cpu.gpr[5] = 0xDEADBEEFu;
  const auto result = fx.invoke(0x0126u, cpu);
  assert(result.handled && result.success);
  assert(fx.address_space.read32_be(dest + 0u) == 0xDEADBEEFu);
  assert(fx.address_space.read32_be(dest + 4u) == 0xDEADBEEFu);
  assert(fx.address_space.read8(dest + 11u) == 0xAAu);  // untouched trailing byte

  std::cout << "  \xE2\x9C\x93 RtlFillMemoryUlong fills whole ULONGs only, matching real semantics"
            << std::endl;
}

void test_rtl_init_ansi_string() {
  std::cout << "[TEST] RtlInitAnsiString..." << std::endl;
  Fixture fx;

  memory::GuestAddress dest{}, source{};
  assert(fx.address_space.allocate(8u, 4u, memory::kReadWrite, false, dest));
  assert(fx.address_space.allocate(8u, 4u, memory::kReadWrite, false, source));
  const char* text = "AC6";
  for (std::size_t i = 0; text[i] != '\0'; ++i) {
    fx.address_space.write8(source + static_cast<memory::GuestAddress>(i),
                            static_cast<std::uint8_t>(text[i]));
  }
  fx.address_space.write8(source + 3u, 0u);

  cpu::CpuState cpu{};
  cpu.gpr[3] = dest;
  cpu.gpr[4] = source;
  const auto result = fx.invoke(0x012Cu, cpu);
  assert(result.handled && result.success);
  assert(fx.address_space.read16_be(dest + 0u) == 3u);      // Length
  assert(fx.address_space.read16_be(dest + 2u) == 4u);      // MaximumLength
  assert(fx.address_space.read32_be(dest + 4u) == source);  // Buffer aliases source

  std::cout << "  \xE2\x9C\x93 RtlInitAnsiString aliases the source buffer, matching real semantics"
            << std::endl;
}

void test_rtl_free_ansi_string() {
  std::cout << "[TEST] RtlFreeAnsiString..." << std::endl;
  Fixture fx;

  memory::GuestAddress dest{};
  assert(fx.address_space.allocate(8u, 4u, memory::kReadWrite, false, dest));
  fx.address_space.write16_be(dest + 0u, 3u);
  fx.address_space.write16_be(dest + 2u, 4u);
  fx.address_space.write32_be(dest + 4u, 0x12340000u);

  cpu::CpuState cpu{};
  cpu.gpr[3] = dest;
  const auto result = fx.invoke(0x0127u, cpu);
  assert(result.handled && result.success);
  assert(fx.address_space.read16_be(dest + 0u) == 0u);
  assert(fx.address_space.read16_be(dest + 2u) == 0u);
  assert(fx.address_space.read32_be(dest + 4u) == 0u);

  std::cout << "  \xE2\x9C\x93 RtlFreeAnsiString zeroes the STRING struct" << std::endl;
}

void test_rtl_init_unicode_string() {
  std::cout << "[TEST] RtlInitUnicodeString..." << std::endl;
  Fixture fx;

  memory::GuestAddress dest{}, source{};
  assert(fx.address_space.allocate(8u, 4u, memory::kReadWrite, false, dest));
  assert(fx.address_space.allocate(16u, 4u, memory::kReadWrite, false, source));
  const char16_t text[] = u"AC6";
  for (std::size_t i = 0; text[i] != 0; ++i) {
    fx.address_space.write16_be(source + static_cast<memory::GuestAddress>(i * 2u),
                                static_cast<std::uint16_t>(text[i]));
  }
  fx.address_space.write16_be(source + 6u, 0u);

  cpu::CpuState cpu{};
  cpu.gpr[3] = dest;
  cpu.gpr[4] = source;
  const auto result = fx.invoke(0x012Du, cpu);
  assert(result.handled && result.success);
  assert(fx.address_space.read16_be(dest + 0u) == 6u);  // Length (bytes)
  assert(fx.address_space.read16_be(dest + 2u) == 8u);  // MaximumLength
  assert(fx.address_space.read32_be(dest + 4u) == source);

  std::cout << "  \xE2\x9C\x93 RtlInitUnicodeString aliases the source buffer, matching real "
               "semantics"
            << std::endl;
}

void test_rtl_time_to_time_fields_epoch() {
  std::cout << "[TEST] RtlTimeToTimeFields at the Unix epoch..." << std::endl;
  Fixture fx;

  memory::GuestAddress time_ptr{}, fields_ptr{};
  assert(fx.address_space.allocate(8u, 8u, memory::kReadWrite, false, time_ptr));
  assert(fx.address_space.allocate(16u, 4u, memory::kReadWrite, false, fields_ptr));
  // 1970-01-01 00:00:00.000 UTC, a Thursday - the standard FILETIME/Unix
  // epoch cross-check.
  fx.address_space.write64_be(time_ptr, static_cast<std::uint64_t>(kFiletimeUnixEpoch100ns));

  cpu::CpuState cpu{};
  cpu.gpr[3] = time_ptr;
  cpu.gpr[4] = fields_ptr;
  const auto result = fx.invoke(0x0140u, cpu);
  assert(result.handled && result.success);
  assert(fx.address_space.read16_be(fields_ptr + 0u) == 1970u);   // Year
  assert(fx.address_space.read16_be(fields_ptr + 2u) == 1u);      // Month
  assert(fx.address_space.read16_be(fields_ptr + 4u) == 1u);      // Day
  assert(fx.address_space.read16_be(fields_ptr + 6u) == 0u);      // Hour
  assert(fx.address_space.read16_be(fields_ptr + 8u) == 0u);      // Minute
  assert(fx.address_space.read16_be(fields_ptr + 10u) == 0u);     // Second
  assert(fx.address_space.read16_be(fields_ptr + 14u) == 4u);     // Weekday: Thursday

  std::cout << "  \xE2\x9C\x93 RtlTimeToTimeFields matches the real Unix epoch cross-check"
            << std::endl;
}

void test_rtl_time_to_time_fields_known_date() {
  std::cout << "[TEST] RtlTimeToTimeFields at a known reference date..." << std::endl;
  Fixture fx;

  memory::GuestAddress time_ptr{}, fields_ptr{};
  assert(fx.address_space.allocate(8u, 8u, memory::kReadWrite, false, time_ptr));
  assert(fx.address_space.allocate(16u, 4u, memory::kReadWrite, false, fields_ptr));
  // 2000-01-01 00:00:00 UTC, a Saturday (well-known reference date) =
  // Unix timestamp 946684800.
  const std::int64_t raw = kFiletimeUnixEpoch100ns + 946684800LL * 10'000'000LL;
  fx.address_space.write64_be(time_ptr, static_cast<std::uint64_t>(raw));

  cpu::CpuState cpu{};
  cpu.gpr[3] = time_ptr;
  cpu.gpr[4] = fields_ptr;
  const auto result = fx.invoke(0x0140u, cpu);
  assert(result.handled && result.success);
  assert(fx.address_space.read16_be(fields_ptr + 0u) == 2000u);
  assert(fx.address_space.read16_be(fields_ptr + 2u) == 1u);
  assert(fx.address_space.read16_be(fields_ptr + 4u) == 1u);
  assert(fx.address_space.read16_be(fields_ptr + 14u) == 6u);  // Weekday: Saturday

  std::cout << "  \xE2\x9C\x93 RtlTimeToTimeFields matches the known 2000-01-01 reference date"
            << std::endl;
}

void test_rtl_time_fields_to_time_round_trip() {
  std::cout << "[TEST] RtlTimeFieldsToTime round-trips through RtlTimeToTimeFields..."
            << std::endl;
  Fixture fx;

  memory::GuestAddress fields_ptr{}, time_ptr{};
  assert(fx.address_space.allocate(16u, 4u, memory::kReadWrite, false, fields_ptr));
  assert(fx.address_space.allocate(8u, 8u, memory::kReadWrite, false, time_ptr));
  fx.address_space.write16_be(fields_ptr + 0u, 2026u);  // Year
  fx.address_space.write16_be(fields_ptr + 2u, 9u);     // Month
  fx.address_space.write16_be(fields_ptr + 4u, 27u);    // Day
  fx.address_space.write16_be(fields_ptr + 6u, 14u);    // Hour
  fx.address_space.write16_be(fields_ptr + 8u, 30u);    // Minute
  fx.address_space.write16_be(fields_ptr + 10u, 15u);   // Second
  fx.address_space.write16_be(fields_ptr + 12u, 500u);  // Milliseconds

  cpu::CpuState to_time_cpu{};
  to_time_cpu.gpr[3] = fields_ptr;
  to_time_cpu.gpr[4] = time_ptr;
  const auto to_time_result = fx.invoke(0x013Fu, to_time_cpu);
  assert(to_time_result.handled && to_time_result.success);
  assert(to_time_cpu.gpr[3] == 1u);  // valid date

  memory::GuestAddress round_trip_fields{};
  assert(fx.address_space.allocate(16u, 4u, memory::kReadWrite, false, round_trip_fields));
  cpu::CpuState to_fields_cpu{};
  to_fields_cpu.gpr[3] = time_ptr;
  to_fields_cpu.gpr[4] = round_trip_fields;
  const auto to_fields_result = fx.invoke(0x0140u, to_fields_cpu);
  assert(to_fields_result.handled && to_fields_result.success);

  assert(fx.address_space.read16_be(round_trip_fields + 0u) == 2026u);
  assert(fx.address_space.read16_be(round_trip_fields + 2u) == 9u);
  assert(fx.address_space.read16_be(round_trip_fields + 4u) == 27u);
  assert(fx.address_space.read16_be(round_trip_fields + 6u) == 14u);
  assert(fx.address_space.read16_be(round_trip_fields + 8u) == 30u);
  assert(fx.address_space.read16_be(round_trip_fields + 10u) == 15u);
  assert(fx.address_space.read16_be(round_trip_fields + 12u) == 500u);

  std::cout << "  \xE2\x9C\x93 RtlTimeFieldsToTime/RtlTimeToTimeFields round-trip exactly"
            << std::endl;
}

void test_rtl_time_fields_to_time_rejects_invalid() {
  std::cout << "[TEST] RtlTimeFieldsToTime rejects an invalid date..." << std::endl;
  Fixture fx;

  memory::GuestAddress fields_ptr{}, time_ptr{};
  assert(fx.address_space.allocate(16u, 4u, memory::kReadWrite, false, fields_ptr));
  assert(fx.address_space.allocate(8u, 8u, memory::kReadWrite, false, time_ptr));
  fx.address_space.write16_be(fields_ptr + 0u, 2026u);
  fx.address_space.write16_be(fields_ptr + 2u, 13u);  // invalid month
  fx.address_space.write16_be(fields_ptr + 4u, 1u);

  cpu::CpuState cpu{};
  cpu.gpr[3] = fields_ptr;
  cpu.gpr[4] = time_ptr;
  const auto result = fx.invoke(0x013Fu, cpu);
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == 0u);  // FALSE: invalid

  std::cout << "  \xE2\x9C\x93 RtlTimeFieldsToTime returns FALSE for an invalid date" << std::endl;
}

void test_rtl_capture_context_is_a_documented_no_op() {
  std::cout << "[TEST] RtlCaptureContext (documented no-op)..." << std::endl;
  Fixture fx;
  assert(fx.registry.contains("xboxkrnl.exe", 0x0119u));
  assert(fx.registry.contains("xboxkrnl.exe", "RtlCaptureContext"));

  // Fill the guest "CONTEXT" buffer with a sentinel first: the whole point
  // of not implementing this is that it must NOT touch guest memory (see
  // xboxkrnl_rtl_exports.cpp's comment) rather than writing a guessed-size/
  // guessed-layout buffer - the sentinel must survive the call untouched.
  memory::GuestAddress context_buffer{};
  assert(fx.address_space.allocate(0x200u, 16u, memory::kReadWrite, false, context_buffer));
  for (std::uint32_t i = 0; i < 0x200u; i += 4u) {
    fx.address_space.write32_be(context_buffer + i, 0xDEADBEEFu);
  }

  cpu::CpuState cpu{};
  cpu.gpr[3] = context_buffer;
  const auto result = fx.invoke(0x0119u, cpu);
  assert(result.handled && result.success);
  for (std::uint32_t i = 0; i < 0x200u; i += 4u) {
    assert(fx.address_space.read32_be(context_buffer + i) == 0xDEADBEEFu);
  }

  std::cout << "  \xE2\x9C\x93 RtlCaptureContext does not touch guest memory" << std::endl;
}

void test_c_specific_handler_reports_continue_search() {
  std::cout << "[TEST] __C_specific_handler (documented no-op)..." << std::endl;
  Fixture fx;
  assert(fx.registry.contains("xboxkrnl.exe", 0x01A5u));
  assert(fx.registry.contains("xboxkrnl.exe", "__C_specific_handler"));

  cpu::CpuState cpu{};
  const auto result = fx.invoke(0x01A5u, cpu);
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == 1u);  // ExceptionContinueSearch

  std::cout << "  \xE2\x9C\x93 __C_specific_handler reports ExceptionContinueSearch" << std::endl;
}

}  // namespace

int main() {
  test_export_metadata_matches_registered_rtl_subset();
  std::cout << "\n=== RTL String/Memory Export Correctness Tests ===" << std::endl;

  test_rtl_compare_memory_ulong();
  test_rtl_fill_memory_ulong();
  test_rtl_init_ansi_string();
  test_rtl_free_ansi_string();
  test_rtl_init_unicode_string();
  test_rtl_time_to_time_fields_epoch();
  test_rtl_time_to_time_fields_known_date();
  test_rtl_time_fields_to_time_round_trip();
  test_rtl_time_fields_to_time_rejects_invalid();
  test_rtl_capture_context_is_a_documented_no_op();
  test_c_specific_handler_reports_continue_search();

  std::cout << "\n\xE2\x9C\x85 All tests passed!" << std::endl;
  return 0;
}

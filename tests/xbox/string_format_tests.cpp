// xboxkrnl formatted-output family (sprintf/_snprintf/_scprintf/vsprintf/
// _vsnprintf/_vscprintf and wide variants) and DbgPrint, plus the shared
// printf formatter they run on.
//
// The formatter is checked directly (grammar, flags, sizes, argument
// sources), then each export is driven through core::ExportRegistry::invoke()
// with real guest memory, checking the guest-visible result: the destination
// buffer contents and the sign-extended int in r3. Ace Combat 6 imports
// sprintf and _vsnprintf.

#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "xenon/core/export_registry.hpp"
#include "xenon/logging/logger.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xbox/string_format.hpp"
#include "xenon/xbox/xboxkrnl_string_exports.hpp"

using namespace xenon;
namespace format = xenon::xbox::format;

namespace {

constexpr std::uint32_t kOrdDbgPrint = 0x03u;
constexpr std::uint32_t kOrdScprintf = 0x139u;
constexpr std::uint32_t kOrdSnprintf = 0x13Au;
constexpr std::uint32_t kOrdSprintf = 0x13Bu;
constexpr std::uint32_t kOrdScwprintf = 0x13Cu;
constexpr std::uint32_t kOrdSnwprintf = 0x13Du;
constexpr std::uint32_t kOrdSwprintf = 0x13Eu;
constexpr std::uint32_t kOrdVscprintf = 0x14Cu;
constexpr std::uint32_t kOrdVsnprintf = 0x14Du;
constexpr std::uint32_t kOrdVsprintf = 0x14Eu;
constexpr std::uint32_t kOrdVsnwprintf = 0x150u;
constexpr std::uint32_t kOrdVswprintf = 0x151u;

struct Fixture {
  std::shared_ptr<memory::AddressSpace> memory;
  core::ExportRegistry registry;
  memory::GuestAddress arena{};
  std::uint32_t used{};
  static constexpr std::uint32_t kArena = 0x40000u;

  Fixture() {
    memory = std::make_shared<memory::AddressSpace>(memory::GuestTranslationMode::Compact);
    const bool ok = memory->initialize();
    assert(ok);
    const bool arena_ok = memory->allocate(kArena, 0x1000u, memory::kReadWrite, true, arena);
    assert(arena_ok);
    const bool reg = xbox::register_xboxkrnl_string_exports(registry);
    assert(reg);
    static_cast<void>(ok);
    static_cast<void>(arena_ok);
    static_cast<void>(reg);
  }

  memory::GuestAddress alloc(std::uint32_t bytes) {
    const auto aligned = (bytes + 15u) & ~15u;
    assert(used + aligned <= kArena);
    const auto addr = arena + used;
    used += aligned;
    return addr;
  }
  memory::GuestAddress str(const std::string& text) {
    const auto addr = alloc(static_cast<std::uint32_t>(text.size()) + 1u);
    for (std::size_t i = 0; i < text.size(); ++i) {
      memory->write8(addr + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(text[i]));
    }
    memory->write8(addr + static_cast<std::uint32_t>(text.size()), 0u);
    return addr;
  }
  memory::GuestAddress wstr(const std::u16string& text) {
    const auto addr = alloc(static_cast<std::uint32_t>(text.size() + 1u) * 2u);
    for (std::size_t i = 0; i < text.size(); ++i) {
      memory->write16_be(addr + static_cast<std::uint32_t>(i) * 2u, text[i]);
    }
    memory->write16_be(addr + static_cast<std::uint32_t>(text.size()) * 2u, 0u);
    return addr;
  }
  std::string read_str(memory::GuestAddress addr) {
    std::string out;
    for (std::uint32_t i = 0;; ++i) {
      const auto c = memory->read8(addr + i);
      if (c == 0) break;
      out.push_back(static_cast<char>(c));
    }
    return out;
  }
  std::u16string read_wstr(memory::GuestAddress addr) {
    std::u16string out;
    for (std::uint32_t i = 0;; ++i) {
      const auto c = memory->read16_be(addr + i * 2u);
      if (c == 0) break;
      out.push_back(static_cast<char16_t>(c));
    }
    return out;
  }

  // Formats `fmt` with the given integer/pointer/double-bit slots as varargs
  // starting at argument index `first` (registers only).
  format::Result run(const std::string& fmt, std::vector<std::uint64_t> slots, bool wide = false,
                     std::uint32_t first = 1u) {
    cpu::CpuState cpu{};
    for (std::size_t i = 0; i < slots.size() && first + i < 8u; ++i) cpu.gpr[3u + first + i] = slots[i];
    format::RegisterArgumentSource args(cpu, *memory, first);
    return format::format(*memory, std::u16string(fmt.begin(), fmt.end()), args, wide);
  }
  std::string text(const format::Result& r) {
    std::string out;
    for (const char16_t c : r.text) out.push_back(static_cast<char>(c));
    return out;
  }
  std::string fmt(const std::string& f, std::vector<std::uint64_t> slots = {}) {
    const auto r = run(f, std::move(slots));
    assert(r.count >= 0);
    assert(static_cast<std::size_t>(r.count) == r.text.size());
    return text(r);
  }

  std::uint64_t call(std::uint32_t ordinal, std::vector<std::uint64_t> regs) {
    cpu::CpuState cpu{};
    for (std::size_t i = 0; i < regs.size(); ++i) cpu.gpr[3u + i] = regs[i];
    core::ExportCallContext ctx{cpu, *memory, 0, 0};
    const auto result = registry.invoke("xboxkrnl", ordinal, ctx);
    assert(result.handled && result.success);
    return cpu.gpr[3];
  }
};

std::uint64_t dbits(double v) { return std::bit_cast<std::uint64_t>(v); }
constexpr std::uint64_t kMinusOne = 0xFFFFFFFFFFFFFFFFull;

void test_integers() {
  Fixture f;
  assert(f.fmt("%d", {42}) == "42");
  assert(f.fmt("%d", {static_cast<std::uint64_t>(-42)}) == "-42");
  assert(f.fmt("%i", {7}) == "7");
  assert(f.fmt("%u", {0xFFFFFFFFull}) == "4294967295");
  assert(f.fmt("%u", {static_cast<std::uint64_t>(-1)}) == "4294967295" && "32-bit unsigned");
  assert(f.fmt("%x", {0xBEEF}) == "beef");
  assert(f.fmt("%X", {0xBEEF}) == "BEEF");
  assert(f.fmt("%#x", {0xBEEF}) == "0xbeef");
  assert(f.fmt("%#X", {0xBEEF}) == "0XBEEF");
  assert(f.fmt("%#x", {0}) == "0" && "no 0x prefix on zero");
  assert(f.fmt("%o", {8}) == "10");
  assert(f.fmt("%#o", {8}) == "010");
  assert(f.fmt("%5d|", {42}) == "   42|");
  assert(f.fmt("%-5d|", {42}) == "42   |");
  assert(f.fmt("%05d", {42}) == "00042");
  assert(f.fmt("%05d", {static_cast<std::uint64_t>(-42)}) == "-0042");
  assert(f.fmt("%+d", {5}) == "+5");
  assert(f.fmt("% d", {5}) == " 5");
  assert(f.fmt("%.3d", {5}) == "005");
  assert(f.fmt("%8.3d|", {5}) == "     005|");
  assert(f.fmt("%08.3d|", {5}) == "     005|" && "explicit precision disables the 0 flag");
  assert(f.fmt("%.0d", {0}) == "" && "zero with zero precision prints nothing");
  assert(f.fmt("%d", {0}) == "0");
  assert(f.fmt("%+u", {5}) == "5" && "the + flag does not apply to unsigned");
  assert(f.fmt("%-+6d|", {5}) == "+5    |");
}

void test_size_prefixes() {
  Fixture f;
  assert(f.fmt("%hd", {0x12348000ull}) == "-32768" && "h narrows to 16 bits (signed)");
  assert(f.fmt("%hu", {0xFFFFull}) == "65535" && "h narrows unsigned to 16 bits");
  assert(f.fmt("%hx", {0x12345ull}) == "2345");
  assert(f.fmt("%ld", {0xFFFFFFFFull}) == "-1" && "long is 32 bits on this platform");
  assert(f.fmt("%lld", {0x100000000ull}) == "4294967296");
  assert(f.fmt("%llu", {0xFFFFFFFFFFFFFFFFull}) == "18446744073709551615");
  assert(f.fmt("%I64d", {0x100000000ull}) == "4294967296");
  assert(f.fmt("%I64x", {0x1FFFFFFFFull}) == "1ffffffff");
  assert(f.fmt("%I32d", {0x100000005ull}) == "5" && "I32 is a plain 32-bit int");
  assert(f.fmt("%Ld", {9}) == "9" && "L is ignored");
  assert(f.fmt("%d", {0x1FFFFFFFFull}) == "-1" && "an int takes the low 32 bits");
}

void test_pointers_and_percent() {
  Fixture f;
  assert(f.fmt("%p", {0xABCDull}) == "0000ABCD" && "%p is eight uppercase hex digits");
  assert(f.fmt("%p", {0x82001000ull}) == "82001000");
  assert(f.fmt("100%%") == "100%");
  assert(f.fmt("%%%d%%", {5}) == "%5%");
  assert(f.fmt("no format") == "no format");
  assert(f.fmt("") == "");
}

void test_strings_and_chars() {
  Fixture f;
  const auto hello = f.str("hello");
  assert(f.fmt("%s", {hello}) == "hello");
  assert(f.fmt("[%10s]", {hello}) == "[     hello]");
  assert(f.fmt("[%-10s]", {hello}) == "[hello     ]");
  assert(f.fmt("[%.3s]", {hello}) == "[hel]");
  assert(f.fmt("[%8.3s]", {hello}) == "[     hel]");
  assert(f.fmt("[%010s]", {hello}) == "[00000hello]" && "the 0 flag pads strings, as the CRT does");
  assert(f.fmt("%s", {0}) == "(null)");
  assert(f.fmt("%.3s", {0}) == "(nu");
  assert(f.fmt("%s|%s", {hello, f.str("x")}) == "hello|x");
  assert(f.fmt("%c%c", {'o', 'k'}) == "ok");
  assert(f.fmt("[%3c]", {'x'}) == "[  x]");
  assert(f.fmt("[%-3c]", {'x'}) == "[x  ]");

  // Wide arguments to a narrow function: %ws / %S; and %hs in a wide function.
  const auto wide = f.wstr(u"wide");
  assert(f.fmt("%ws", {wide}) == "wide");
  assert(f.fmt("%S", {wide}) == "wide");
  assert(f.fmt("%ls|%lc", {wide, 0x41}) == "wide|A");
  assert(f.fmt("%C", {0x42}) == "B");

  // ANSI_STRING / UNICODE_STRING via %Z / %wZ.
  const auto ansi = f.alloc(8);
  f.memory->write16_be(ansi, 3u);
  f.memory->write16_be(ansi + 2u, 8u);
  f.memory->write32_be(ansi + 4u, f.str("abcdef"));
  assert(f.fmt("%Z", {ansi}) == "abc" && "Length bounds the ANSI_STRING");
  const auto unicode = f.alloc(8);
  f.memory->write16_be(unicode, 4u);  // bytes
  f.memory->write16_be(unicode + 2u, 8u);
  f.memory->write32_be(unicode + 4u, f.wstr(u"uv-extra"));
  assert(f.fmt("%wZ", {unicode}) == "uv");
  assert(f.fmt("%Z", {0}) == "(null)");
}

void test_wide_function_defaults() {
  Fixture f;
  const auto wide = f.wstr(u"wchar");
  const auto narrow = f.str("nchar");
  auto r = f.run("%s|%hs|%c|%hc", {wide, narrow, 0x263A, 'n'}, /*wide=*/true);
  assert(r.count == 15);
  assert(r.text == std::u16string(u"wchar|nchar|") + char16_t(0x263A) + u"|n");
  // In a wide function %S is the narrow one.
  r = f.run("%S", {narrow}, true);
  assert(r.text == u"nchar");
}

void test_floats() {
  Fixture f;
  assert(f.fmt("%f", {dbits(3.14159)}) == "3.141590");
  assert(f.fmt("%.2f", {dbits(3.14159)}) == "3.14");
  assert(f.fmt("%.0f", {dbits(2.5)}) == "2");
  assert(f.fmt("%8.3f|", {dbits(3.14159)}) == "   3.142|");
  assert(f.fmt("%-8.1f|", {dbits(3.14159)}) == "3.1     |");
  assert(f.fmt("%08.2f", {dbits(3.14159)}) == "00003.14");
  assert(f.fmt("%+.1f", {dbits(2.0)}) == "+2.0");
  assert(f.fmt("% .1f", {dbits(2.0)}) == " 2.0");
  assert(f.fmt("%f", {dbits(-1.5)}) == "-1.500000");
  assert(f.fmt("%08.2f", {dbits(-1.5)}) == "-0001.50");
  assert(f.fmt("%e", {dbits(12345.678)}) == "1.234568e+04");
  assert(f.fmt("%E", {dbits(0.00012)}) == "1.200000E-04");
  assert(f.fmt("%g", {dbits(100000.0)}) == "100000");
  assert(f.fmt("%g", {dbits(1000000.0)}) == "1e+06");
  assert(f.fmt("%.3g", {dbits(3.14159)}) == "3.14");
  assert(f.fmt("%#.0f", {dbits(3.0)}) == "3." && "# forces the decimal point");
  assert(f.fmt("%f", {dbits(0.0)}) == "0.000000");
  assert(f.fmt("%f", {dbits(-0.0)}) == "-0.000000");
  // Hex floats and non-finite values are formatted by Xenon, not the host CRT,
  // so they are identical on every host.
  assert(f.fmt("%a", {dbits(1.0)}) == "0x1p+0");
  assert(f.fmt("%a", {dbits(0.5)}) == "0x1p-1");
  assert(f.fmt("%a", {dbits(3.0)}) == "0x1.8p+1");
  assert(f.fmt("%a", {dbits(0.0)}) == "0x0p+0");
  assert(f.fmt("%a", {dbits(-2.0)}) == "-0x1p+1");
  assert(f.fmt("%A", {dbits(255.5)}) == "0X1.FFP+7");
  assert(f.fmt("%.2a", {dbits(1.0)}) == "0x1.00p+0");
  assert(f.fmt("%.1a", {dbits(1.5)}) == "0x1.8p+0");
  assert(f.fmt("%.1a", {dbits(std::bit_cast<double>(0x3FFFF00000000000ull))}) == "0x1.0p+1" &&
         "rounding carries into the exponent");
  assert(f.fmt("%#a", {dbits(1.0)}) == "0x1.p+0");
  assert(f.fmt("%a", {dbits(std::bit_cast<double>(1ull))}) == "0x0.0000000000001p-1022" &&
         "subnormals keep a zero lead digit");
  assert(f.fmt("%12a|", {dbits(1.0)}) == "      0x1p+0|");
  assert(f.fmt("%F", {dbits(1.0 / 0.0)}) == "INF");
  assert(f.fmt("%E", {dbits(-1.0 / 0.0)}) == "-INF");
  assert(f.fmt("%G", {dbits(std::bit_cast<double>(0x7FF8000000000000ull))}) == "NAN");
  assert(f.fmt("%+f", {dbits(1.0 / 0.0)}) == "+inf");
  assert(f.fmt("%10f|", {dbits(1.0 / 0.0)}) == "       inf|");
  assert(f.fmt("%010f|", {dbits(1.0 / 0.0)}) == "       inf|" && "non-finite pads with spaces");
  assert(f.fmt("%f", {dbits(-1.0 / 0.0)}) == "-inf");
  assert(f.fmt("%f", {dbits(std::bit_cast<double>(0x7FF8000000000000ull))}) == "nan");
}

void test_star_width_precision() {
  Fixture f;
  assert(f.fmt("[%*d]", {6, 42}) == "[    42]");
  assert(f.fmt("[%*d]", {static_cast<std::uint64_t>(-6), 42}) == "[42    ]" && "negative * width left-justifies");
  assert(f.fmt("[%.*d]", {4, 7}) == "[0007]");
  assert(f.fmt("[%.*d]", {static_cast<std::uint64_t>(-4), 7}) == "[7]" && "negative * precision is unspecified");
  assert(f.fmt("[%*.*f]", {8, 2, dbits(3.14159)}) == "[    3.14]");
  assert(f.fmt("[%.*s]", {2, f.str("abcdef")}) == "[ab]");
}

void test_percent_n() {
  Fixture f;
  const auto slot = f.alloc(8);
  f.memory->write32_be(slot, 0xFFFFFFFFu);
  assert(f.fmt("abc%n-", {slot}) == "abc-");
  assert(f.memory->read32_be(slot) == 3u && "%n stores the count so far");
  f.memory->write32_be(slot, 0xFFFFFFFFu);
  assert(f.fmt("ab%hn", {slot}) == "ab");
  assert(f.memory->read16_be(slot) == 2u && "%hn stores 16 bits");
  assert(f.memory->read16_be(slot + 2u) == 0xFFFFu);
}

void test_errors() {
  Fixture f;
  assert(f.run("%", {}).count == -1 && "a dangling %");
  assert(f.run("abc%", {}).count == -1);
  assert(f.run("%y", {1}).count == -1 && "an unknown conversion");
  assert(f.run("%5", {1}).count == -1 && "a format cut off inside a specifier");
  // A wide character above 0xFF cannot be produced by a narrow function.
  const auto wide = f.wstr(std::u16string(1, char16_t(0x4E2D)));
  assert(f.run("%ws", {wide}).count == -1);
  assert(f.run("%ws", {wide}, /*wide=*/true).count == 1 && "but a wide function can");
  // An absurd guest-supplied width is a failed format, not an allocation.
  assert(f.run("%*d", {0x7FFFFFFFull, 1}).count == -1);
}

// Arguments past the eighth come from 8-byte stack slots at r1 + 0x54 + 8k,
// and a va_list is an array of consecutive 8-byte slots.
void test_stack_and_array_arguments() {
  Fixture f;
  // 12 integer arguments (indexes 1..12): r4..r10 hold the first 7, then the
  // stack slots (index 8 upward) hold the rest.
  cpu::CpuState cpu{};
  const auto stack = f.alloc(0x100);
  cpu.gpr[1] = stack;
  for (std::uint32_t i = 1; i <= 7; ++i) cpu.gpr[3u + i] = i;  // indexes 1..7
  for (std::uint32_t i = 8; i <= 12; ++i) {
    f.memory->write64_be(stack + 0x54u + 8u * (i - 8u), i);
  }
  format::RegisterArgumentSource reg_args(cpu, *f.memory, 1u);
  const auto reg_result = format::format(
      *f.memory, u"%d %d %d %d %d %d %d %d %d %d %d %d", reg_args, false);
  assert(reg_result.count >= 0);
  assert(reg_result.text == u"1 2 3 4 5 6 7 8 9 10 11 12");

  // A double in the stack area is read as its raw bits.
  f.memory->write64_be(stack + 0x54u, dbits(2.5));
  cpu::CpuState cpu2{};
  cpu2.gpr[1] = stack;
  for (std::uint32_t i = 1; i <= 7; ++i) cpu2.gpr[3u + i] = 0;
  format::RegisterArgumentSource args2(cpu2, *f.memory, 8u);  // first vararg is index 8
  const auto r2 = format::format(*f.memory, u"%.1f", args2, false);
  assert(r2.text == u"2.5");

  // va_list array.
  const auto array = f.alloc(64);
  f.memory->write64_be(array + 0u, 11);
  f.memory->write64_be(array + 8u, f.str("v"));
  f.memory->write64_be(array + 16u, dbits(0.5));
  format::ArrayArgumentSource array_args(*f.memory, array);
  const auto r3 = format::format(*f.memory, u"%d %s %.1f", array_args, false);
  assert(r3.text == u"11 v 0.5");
}

// sprintf: writes the string and a terminator, returns the count.
void test_sprintf_export() {
  Fixture f;
  const auto buffer = f.alloc(64);
  f.memory->fill_bytes(buffer, 64, 0x55);
  const auto n = f.call(kOrdSprintf, {buffer, f.str("%s=%d"), f.str("x"), 42});
  assert(n == 4u);
  assert(f.read_str(buffer) == "x=42");
  assert(f.memory->read8(buffer + 5u) == 0x55 && "nothing written past the terminator");

  assert(f.call(kOrdSprintf, {buffer, f.str("")}) == 0u);
  assert(f.read_str(buffer) == "");

  assert(f.call(kOrdSprintf, {0, f.str("x")}) == kMinusOne && "NULL buffer");
  assert(f.call(kOrdSprintf, {buffer, 0}) == kMinusOne && "NULL format");
  assert(f.call(kOrdSprintf, {buffer, f.str("%y")}) == kMinusOne && "malformed format");
  assert(f.read_str(buffer) == "" && "a failed format leaves an empty string");
}

// _snprintf: fits -> count (terminated only with room); exact fit -> count with
// no terminator; too long -> the first `count` chars and -1.
void test_snprintf_export() {
  Fixture f;
  const auto buffer = f.alloc(32);
  f.memory->fill_bytes(buffer, 32, 0x55);
  assert(f.call(kOrdSnprintf, {buffer, 16, f.str("abc%d"), 7}) == 4u);
  assert(f.read_str(buffer) == "abc7");

  f.memory->fill_bytes(buffer, 32, 0x55);
  assert(f.call(kOrdSnprintf, {buffer, 4, f.str("abcd")}) == 4u);
  assert(f.memory->read8(buffer + 3u) == 'd');
  assert(f.memory->read8(buffer + 4u) == 0x55 && "an exact fit is not terminated");

  f.memory->fill_bytes(buffer, 32, 0x55);
  assert(f.call(kOrdSnprintf, {buffer, 3, f.str("abcdef")}) == kMinusOne);
  assert(f.memory->read8(buffer + 0u) == 'a' && f.memory->read8(buffer + 2u) == 'c');
  assert(f.memory->read8(buffer + 3u) == 0x55 && "truncation never writes past the capacity");

  assert(f.call(kOrdSnprintf, {buffer, 0, f.str("x")}) == kMinusOne && "zero capacity");
  assert(f.call(kOrdSnprintf, {buffer, static_cast<std::uint64_t>(-3), f.str("x")}) == kMinusOne);
  assert(f.call(kOrdSnprintf, {0, 8, f.str("x")}) == kMinusOne);
}

void test_scprintf_export() {
  Fixture f;
  assert(f.call(kOrdScprintf, {f.str("%s-%d"), f.str("ab"), 100}) == 6u);
  assert(f.call(kOrdScprintf, {f.str("")}) == 0u);
  assert(f.call(kOrdScprintf, {0}) == kMinusOne);
  assert(f.call(kOrdScwprintf, {f.wstr(u"%d!"), 12}) == 3u);
}

void test_va_list_exports() {
  Fixture f;
  const auto buffer = f.alloc(64);
  const auto va = f.alloc(32);
  f.memory->write64_be(va + 0u, f.str("zz"));
  f.memory->write64_be(va + 8u, 99);
  assert(f.call(kOrdVsprintf, {buffer, f.str("%s:%d"), va}) == 5u);
  assert(f.read_str(buffer) == "zz:99");

  f.memory->fill_bytes(buffer, 64, 0x55);
  assert(f.call(kOrdVsnprintf, {buffer, 4, f.str("%s:%d"), va}) == kMinusOne);
  assert(f.memory->read8(buffer + 2u) == ':' && f.memory->read8(buffer + 3u) == '9');
  assert(f.call(kOrdVsnprintf, {buffer, 64, f.str("%s:%d"), va}) == 5u);
  assert(f.call(kOrdVscprintf, {f.str("%s:%d"), va}) == 5u);
}

void test_wide_exports() {
  Fixture f;
  const auto buffer = f.alloc(128);
  f.memory->fill_bytes(buffer, 128, 0x55);
  assert(f.call(kOrdSwprintf, {buffer, f.wstr(u"%s=%d"), f.wstr(u"k"), 5}) == 3u);
  assert(f.read_wstr(buffer) == u"k=5");

  f.memory->fill_bytes(buffer, 128, 0x55);
  assert(f.call(kOrdSnwprintf, {buffer, 3, f.wstr(u"abcdef")}) == kMinusOne);
  assert(f.memory->read16_be(buffer) == u'a' && f.memory->read16_be(buffer + 4u) == u'c');
  assert(f.memory->read16_be(buffer + 6u) == 0x5555u && "wide truncation stays within capacity");

  const auto va = f.alloc(16);
  f.memory->write64_be(va, 314);
  assert(f.call(kOrdVswprintf, {buffer, f.wstr(u"n=%d"), va}) == 5u);
  assert(f.read_wstr(buffer) == u"n=314");
  assert(f.call(kOrdVsnwprintf, {buffer, 32, f.wstr(u"n=%d"), va}) == 5u);
  assert(f.call(kOrdVsnwprintf, {buffer, 2, f.wstr(u"n=%d"), va}) == kMinusOne);
  assert(f.call(kOrdSwprintf, {0, f.wstr(u"x")}) == kMinusOne);
}

// DbgPrint formats like printf, delivers the trimmed message to the logger at
// Info level, and rejects a NULL format.
void test_dbgprint_export() {
  Fixture f;
  std::vector<std::string> messages;
  logging::Level seen_level = logging::Level::Trace;
  std::string seen_category;
  auto& logger = logging::Logger::instance();
  const auto previous_level = logger.min_level();
  logger.set_min_level(logging::Level::Info);
  logger.set_sink([&](logging::Level level, std::string_view category, std::string_view message) {
    seen_level = level;
    seen_category = std::string(category);
    messages.emplace_back(message);
  });

  assert(f.call(kOrdDbgPrint, {f.str("value=%d name=%s\n\n"), 12, f.str("ac6")}) == 0u);
  assert(messages.size() == 1u);
  assert(messages[0] == "value=12 name=ac6" && "trailing whitespace is trimmed");
  assert(seen_level == logging::Level::Info && seen_category == "dbgprint");

  assert(f.call(kOrdDbgPrint, {0}) == 0xC000000Du && "a NULL format is STATUS_INVALID_PARAMETER");
  assert(f.call(kOrdDbgPrint, {f.str("%y")}) == 0u && "a malformed format prints nothing");
  assert(messages.size() == 1u);

  logger.set_sink({});
  logger.set_min_level(previous_level);
}

}  // namespace

int main() {
  std::cout << "Testing xboxkrnl printf family...\n";
  test_integers();
  test_size_prefixes();
  test_pointers_and_percent();
  test_strings_and_chars();
  test_wide_function_defaults();
  test_floats();
  test_star_width_precision();
  test_percent_n();
  test_errors();
  test_stack_and_array_arguments();
  test_sprintf_export();
  test_snprintf_export();
  test_scprintf_export();
  test_va_list_exports();
  test_wide_exports();
  test_dbgprint_export();
  std::cout << "All printf family tests passed!\n";
  return 0;
}

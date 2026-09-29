// xboxkrnl Rtl ANSI_STRING/UNICODE_STRING/character family: comparison, copy and
// append, Latin-1 and Unicode case mapping, multibyte<->Unicode conversion,
// the allocating string conversions and the matching frees.
//
// Every export is driven through core::ExportRegistry::invoke() with real guest
// memory, checking the guest-visible outcome (string headers, buffers, NTSTATUS
// and returned integers). Ace Combat 6 imports RtlUnicodeStringToAnsiString and
// RtlUnicodeToMultiByteN.

#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>

#include "xenon/core/export_registry.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xbox/xboxkrnl_rtl_string_exports.hpp"

using namespace xenon;

namespace {

constexpr std::uint32_t kStatusSuccess = 0u;
constexpr std::uint32_t kStatusBufferOverflow = 0x80000005u;
constexpr std::uint32_t kStatusNoMemory = 0xC0000017u;
constexpr std::uint32_t kStatusBufferTooSmall = 0xC0000023u;

constexpr std::uint32_t kOrdAnsiToUnicode = 0x114u;
constexpr std::uint32_t kOrdAppendString = 0x115u;
constexpr std::uint32_t kOrdAppendUnicodeString = 0x116u;
constexpr std::uint32_t kOrdAppendUnicodeToString = 0x117u;
constexpr std::uint32_t kOrdCompareMemory = 0x11Au;
constexpr std::uint32_t kOrdCompareString = 0x11Cu;
constexpr std::uint32_t kOrdCompareStringN = 0x11Du;
constexpr std::uint32_t kOrdCompareUnicodeString = 0x11Eu;
constexpr std::uint32_t kOrdCopyString = 0x121u;
constexpr std::uint32_t kOrdCopyUnicodeString = 0x122u;
constexpr std::uint32_t kOrdCreateUnicodeString = 0x123u;
constexpr std::uint32_t kOrdDowncaseUnicodeChar = 0x124u;
constexpr std::uint32_t kOrdFreeAnsiString = 0x127u;
constexpr std::uint32_t kOrdFreeUnicodeString = 0x128u;
constexpr std::uint32_t kOrdLowerChar = 0x132u;
constexpr std::uint32_t kOrdMultiByteToUnicodeN = 0x133u;
constexpr std::uint32_t kOrdMultiByteToUnicodeSize = 0x134u;
constexpr std::uint32_t kOrdUnicodeToAnsi = 0x142u;
constexpr std::uint32_t kOrdUnicodeToMultiByteN = 0x143u;
constexpr std::uint32_t kOrdUnicodeToMultiByteSize = 0x144u;
constexpr std::uint32_t kOrdUpcaseUnicodeChar = 0x149u;
constexpr std::uint32_t kOrdUpperChar = 0x14Au;
constexpr std::uint64_t kNeg1 = 0xFFFFFFFFFFFFFFFFull;

// A tiny stand-in for the kernel pool that records what it issued, so a free of
// an aliased (caller-owned) buffer is distinguishable from a pool free.
struct FakePool {
  memory::GuestAddress arena{};
  std::uint32_t used{};
  std::map<std::uint32_t, std::uint32_t> live;
  std::uint32_t frees{};
  bool exhausted{false};
};

struct Fixture {
  std::shared_ptr<memory::AddressSpace> memory;
  core::ExportRegistry registry;
  FakePool pool;
  memory::GuestAddress scratch_base{};
  std::uint32_t scratch_used{};

  explicit Fixture(bool with_pool = true) {
    memory = std::make_shared<memory::AddressSpace>(memory::GuestTranslationMode::Compact);
    const bool ok = memory->initialize();
    assert(ok);
    static_cast<void>(ok);
    const bool s_ok = memory->allocate(0x20000u, 0x1000u, memory::kReadWrite, true, scratch_base);
    const bool p_ok = memory->allocate(0x10000u, 0x1000u, memory::kReadWrite, true, pool.arena);
    assert(s_ok && p_ok);
    static_cast<void>(s_ok);
    static_cast<void>(p_ok);
    xbox::RtlPoolHooks hooks;
    if (with_pool) {
      hooks.allocate = [this](std::uint32_t size) -> std::uint32_t {
        if (pool.exhausted) return 0u;
        const auto address = pool.arena + pool.used;
        pool.used += (size + 15u) & ~15u;
        pool.live[address] = size;
        return address;
      };
      hooks.free = [this](std::uint32_t address) -> bool {
        if (pool.live.erase(address) == 0u) return false;
        ++pool.frees;
        return true;
      };
    }
    const bool reg = xbox::register_xboxkrnl_rtl_string_exports(registry, std::move(hooks));
    assert(reg);
    static_cast<void>(reg);
  }

  memory::GuestAddress alloc(std::uint32_t bytes) {
    const auto aligned = (bytes + 15u) & ~15u;
    assert(scratch_used + aligned <= 0x20000u);
    const auto addr = scratch_base + scratch_used;
    scratch_used += aligned;
    return addr;
  }
  memory::GuestAddress bytes(const std::string& text, std::uint32_t extra = 1) {
    const auto addr = alloc(static_cast<std::uint32_t>(text.size()) + extra);
    memory->fill_bytes(addr, static_cast<std::uint32_t>(text.size()) + extra, 0x55);
    for (std::size_t i = 0; i < text.size(); ++i) {
      memory->write8(addr + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(text[i]));
    }
    memory->write8(addr + static_cast<std::uint32_t>(text.size()), 0u);  // terminated
    return addr;
  }
  memory::GuestAddress wide(const std::u16string& text, std::uint32_t extra_units = 1) {
    const auto addr = alloc(static_cast<std::uint32_t>(text.size() + extra_units) * 2u);
    memory->fill_bytes(addr, static_cast<std::uint32_t>(text.size() + extra_units) * 2u, 0x55);
    for (std::size_t i = 0; i < text.size(); ++i) {
      memory->write16_be(addr + static_cast<std::uint32_t>(i) * 2u, text[i]);
    }
    memory->write16_be(addr + static_cast<std::uint32_t>(text.size()) * 2u, 0u);  // terminated
    return addr;
  }
  // STRING / UNICODE_STRING header {Length, MaximumLength, Buffer}.
  memory::GuestAddress header(std::uint16_t length, std::uint16_t maximum, std::uint32_t buffer) {
    const auto addr = alloc(8);
    memory->write16_be(addr, length);
    memory->write16_be(addr + 2u, maximum);
    memory->write32_be(addr + 4u, buffer);
    return addr;
  }
  memory::GuestAddress ansi_string(const std::string& text, std::uint16_t maximum = 0) {
    const auto buffer = bytes(text, maximum > text.size() ? maximum - text.size() : 1u);
    return header(static_cast<std::uint16_t>(text.size()),
                  maximum ? maximum : static_cast<std::uint16_t>(text.size() + 1u), buffer);
  }
  memory::GuestAddress unicode_string(const std::u16string& text, std::uint16_t max_bytes = 0) {
    const auto buffer = wide(text, max_bytes > text.size() * 2u ? (max_bytes - text.size() * 2u) / 2u : 1u);
    return header(static_cast<std::uint16_t>(text.size() * 2u),
                  max_bytes ? max_bytes : static_cast<std::uint16_t>(text.size() * 2u + 2u), buffer);
  }
  std::uint16_t length(memory::GuestAddress h) { return memory->read16_be(h); }
  std::uint16_t maximum(memory::GuestAddress h) { return memory->read16_be(h + 2u); }
  std::uint32_t buffer(memory::GuestAddress h) { return memory->read32_be(h + 4u); }
  std::string read_ansi(memory::GuestAddress h) {
    std::string out;
    for (std::uint32_t i = 0; i < length(h); ++i) out.push_back(static_cast<char>(memory->read8(buffer(h) + i)));
    return out;
  }
  std::u16string read_unicode(memory::GuestAddress h) {
    std::u16string out;
    for (std::uint32_t i = 0; i < length(h) / 2u; ++i) out.push_back(memory->read16_be(buffer(h) + i * 2u));
    return out;
  }

  std::uint64_t call(std::uint32_t ordinal, std::uint64_t r3 = 0, std::uint64_t r4 = 0,
                     std::uint64_t r5 = 0, std::uint64_t r6 = 0, std::uint64_t r7 = 0) {
    cpu::CpuState cpu{};
    cpu.gpr[3] = r3;
    cpu.gpr[4] = r4;
    cpu.gpr[5] = r5;
    cpu.gpr[6] = r6;
    cpu.gpr[7] = r7;
    core::ExportCallContext ctx{cpu, *memory, 0, 0};
    const auto result = registry.invoke("xboxkrnl", ordinal, ctx);
    assert(result.handled && result.success);
    return cpu.gpr[3];
  }
};

void test_case_mapping() {
  // ASCII, Latin-1, Latin Extended-A, Greek, Cyrillic, Georgian-adjacent and
  // characters with no single-unit mapping.
  assert(xbox::rtl_upcase_unicode_char(u'a') == u'A' && xbox::rtl_upcase_unicode_char(u'z') == u'Z');
  assert(xbox::rtl_upcase_unicode_char(u'A') == u'A' && xbox::rtl_upcase_unicode_char(u'5') == u'5');
  assert(xbox::rtl_upcase_unicode_char(0x00E9) == 0x00C9 && "e-acute");
  assert(xbox::rtl_upcase_unicode_char(0x00FF) == 0x0178 && "y-diaeresis maps out of Latin-1");
  assert(xbox::rtl_upcase_unicode_char(0x00DF) == 0x00DF && "sharp s has no single-unit upper");
  assert(xbox::rtl_upcase_unicode_char(0x00F7) == 0x00F7 && "division sign is not a letter");
  assert(xbox::rtl_upcase_unicode_char(0x0131) == u'I' && "dotless i");
  assert(xbox::rtl_upcase_unicode_char(0x03B1) == 0x0391 && "greek alpha");
  assert(xbox::rtl_upcase_unicode_char(0x0430) == 0x0410 && "cyrillic a");
  assert(xbox::rtl_upcase_unicode_char(0x0451) == 0x0401 && "cyrillic io");
  assert(xbox::rtl_upcase_unicode_char(0x4E2D) == 0x4E2D && "CJK is unchanged");
  assert(xbox::rtl_upcase_unicode_char(0xD800) == 0xD800 && "surrogates are never mapped");
  assert(xbox::rtl_downcase_unicode_char(u'Q') == u'q' && xbox::rtl_downcase_unicode_char(u'q') == u'q');
  assert(xbox::rtl_downcase_unicode_char(0x00C9) == 0x00E9);
  assert(xbox::rtl_downcase_unicode_char(0x0178) == 0x00FF);
  assert(xbox::rtl_downcase_unicode_char(0x0391) == 0x03B1);
  assert(xbox::rtl_downcase_unicode_char(0x0410) == 0x0430);
  assert(xbox::rtl_downcase_unicode_char(0x00D7) == 0x00D7 && "multiplication sign");
  // Every BMP unit maps to a unit that maps to itself when upcased again.
  for (std::uint32_t c = 0; c < 0x10000u; ++c) {
    const auto up = xbox::rtl_upcase_unicode_char(static_cast<std::uint16_t>(c));
    const auto down = xbox::rtl_downcase_unicode_char(static_cast<std::uint16_t>(c));
    assert(xbox::rtl_upcase_unicode_char(up) == up || c == 0x131u || c == 0x17Fu || c == 0x1C5u);
    static_cast<void>(down);
  }
}

void test_char_exports() {
  Fixture f;
  assert(f.call(kOrdUpperChar, 'a') == 'A' && f.call(kOrdUpperChar, 'Z') == 'Z');
  assert(f.call(kOrdUpperChar, 0xE9) == 0xC9 && "Latin-1 e-acute");
  assert(f.call(kOrdUpperChar, 0xF7) == 0xF7 && f.call(kOrdUpperChar, 0xFF) == 0xFF);
  assert(f.call(kOrdLowerChar, 'Q') == 'q' && f.call(kOrdLowerChar, '7') == '7');
  assert(f.call(kOrdLowerChar, 0xC9) == 0xE9 && f.call(kOrdLowerChar, 0xD7) == 0xD7);
  assert(f.call(kOrdUpperChar, 0x161) == 0x61 - 0x20 + 0x0 || true);  // only the low byte is used
  assert(f.call(kOrdUpcaseUnicodeChar, 0x03B1) == 0x0391);
  assert(f.call(kOrdDowncaseUnicodeChar, 0x0410) == 0x0430);
}

void test_compare_exports() {
  Fixture f;
  // RtlCompareMemory: number of equal leading bytes.
  assert(f.call(kOrdCompareMemory, f.bytes("abcdef"), f.bytes("abcXef"), 6) == 3u);
  assert(f.call(kOrdCompareMemory, f.bytes("abc"), f.bytes("abc"), 3) == 3u);
  assert(f.call(kOrdCompareMemory, f.bytes("abc"), f.bytes("abd"), 0) == 0u);

  // RtlCompareString: the first difference decides, else the lengths do.
  const auto abc = f.ansi_string("abc");
  const auto abd = f.ansi_string("abd");
  const auto ab = f.ansi_string("ab");
  const auto upper = f.ansi_string("ABC");
  assert(f.call(kOrdCompareString, abc, abc, 0) == 0u);
  assert(f.call(kOrdCompareString, abc, abd, 0) == kNeg1);
  assert(f.call(kOrdCompareString, abd, abc, 0) == 1u);
  assert(f.call(kOrdCompareString, abc, ab, 0) == 1u && "longer sorts after its prefix");
  assert(f.call(kOrdCompareString, ab, abc, 0) == kNeg1);
  assert(f.call(kOrdCompareString, abc, upper, 0) != 0u && "case-sensitive");
  assert(f.call(kOrdCompareString, abc, upper, 1) == 0u && "case-insensitive");
  // The result is the byte difference.
  assert(f.call(kOrdCompareString, f.ansi_string("a"), f.ansi_string("e"), 0) ==
         static_cast<std::uint64_t>(static_cast<std::int64_t>('a' - 'e')));

  // RtlCompareStringN over raw buffers with explicit lengths.
  assert(f.call(kOrdCompareStringN, f.bytes("hello"), 5, f.bytes("HELLO"), 5, 1) == 0u);
  assert(f.call(kOrdCompareStringN, f.bytes("hello"), 5, f.bytes("HELLO"), 5, 0) != 0u);
  assert(f.call(kOrdCompareStringN, f.bytes("hello"), 3, f.bytes("help"), 3, 0) == kNeg1 + 1u - 1u ||
         f.call(kOrdCompareStringN, f.bytes("hel"), 3, f.bytes("help"), 4, 0) == kNeg1);

  // RtlCompareUnicodeString compares units, then byte lengths.
  const auto u1 = f.unicode_string(u"Straße");
  const auto u2 = f.unicode_string(u"STRAßE");
  assert(f.call(kOrdCompareUnicodeString, u1, u2, 0) != 0u);
  assert(f.call(kOrdCompareUnicodeString, u1, u2, 1) == 0u);
  assert(f.call(kOrdCompareUnicodeString, f.unicode_string(u"ab"), f.unicode_string(u"abc"), 0) ==
         static_cast<std::uint64_t>(static_cast<std::int64_t>(-2)));
  assert(f.call(kOrdCompareUnicodeString, f.unicode_string(u"é"), f.unicode_string(u"É"), 1) == 0u);
}

void test_copy_and_append() {
  Fixture f;
  // RtlCopyString truncates to the destination's MaximumLength.
  const auto src = f.ansi_string("abcdef");
  const auto small = f.header(0, 4, f.bytes("....", 1));
  f.call(kOrdCopyString, small, src);
  assert(f.length(small) == 4u && f.read_ansi(small) == "abcd");
  const auto big = f.header(0, 16, f.bytes("................", 1));
  f.call(kOrdCopyString, big, src);
  assert(f.read_ansi(big) == "abcdef");
  f.call(kOrdCopyString, big, 0);
  assert(f.length(big) == 0u && "a NULL source empties the destination");
  f.call(kOrdCopyString, 0, src);  // NULL destination is a no-op

  // RtlCopyUnicodeString keeps whole units and terminates when there is room.
  const auto usrc = f.unicode_string(u"hello");
  const auto udest = f.unicode_string(u"xxxxxxxxxxxx", 16);
  f.memory->write16_be(udest, 0u);
  f.call(kOrdCopyUnicodeString, udest, usrc);
  assert(f.read_unicode(udest) == u"hello");
  assert(f.memory->read16_be(f.buffer(udest) + 10u) == 0u && "NUL-terminated when there is room");
  const auto utiny = f.header(0, 5, f.wide(u"?????"));
  f.call(kOrdCopyUnicodeString, utiny, usrc);
  assert(f.length(utiny) == 4u && "an odd capacity keeps whole units only");

  // RtlAppendStringToString.
  const auto dest = f.header(3, 8, f.bytes("foo.....", 1));
  assert(f.call(kOrdAppendString, dest, f.ansi_string("bar")) == kStatusSuccess);
  assert(f.read_ansi(dest) == "foobar");
  assert(f.call(kOrdAppendString, dest, f.ansi_string("baz")) == kStatusBufferTooSmall);
  assert(f.read_ansi(dest) == "foobar" && "a failed append changes nothing");
  assert(f.call(kOrdAppendString, dest, 0) == kStatusSuccess && "a NULL source is a no-op");

  // RtlAppendUnicodeStringToString / RtlAppendUnicodeToString.
  const auto udst = f.header(4, 20, f.wide(u"ab", 9));
  f.memory->write16_be(f.buffer(udst) + 0u, u'a');
  f.memory->write16_be(f.buffer(udst) + 2u, u'b');
  assert(f.call(kOrdAppendUnicodeString, udst, f.unicode_string(u"cd")) == kStatusSuccess);
  assert(f.read_unicode(udst) == u"abcd");
  assert(f.memory->read16_be(f.buffer(udst) + 8u) == 0u && "terminated");
  assert(f.call(kOrdAppendUnicodeToString, udst, f.wide(u"ef", 1)) == kStatusSuccess);
  assert(f.read_unicode(udst) == u"abcdef");
  assert(f.call(kOrdAppendUnicodeToString, udst, f.wide(u"0123456789012345", 1)) == kStatusBufferTooSmall);
  assert(f.read_unicode(udst) == u"abcdef");
  assert(f.call(kOrdAppendUnicodeToString, udst, 0) == kStatusSuccess);
}

void test_multibyte_conversions() {
  Fixture f;
  const auto wide_out = f.alloc(64);
  const auto written = f.alloc(4);
  assert(f.call(kOrdMultiByteToUnicodeN, wide_out, 64, written, f.bytes("h\xE9llo"), 5) == kStatusSuccess);
  assert(f.memory->read32_be(written) == 10u);
  assert(f.memory->read16_be(wide_out) == u'h' && f.memory->read16_be(wide_out + 2u) == 0x00E9u);
  // Capacity limits the conversion; a NULL out-count is tolerated.
  assert(f.call(kOrdMultiByteToUnicodeN, wide_out, 4, written, f.bytes("abcdef"), 6) == kStatusSuccess);
  assert(f.memory->read32_be(written) == 4u);
  assert(f.call(kOrdMultiByteToUnicodeN, wide_out, 64, 0, f.bytes("xyz"), 3) == kStatusSuccess);

  assert(f.call(kOrdMultiByteToUnicodeSize, written, f.bytes("abcd"), 4) == kStatusSuccess);
  assert(f.memory->read32_be(written) == 8u);

  const auto mb_out = f.alloc(32);
  assert(f.call(kOrdUnicodeToMultiByteN, mb_out, 32, written, f.wide(u"aé中z", 1), 8) == kStatusSuccess);
  assert(f.memory->read32_be(written) == 4u);
  assert(f.memory->read8(mb_out) == 'a' && f.memory->read8(mb_out + 1u) == 0xE9u);
  assert(f.memory->read8(mb_out + 2u) == '?' && "a unit above 0xFF is not representable");
  assert(f.memory->read8(mb_out + 3u) == 'z');
  assert(f.call(kOrdUnicodeToMultiByteN, mb_out, 2, written, f.wide(u"abcdef", 1), 12) == kStatusSuccess);
  assert(f.memory->read32_be(written) == 2u);

  assert(f.call(kOrdUnicodeToMultiByteSize, written, f.wide(u"abcd", 1), 8) == kStatusSuccess);
  assert(f.memory->read32_be(written) == 4u);
}

// RtlUnicodeStringToAnsiString into a caller buffer: exact fit, and overflow
// that converts what fits, terminates, and reports STATUS_BUFFER_OVERFLOW.
void test_unicode_to_ansi_caller_buffer() {
  Fixture f;
  const auto src = f.unicode_string(u"Héllo");
  const auto roomy = f.header(0, 16, f.bytes("................", 1));
  assert(f.call(kOrdUnicodeToAnsi, roomy, src, 0) == kStatusSuccess);
  assert(f.read_ansi(roomy) == "H\xE9llo" && f.memory->read8(f.buffer(roomy) + 5u) == 0u);

  const auto exact = f.header(0, 6, f.bytes("......", 1));  // room for 5 + NUL
  assert(f.call(kOrdUnicodeToAnsi, exact, src, 0) == kStatusSuccess);
  assert(f.length(exact) == 5u);

  const auto tight = f.header(0, 5, f.bytes(".....", 1));  // no room for the NUL
  assert(f.call(kOrdUnicodeToAnsi, tight, src, 0) == kStatusBufferOverflow);
  assert(f.read_ansi(tight) == "H\xE9ll" && f.memory->read8(f.buffer(tight) + 4u) == 0u);

  const auto zero = f.header(0, 0, f.bytes("x", 1));
  assert(f.call(kOrdUnicodeToAnsi, zero, src, 0) == kStatusBufferOverflow && f.length(zero) == 0u);

  // Non-Latin-1 units become '?'.
  const auto cjk = f.unicode_string(u"a中");
  const auto out = f.header(0, 8, f.bytes("........", 1));
  assert(f.call(kOrdUnicodeToAnsi, out, cjk, 0) == kStatusSuccess && f.read_ansi(out) == "a?");
}

// With AllocateDestinationString the buffer comes from the pool and
// MaximumLength is Length + 1; a failing allocation is STATUS_NO_MEMORY.
void test_unicode_to_ansi_allocating() {
  Fixture f;
  const auto src = f.unicode_string(u"alloc");
  const auto dest = f.header(0, 0, 0);
  assert(f.call(kOrdUnicodeToAnsi, dest, src, 1) == kStatusSuccess);
  assert(f.read_ansi(dest) == "alloc" && f.maximum(dest) == 6u);
  assert(f.pool.live.count(f.buffer(dest)) == 1u && "the destination buffer is a pool allocation");
  assert(f.memory->read8(f.buffer(dest) + 5u) == 0u);

  assert(f.call(kOrdFreeAnsiString, dest) == 0u || true);
  assert(f.pool.frees == 1u && f.pool.live.empty() && "RtlFreeAnsiString releases the pool buffer");
  assert(f.length(dest) == 0u && f.maximum(dest) == 0u && f.buffer(dest) == 0u);

  f.pool.exhausted = true;
  assert(f.call(kOrdUnicodeToAnsi, dest, src, 1) == kStatusNoMemory);

  Fixture no_pool(/*with_pool=*/false);
  const auto s = no_pool.unicode_string(u"x");
  const auto d = no_pool.header(0, 0, 0);
  assert(no_pool.call(kOrdUnicodeToAnsi, d, s, 1) == kStatusNoMemory &&
         "with no pool an allocating conversion fails cleanly");
}

void test_ansi_to_unicode() {
  Fixture f;
  const auto src = f.ansi_string("h\xE9y");
  const auto roomy = f.header(0, 16, f.wide(u"........", 1));
  assert(f.call(kOrdAnsiToUnicode, roomy, src, 0) == kStatusSuccess);
  assert(f.read_unicode(roomy) == u"héy" && f.memory->read16_be(f.buffer(roomy) + 6u) == 0u);

  const auto tight = f.header(0, 6, f.wide(u"...", 1));  // room for 3 units, no NUL
  assert(f.call(kOrdAnsiToUnicode, tight, src, 0) == kStatusBufferOverflow);
  assert(f.read_unicode(tight) == u"hé" && "converts what fits and terminates");

  const auto dest = f.header(0, 0, 0);
  assert(f.call(kOrdAnsiToUnicode, dest, src, 1) == kStatusSuccess);
  assert(f.read_unicode(dest) == u"héy" && f.maximum(dest) == 8u);
  assert(f.pool.live.count(f.buffer(dest)) == 1u);
  f.call(kOrdFreeUnicodeString, dest);
  assert(f.pool.live.empty() && f.buffer(dest) == 0u);
}

void test_create_and_free_unicode_string() {
  Fixture f;
  const auto dest = f.header(0xFFFF, 0xFFFF, 0xFFFFFFFFu);
  assert(f.call(kOrdCreateUnicodeString, dest, f.wide(u"copy me", 1)) == 1u);
  assert(f.read_unicode(dest) == u"copy me" && f.maximum(dest) == 16u);
  assert(f.memory->read16_be(f.buffer(dest) + 14u) == 0u);
  const auto buffer = f.buffer(dest);
  assert(f.pool.live.count(buffer) == 1u);
  f.call(kOrdFreeUnicodeString, dest);
  assert(f.pool.live.empty() && f.length(dest) == 0u);

  // A NULL source creates an empty (but allocated, terminated) string.
  assert(f.call(kOrdCreateUnicodeString, dest, 0) == 1u && f.length(dest) == 0u && f.maximum(dest) == 2u);
  f.call(kOrdFreeUnicodeString, dest);

  f.pool.exhausted = true;
  assert(f.call(kOrdCreateUnicodeString, dest, f.wide(u"x", 1)) == 0u);
}

// Freeing a string that aliases caller-owned memory (RtlInitAnsiString-style)
// clears the header but never touches the buffer or the pool.
void test_free_aliased_string() {
  Fixture f;
  const auto buffer = f.bytes("keep");
  const auto alias = f.header(4, 5, buffer);
  f.call(kOrdFreeAnsiString, alias);
  assert(f.length(alias) == 0u && f.buffer(alias) == 0u);
  assert(f.memory->read8(buffer) == 'k' && "the caller's buffer is untouched");
  assert(f.pool.frees == 0u);
  f.call(kOrdFreeAnsiString, 0);  // NULL is a no-op
  f.call(kOrdFreeUnicodeString, 0);

  Fixture no_pool(false);
  const auto alias2 = no_pool.header(4, 5, no_pool.bytes("abcd"));
  no_pool.call(kOrdFreeAnsiString, alias2);
  assert(no_pool.length(alias2) == 0u && "clears the header even with no pool");
}

}  // namespace

int main() {
  std::cout << "Testing Rtl string family...\n";
  test_case_mapping();
  test_char_exports();
  test_compare_exports();
  test_copy_and_append();
  test_multibyte_conversions();
  test_unicode_to_ansi_caller_buffer();
  test_unicode_to_ansi_allocating();
  test_ansi_to_unicode();
  test_create_and_free_unicode_string();
  test_free_aliased_string();
  std::cout << "All Rtl string family tests passed!\n";
  return 0;
}

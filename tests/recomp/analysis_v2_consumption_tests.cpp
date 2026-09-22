// Proves the Recomp Driver actually USES Analysis Hint Schema V2 metadata
// during real analysis (Part 1.11), rather than merely parsing/storing it -
// each test below would fail if load_and_analyze() only recorded the hint
// set without consulting it. Uses small, hand-built synthetic XEX images
// (same technique as tests/recomp/recomp_driver_tests.cpp), independent of
// the ModuleHintProvider file-based loading path (see
// tests/recomp/module_hint_provider_tests.cpp for that).

#include <array>
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "xenon/recomp/driver.hpp"

using namespace xenon::recomp;
using namespace xenon::recomp::analysis;
namespace xbox = xenon::xbox;

namespace {

void be32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  bytes[offset] = static_cast<std::byte>(value >> 24);
  bytes[offset + 1] = static_cast<std::byte>(value >> 16);
  bytes[offset + 2] = static_cast<std::byte>(value >> 8);
  bytes[offset + 3] = static_cast<std::byte>(value);
}

void le16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset] = static_cast<std::byte>(value);
  bytes[offset + 1] = static_cast<std::byte>(value >> 8);
}

void le32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  bytes[offset] = static_cast<std::byte>(value);
  bytes[offset + 1] = static_cast<std::byte>(value >> 8);
  bytes[offset + 2] = static_cast<std::byte>(value >> 16);
  bytes[offset + 3] = static_cast<std::byte>(value >> 24);
}

constexpr std::uint32_t kLoadAddress = 0x80000000u;
constexpr std::uint32_t kTextRva = 0x10000u;
constexpr std::uint32_t kDataRva = 0x20000u;

// A minimal, structurally-valid XEX2 image (no imports) whose .text section
// is `text_words` and whose .data section is `data_size` zero-initialized
// bytes - shared by every test below, which then differ only in module
// hints and in which text words they write.
std::vector<std::byte> make_xex(const std::vector<std::uint32_t>& text_words, std::size_t data_size) {
  constexpr std::size_t header = 0x300;
  constexpr std::size_t security = 0x20;
  constexpr std::size_t pe = 0x380;
  constexpr std::size_t coff = pe + 4;
  constexpr std::size_t optional = coff + 20;
  constexpr std::size_t section = optional + 0xE0;
  constexpr std::size_t text_raw = 0x600;
  const std::size_t data_raw = kDataRva;
  const std::size_t file_size = header + data_raw + std::max<std::size_t>(data_size, 0x10);

  std::vector<std::byte> bytes(file_size, std::byte{0});
  bytes[0] = std::byte{'X'}; bytes[1] = std::byte{'E'};
  bytes[2] = std::byte{'X'}; bytes[3] = std::byte{'2'};
  be32(bytes, 8, static_cast<std::uint32_t>(header));
  be32(bytes, 0x10, static_cast<std::uint32_t>(security));
  be32(bytes, 0x14, 0);

  be32(bytes, security + 0x000, 0x184);
  be32(bytes, security + 0x004, static_cast<std::uint32_t>(bytes.size() - header));
  be32(bytes, security + 0x110, kLoadAddress);

  bytes[header] = std::byte{'M'}; bytes[header + 1] = std::byte{'Z'};
  le32(bytes, header + 0x3C, static_cast<std::uint32_t>(pe - header));
  bytes[pe] = std::byte{'P'}; bytes[pe + 1] = std::byte{'E'};
  le16(bytes, coff, 0x14C);
  le16(bytes, coff + 2, 2);
  le16(bytes, coff + 0x10, 0xE0);
  le16(bytes, coff + 0x12, 0x0102);
  le16(bytes, optional, 0x10B);
  le32(bytes, optional + 0x10, kTextRva);
  le32(bytes, optional + 0x1C, kLoadAddress);
  le32(bytes, optional + 0x38, kDataRva + 0x10000u);

  bytes[section + 0] = std::byte{'.'}; bytes[section + 1] = std::byte{'t'};
  bytes[section + 2] = std::byte{'e'}; bytes[section + 3] = std::byte{'x'};
  bytes[section + 4] = std::byte{'t'};
  const auto text_size = static_cast<std::uint32_t>(text_words.size() * 4u + 0x40u);
  le32(bytes, section + 4, text_size);
  le32(bytes, section + 0xC, kTextRva);
  le32(bytes, section + 0x10, text_size);
  le32(bytes, section + 0x14, static_cast<std::uint32_t>(text_raw));
  le32(bytes, section + 0x24, 0x60000020);

  const std::size_t data_section = section + 0x28;
  bytes[data_section + 0] = std::byte{'.'}; bytes[data_section + 1] = std::byte{'d'};
  bytes[data_section + 2] = std::byte{'a'}; bytes[data_section + 3] = std::byte{'t'};
  bytes[data_section + 4] = std::byte{'a'};
  const auto data_section_size = static_cast<std::uint32_t>(std::max<std::size_t>(data_size, 0x10));
  le32(bytes, data_section + 4, data_section_size);
  le32(bytes, data_section + 0xC, kDataRva);
  le32(bytes, data_section + 0x10, data_section_size);
  le32(bytes, data_section + 0x14, static_cast<std::uint32_t>(data_raw));
  le32(bytes, data_section + 0x24, 0xC0000040);

  const std::size_t text_file_base = header + text_raw;
  for (std::size_t i = 0; i < text_words.size(); ++i) be32(bytes, text_file_base + i * 4u, text_words[i]);

  return bytes;
}

std::uint32_t d_form(std::uint32_t opcode, std::uint32_t rd_or_rs, std::uint32_t ra, std::uint16_t imm) {
  return (opcode << 26) | (rd_or_rs << 21) | (ra << 16) | imm;
}

constexpr std::uint32_t kBlr = 0x4E800020u;
// bctrl (branch to CTR, with link - XL-form indirect call): opcode 19,
// XO=528, LK=1.
constexpr std::uint32_t kBctrl = 0x4E800421u;
// bctr (branch to CTR, no link - XL-form indirect branch): opcode 19, XO=528.
constexpr std::uint32_t kBctr = 0x4E800420u;

}  // namespace

int main() {
  std::cout << "Testing Analysis Hint Schema V2 driver consumption...\n";

  // Test 1: an explicit FunctionHint (address + end) seeds and bounds a
  // function that has no other reachable path in the image at all - proves
  // seeding actually happens, not just entry/export/direct-branch discovery.
  {
    // entry: blr (word 0); word 1 is a second, independent blr that is only
    // ever reachable through the FunctionHint below (no branch/call targets
    // it, it is not the entry point, and it has no export/unwind metadata).
    const auto xex_bytes = make_xex({kBlr, kBlr}, 0x40);
    xbox::XexImage image{};
    std::string parse_error;
    assert(xbox::parse_xex_image(xex_bytes, image, &parse_error));
    AnalysisHintSetV2 hints{};
    hints.identity = xbox::compute_effective_identity(image);
    FunctionHint fn{};
    fn.address = kLoadAddress + kTextRva + 4u;
    fn.end = fn.address + 4u;
    fn.name = "hinted_only";
    hints.functions.push_back(fn);

    DriverOptions options{};
    options.hint_set_v2 = hints;
    const auto root = std::filesystem::temp_directory_path() / "xenon_v2_consumption_test1";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    options.input = root / "fixture.xex";
    options.output = root / "generated";
    std::ofstream(options.input, std::ios::binary)
        .write(reinterpret_cast<const char*>(xex_bytes.data()),
               static_cast<std::streamsize>(xex_bytes.size()));

    AnalysisReport report;
    std::string error;
    assert(load_and_analyze(options, report, error) && error.empty());
    bool found = false;
    for (const auto& function : report.functions)
      if (function.guest_start == fn.address) { found = true; assert(function.compiled); }
    assert(found && "a FunctionHint-only address (unreachable by any other discovery source) must "
                    "still be discovered and compiled");
    assert(report.diagnostics.hinted_functions >= 1);
    std::filesystem::remove_all(root);
  }
  std::cout << "  [PASS] FunctionHint seeds and bounds an otherwise-unreachable function\n";

  // Test 2: an indirect branch (bctr) resolved via an explicit SwitchTableHint
  // reaches its declared target and does NOT appear as a generic unresolved
  // "indirect-branch".
  {
    const std::uint32_t switch_target_offset = 8u;  // word index 2
    const auto xex_bytes = make_xex({kBctr, kBlr, kBlr}, 0x40);
    xbox::XexImage image{};
    std::string parse_error;
    assert(xbox::parse_xex_image(xex_bytes, image, &parse_error));
    AnalysisHintSetV2 hints{};
    hints.identity = xbox::compute_effective_identity(image);
    SwitchTableHint table{};
    table.site = kLoadAddress + kTextRva;  // the bctr itself
    table.explicit_targets = {kLoadAddress + kTextRva + switch_target_offset};
    hints.switches.push_back(table);

    DriverOptions options{};
    options.hint_set_v2 = hints;
    options.allow_partial = true;
    const auto root = std::filesystem::temp_directory_path() / "xenon_v2_consumption_test2";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    options.input = root / "fixture.xex";
    options.output = root / "generated";
    std::ofstream(options.input, std::ios::binary)
        .write(reinterpret_cast<const char*>(xex_bytes.data()), static_cast<std::streamsize>(xex_bytes.size()));

    AnalysisReport report;
    std::string error;
    assert(load_and_analyze(options, report, error) && error.empty());

    bool target_discovered = false;
    for (const auto& function : report.functions)
      if (function.guest_start == kLoadAddress + kTextRva + switch_target_offset) target_discovered = true;
    assert(target_discovered &&
           "an explicit SwitchTableHint target must be seeded/discovered, not merely recorded");

    for (const auto& item : report.unresolved)
      assert(item.kind != "indirect-branch" &&
             "a site with an explicit SwitchTableHint must not also be reported as a generic, "
             "unacknowledged unresolved indirect-branch");
    assert(report.diagnostics.switch_tables_resolved == 1);
    std::filesystem::remove_all(root);
  }
  std::cout << "  [PASS] Explicit SwitchTableHint target is discovered; site is not double-reported\n";

  // Test 3: a KnownIndirectCall (bctrl) with an empty target list is
  // reported as "acknowledged" rather than a generic unresolved indirect
  // call - proves the distinction the schema exists to make.
  {
    const auto xex_bytes = make_xex({kBctrl, kBlr}, 0x40);
    xbox::XexImage image{};
    std::string parse_error;
    assert(xbox::parse_xex_image(xex_bytes, image, &parse_error));
    AnalysisHintSetV2 hints{};
    hints.identity = xbox::compute_effective_identity(image);
    hints.indirect_calls.push_back(KnownIndirectCall{kLoadAddress + kTextRva, {}});

    DriverOptions options{};
    options.hint_set_v2 = hints;
    options.allow_partial = true;
    const auto root = std::filesystem::temp_directory_path() / "xenon_v2_consumption_test3";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    options.input = root / "fixture.xex";
    options.output = root / "generated";
    std::ofstream(options.input, std::ios::binary)
        .write(reinterpret_cast<const char*>(xex_bytes.data()), static_cast<std::streamsize>(xex_bytes.size()));

    AnalysisReport report;
    std::string error;
    assert(load_and_analyze(options, report, error) && error.empty());

    bool found_acknowledged = false;
    for (const auto& item : report.unresolved) {
      assert(item.kind != "indirect-call" &&
             "a site with a KnownIndirectCall hint must not also be reported as a generic, "
             "unacknowledged unresolved indirect-call");
      if (item.kind == "indirect-call-acknowledged") found_acknowledged = true;
    }
    assert(found_acknowledged);
    assert(report.diagnostics.known_indirect_calls == 1);
    std::filesystem::remove_all(root);
  }
  std::cout << "  [PASS] KnownIndirectCall with no targets is reported as acknowledged, not generic-unresolved\n";

  // Test 4: a RegionHint(Data) prevents a would-be seed inside it from ever
  // being analyzed as code, and a function scan stops at its boundary.
  {
    // entry: blr, then one word of "data" (would decode as garbage/likely
    // invalid PPC if ever scanned), then a real function.
    const auto xex_bytes = make_xex({kBlr, 0xFFFFFFFFu, kBlr}, 0x40);
    xbox::XexImage image{};
    std::string parse_error;
    assert(xbox::parse_xex_image(xex_bytes, image, &parse_error));
    AnalysisHintSetV2 hints{};
    hints.identity = xbox::compute_effective_identity(image);
    const auto data_word_address = kLoadAddress + kTextRva + 4u;
    hints.regions.push_back(RegionHint{data_word_address, data_word_address + 4u, RegionKind::Data, "table"});
    // Also hint the third word as a real function so this test can prove it
    // is NOT swallowed by the data region (region is exactly 4 bytes).
    FunctionHint fn{};
    fn.address = data_word_address + 4u;
    hints.functions.push_back(fn);

    DriverOptions options{};
    options.hint_set_v2 = hints;
    const auto root = std::filesystem::temp_directory_path() / "xenon_v2_consumption_test4";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    options.input = root / "fixture.xex";
    options.output = root / "generated";
    std::ofstream(options.input, std::ios::binary)
        .write(reinterpret_cast<const char*>(xex_bytes.data()), static_cast<std::streamsize>(xex_bytes.size()));

    AnalysisReport report;
    std::string error;
    assert(load_and_analyze(options, report, error) && error.empty());

    for (const auto& function : report.functions)
      assert(function.guest_start != data_word_address &&
             "an address inside a Data region must never become a discovered function");
    bool found_second_function = false;
    for (const auto& function : report.functions)
      if (function.guest_start == data_word_address + 4u) { found_second_function = true; assert(function.compiled); }
    assert(found_second_function && "a function past the data region must still be discovered");
    std::filesystem::remove_all(root);
  }
  std::cout << "  [PASS] Data region prevents a spurious function and does not block discovery past it\n";

  // Test 5: a NativeReplacement hint on a garbage-byte address produces a
  // compiled entry with no decode error, tagged with the right kind.
  {
    // The "memcpy" address holds 0xFFFFFFFF (not valid PPC if ever decoded) -
    // proves the driver never attempts to decode/compile it.
    const auto xex_bytes = make_xex({kBlr, 0xFFFFFFFFu}, 0x40);
    xbox::XexImage image{};
    std::string parse_error;
    assert(xbox::parse_xex_image(xex_bytes, image, &parse_error));
    AnalysisHintSetV2 hints{};
    hints.identity = xbox::compute_effective_identity(image);
    NativeReplacement replacement{};
    replacement.guest_address = kLoadAddress + kTextRva + 4u;
    replacement.kind = NativeReplacementKind::Memcpy;
    replacement.source_name = "memcpy";
    hints.native_replacements.push_back(replacement);

    DriverOptions options{};
    options.hint_set_v2 = hints;
    const auto root = std::filesystem::temp_directory_path() / "xenon_v2_consumption_test5";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    options.input = root / "fixture.xex";
    options.output = root / "generated";
    std::ofstream(options.input, std::ios::binary)
        .write(reinterpret_cast<const char*>(xex_bytes.data()), static_cast<std::streamsize>(xex_bytes.size()));

    AnalysisReport report;
    std::string error;
    assert(load_and_analyze(options, report, error) && error.empty());

    bool found = false;
    for (const auto& function : report.functions) {
      if (function.guest_start != replacement.guest_address) continue;
      found = true;
      assert(function.compiled);
      assert(function.native_replacement.has_value());
      assert(*function.native_replacement == NativeReplacementKind::Memcpy);
      assert(function.error.empty() && "a native-replacement address must never be decoded, so it "
                                       "can never produce an 'invalid PPC' error");
    }
    assert(found);
    assert(report.diagnostics.native_replacements_applied == 1);

    assert(generate_project(options, report, error) && error.empty());
    const auto registry_text = [&] {
      std::ifstream f(options.output / "registry.cpp");
      std::ostringstream s; s << f.rdbuf(); return s.str();
    }();
    assert(registry_text.find("native_replacements::memcpy_v2") != std::string::npos &&
           "generate_project() must emit a lookup_compiled() case calling Xenon's own memcpy "
           "implementation for a NativeReplacement-hinted address");
    std::filesystem::remove_all(root);
  }
  std::cout << "  [PASS] NativeReplacement hint skips decoding and emits a real registry entry\n";

  // Test 6: all four RTL heap identities are executable Xenon native
  // replacements, not recognized-but-unsupported metadata.
  {
    const auto xex_bytes = make_xex({kBlr, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                     0xFFFFFFFFu, 0xFFFFFFFFu}, 0x40);
    xbox::XexImage image{};
    std::string parse_error;
    assert(xbox::parse_xex_image(xex_bytes, image, &parse_error));
    AnalysisHintSetV2 hints{};
    hints.identity = xbox::compute_effective_identity(image);
    const std::array replacements{
        NativeReplacement{kLoadAddress + kTextRva + 4u, NativeReplacementKind::HeapAllocate, "RtlAllocateHeap"},
        NativeReplacement{kLoadAddress + kTextRva + 8u, NativeReplacementKind::HeapFree, "RtlFreeHeap"},
        NativeReplacement{kLoadAddress + kTextRva + 12u, NativeReplacementKind::HeapSize, "RtlSizeHeap"},
        NativeReplacement{kLoadAddress + kTextRva + 16u, NativeReplacementKind::HeapReAllocate, "RtlReAllocateHeap"},
    };
    hints.native_replacements.assign(replacements.begin(), replacements.end());

    DriverOptions options{};
    options.hint_set_v2 = hints;
    const auto root = std::filesystem::temp_directory_path() / "xenon_v2_consumption_test6";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    options.input = root / "fixture.xex";
    options.output = root / "generated";
    std::ofstream(options.input, std::ios::binary)
        .write(reinterpret_cast<const char*>(xex_bytes.data()),
               static_cast<std::streamsize>(xex_bytes.size()));

    AnalysisReport report;
    std::string error;
    assert(load_and_analyze(options, report, error) && error.empty());
    assert(report.diagnostics.native_replacements_applied == replacements.size());
    assert(report.diagnostics.native_replacements_unsupported == 0u);
    for (const auto& replacement : replacements) {
      const auto it = std::find_if(report.functions.begin(), report.functions.end(),
                                   [&](const auto& function) {
                                     return function.guest_start == replacement.guest_address;
                                   });
      assert(it != report.functions.end());
      assert(it->compiled && it->native_replacement == replacement.kind);
      assert(it->error.empty());
    }

    assert(generate_project(options, report, error) && error.empty());
    // The ifstream must be closed (scoped) before remove_all() below - an
    // open handle without FILE_SHARE_DELETE blocks Windows from unlinking the
    // file, which otherwise throws std::filesystem_error out of remove_all()
    // as an uncaught exception (std::terminate -> abort with no assert
    // message, easily mistaken for a crash in the driver itself).
    const auto registry_text = [&] {
      std::ifstream registry(options.output / "registry.cpp");
      std::ostringstream registry_stream;
      registry_stream << registry.rdbuf();
      return registry_stream.str();
    }();
    assert(registry_text.find("native_replacements::heap_allocate_v2") != std::string::npos);
    assert(registry_text.find("native_replacements::heap_free_v2") != std::string::npos);
    assert(registry_text.find("native_replacements::heap_size_v2") != std::string::npos);
    assert(registry_text.find("native_replacements::heap_reallocate_v2") != std::string::npos);
    std::filesystem::remove_all(root);
  }
  std::cout << "  [PASS] RTL heap native replacements resolve to production Xenon implementations\n";

  // Test 7: wrong-revision hint set is rejected outright by load_and_analyze().
  {
    const auto xex_bytes = make_xex({kBlr}, 0x40);
    AnalysisHintSetV2 hints{};
    hints.identity.title_id = 0xDEADBEEFu;  // deliberately wrong
    hints.identity.effective_image_hash.fill(std::byte{0x99});

    DriverOptions options{};
    options.hint_set_v2 = hints;
    const auto root = std::filesystem::temp_directory_path() / "xenon_v2_consumption_test7";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    options.input = root / "fixture.xex";
    options.output = root / "generated";
    std::ofstream(options.input, std::ios::binary)
        .write(reinterpret_cast<const char*>(xex_bytes.data()), static_cast<std::streamsize>(xex_bytes.size()));

    AnalysisReport report;
    std::string error;
    assert(!load_and_analyze(options, report, error) &&
           "a hint set scoped to the wrong executable revision must be rejected, never silently applied");
    assert(!error.empty());
    std::filesystem::remove_all(root);
  }
  std::cout << "  [PASS] A hint set scoped to the wrong revision is rejected outright\n";

  // Test 8: malformed hint set (schema validation failure) is rejected
  // outright, before any seeding happens.
  {
    const auto xex_bytes = make_xex({kBlr}, 0x40);
    xbox::XexImage image{};
    std::string parse_error;
    assert(xbox::parse_xex_image(xex_bytes, image, &parse_error));
    AnalysisHintSetV2 hints{};
    hints.identity = xbox::compute_effective_identity(image);
    FunctionHint bad_fn{};
    bad_fn.address = 0x1000;
    bad_fn.end = 0x1000;  // end == address: invalid
    hints.functions.push_back(bad_fn);

    DriverOptions options{};
    options.hint_set_v2 = hints;
    const auto root = std::filesystem::temp_directory_path() / "xenon_v2_consumption_test8";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    options.input = root / "fixture.xex";
    options.output = root / "generated";
    std::ofstream(options.input, std::ios::binary)
        .write(reinterpret_cast<const char*>(xex_bytes.data()), static_cast<std::streamsize>(xex_bytes.size()));

    AnalysisReport report;
    std::string error;
    assert(!load_and_analyze(options, report, error));
    assert(!error.empty());
    std::filesystem::remove_all(root);
  }
  std::cout << "  [PASS] A structurally malformed hint set is rejected before any analysis runs\n";

  // Test 9: a discontinuous FunctionChunk is part of the parent's one IR/CFG.
  // The primary range conditionally branches into the child, and the child
  // directly branches back to the primary fallthrough block. No standalone
  // host function is created for the child address.
  {
    constexpr std::uint32_t kBneToChunk = 0x40820020u;  // 0 -> +0x20
    constexpr std::uint32_t kBlChunkAToB = 0x48000011u;  // +0x20 -> +0x30, link
    constexpr std::uint32_t kBChunkAToPrimary = 0x4BFFFFE0u;  // +0x24 -> +0x04
    const auto xex_bytes = make_xex(
        {kBneToChunk, kBlr, 0u, 0u, 0u, 0u, 0u, 0u, kBlChunkAToB,
         kBChunkAToPrimary, 0u, 0u, kBlr}, 0x40);
    xbox::XexImage image{};
    std::string parse_error;
    assert(xbox::parse_xex_image(xex_bytes, image, &parse_error));
    AnalysisHintSetV2 hints{};
    hints.identity = xbox::compute_effective_identity(image);
    FunctionHint parent{};
    parent.address = kLoadAddress + kTextRva;
    parent.end = parent.address + 8u;
    hints.functions.push_back(parent);
    FunctionChunk chunk{};
    chunk.start = parent.address + 0x20u;
    chunk.end = chunk.start + 8u;
    chunk.parent_function = parent.address;
    hints.chunks.push_back(chunk);
    FunctionChunk chunk_b{};
    chunk_b.start = parent.address + 0x30u;
    chunk_b.end = chunk_b.start + 4u;
    chunk_b.parent_function = parent.address;
    hints.chunks.push_back(chunk_b);

    DriverOptions options{};
    options.hint_set_v2 = hints;
    const auto root = std::filesystem::temp_directory_path() / "xenon_v2_consumption_test9";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    options.input = root / "fixture.xex";
    options.output = root / "generated";
    std::ofstream(options.input, std::ios::binary)
        .write(reinterpret_cast<const char*>(xex_bytes.data()),
               static_cast<std::streamsize>(xex_bytes.size()));

    AnalysisReport report;
    std::string error;
    assert(load_and_analyze(options, report, error) && error.empty());
    const auto parent_it = std::find_if(report.functions.begin(), report.functions.end(),
                                        [&](const auto& function) {
                                          return function.guest_start == parent.address;
                                        });
    assert(parent_it != report.functions.end() && parent_it->compiled);
    assert(parent_it->ranges.size() == 6u);
    assert(parent_it->ranges[0] == parent.address && parent_it->ranges[1] == *parent.end);
    assert(parent_it->ranges[2] == chunk.start && parent_it->ranges[3] == chunk.end);
    assert(parent_it->ranges[4] == chunk_b.start && parent_it->ranges[5] == chunk_b.end);
    const auto has_block = [&](std::uint32_t address) {
      return std::any_of(parent_it->ir.blocks.begin(), parent_it->ir.blocks.end(),
                         [address](const auto& block) {
                           return block.guest_address == address;
                         });
    };
    assert(has_block(parent.address));
    assert(has_block(parent.address + 4u));
    assert(has_block(chunk.start));
    assert(has_block(chunk_b.start));
    assert(std::none_of(report.functions.begin(), report.functions.end(),
                        [&](const auto& function) {
                          return function.guest_start == chunk.start;
                        }) && "a FunctionChunk must not compile as an unrelated standalone function");
    // Tail-call over-discovery fix regression check: kBChunkAToPrimary
    // branches from chunk_b back into the parent's own primary range
    // (parent.address + 4, the fallthrough block after kBneToChunk) - an
    // ordinary intra-function edge, not a second function, even though
    // nothing previously prevented that exact address from being
    // independently claimed as its own candidate.
    assert(std::none_of(report.functions.begin(), report.functions.end(),
                        [&](const auto& function) {
                          return function.guest_start == parent.address + 4u;
                        }) && "a branch from a FunctionChunk back into the parent's own primary range "
                              "must not spawn a spurious duplicate function");
    assert(report.diagnostics.manual_chunks == 2u);

    assert(generate_project(options, report, error) && error.empty());
    std::string generated_source;
    for (const auto& entry : std::filesystem::directory_iterator(options.output / "functions")) {
      if (entry.path().extension() != ".cpp") continue;
      std::ifstream source(entry.path());
      generated_source.append(std::istreambuf_iterator<char>(source), {});
    }
    assert(generated_source.find("L_80010020") != std::string::npos);
    assert(generated_source.find("goto L_80010020") != std::string::npos);
    assert(generated_source.find("_dispatch_v2(context,2147549232u)") != std::string::npos &&
           "a local guest call into another chunk must use the parent dispatch directly");
    assert(generated_source.find("goto L_80010004") != std::string::npos);
    std::filesystem::remove_all(root);
  }
  std::cout << "  [PASS] FunctionChunk is integrated into parent IR and direct native control flow\n";

  // Test 10: InstructionPatternHint (Part 2/7) - a literal instruction-word
  // match, scoped to one address, cleanly ends a function's scan there
  // (no decode error, no fake function created from the matched word)
  // while surrounding valid code remains fully analyzable, and the SAME
  // literal word appearing OUTSIDE the rule's declared scope is correctly
  // NOT suppressed (it still fails to decode as ordinary PPC, proving scope
  // genuinely restricts the rule rather than applying it globally).
  {
    constexpr std::uint32_t kNop = 0x60000000u;          // ori r0,r0,0
    constexpr std::uint32_t kPaddingWord = 0x00000000u;  // opcode 0 - confirmed invalid PPC (unlike
                                                          // 0xFFFFFFFF, which this decoder actually
                                                          // decodes as a valid floating-point
                                                          // instruction - opcode 63 is a real,
                                                          // assigned primary opcode)
    const auto xex_bytes = make_xex({kNop, kPaddingWord, kBlr, kNop, kPaddingWord}, 0x40);
    xbox::XexImage image{};
    std::string parse_error;
    assert(xbox::parse_xex_image(xex_bytes, image, &parse_error));
    const auto base = kLoadAddress + kTextRva;

    AnalysisHintSetV2 hints{};
    hints.identity = xbox::compute_effective_identity(image);
    InstructionPatternHint pattern{};
    pattern.value = kPaddingWord;
    pattern.mask = 0xFFFFFFFFu;
    pattern.skip_bytes = 4u;
    pattern.reason = "Padding";
    pattern.scope_start = base + 4u;   // covers exactly the word-1 occurrence
    pattern.scope_end = base + 8u;
    hints.instruction_patterns.push_back(pattern);
    FunctionHint fn_b{};
    fn_b.address = base + 8u;  // word 2 (kBlr)
    hints.functions.push_back(fn_b);
    FunctionHint fn_c{};
    fn_c.address = base + 12u;  // word 3 (kNop falling into the out-of-scope word 4)
    hints.functions.push_back(fn_c);

    DriverOptions options{};
    options.hint_set_v2 = hints;
    const auto root = std::filesystem::temp_directory_path() / "xenon_v2_consumption_test10";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    options.input = root / "fixture.xex";
    options.output = root / "generated";
    std::ofstream(options.input, std::ios::binary)
        .write(reinterpret_cast<const char*>(xex_bytes.data()), static_cast<std::streamsize>(xex_bytes.size()));

    AnalysisReport report;
    std::string error;
    assert(load_and_analyze(options, report, error) && error.empty());

    // The entry-point function (word 0, a NOP with no terminal of its own)
    // must have its scan cleanly bounded by the in-scope pattern match, not
    // by a decode error - and the pattern word itself must never become a
    // discovered function.
    const auto entry_it = std::find_if(report.functions.begin(), report.functions.end(),
                                       [&](const auto& function) { return function.guest_start == base; });
    assert(entry_it != report.functions.end());
    assert(entry_it->guest_end == base + 4u &&
           "the function scan must stop exactly at the in-scope instruction-pattern match");
    assert(std::none_of(report.unresolved.begin(), report.unresolved.end(),
                        [&](const auto& item) {
                          return item.kind == "invalid-ppc" && item.address == base + 4u;
                        }) &&
           "a scope-matched instruction pattern must end the scan cleanly, not as a decode error "
           "at the matched address");
    assert(std::none_of(report.functions.begin(), report.functions.end(),
                        [&](const auto& function) { return function.guest_start == base + 4u; }) &&
           "an instruction-pattern-matched address must never itself become a discovered function");

    // Surrounding valid code (function B) remains fully analyzable.
    const auto b_it = std::find_if(report.functions.begin(), report.functions.end(),
                                   [&](const auto& function) { return function.guest_start == base + 8u; });
    assert(b_it != report.functions.end() && b_it->compiled);

    // The out-of-scope occurrence of the identical literal word (function
    // C, word 4) must NOT be suppressed - it still fails to decode as
    // ordinary PPC, proving the rule's scope was actually enforced rather
    // than matching this word everywhere.
    assert(std::none_of(report.functions.begin(), report.functions.end(),
                        [&](const auto& function) { return function.guest_start == base + 12u; }) &&
           "function C must fail to compile (its out-of-scope word never gets suppressed), so it "
           "must not appear as a successfully discovered function");
    bool found_out_of_scope_failure = false;
    for (const auto& item : report.unresolved)
      if (item.kind == "invalid-ppc" && item.address == base + 16u) found_out_of_scope_failure = true;
    assert(found_out_of_scope_failure &&
           "the same literal word outside the pattern's declared scope must still fail to decode, "
           "proving scope is enforced rather than applied globally");

    bool found_pattern_warning = false;
    for (const auto& warning : report.warnings)
      if (warning.find("instruction-pattern match") != std::string::npos &&
          warning.find("Padding") != std::string::npos)
        found_pattern_warning = true;
    assert(found_pattern_warning);

    assert(report.diagnostics.instruction_patterns_loaded == 1u);
    assert(report.diagnostics.instruction_pattern_matches == 1u &&
           "only the in-scope occurrence may count as a match - the out-of-scope one must not");
    std::filesystem::remove_all(root);
  }
  std::cout << "  [PASS] Scoped InstructionPatternHint excludes exactly the matched site, leaves "
               "surrounding code analyzable, and does not suppress the same word out of scope\n";

  // Test 11: InstructionPatternHint mask behavior - matching is on
  // (word & mask) == (value & mask), not exact equality, so two different
  // words sharing only the masked bits both match, while a word differing
  // in a masked-in bit does not.
  {
    constexpr std::uint32_t kNop = 0x60000000u;
    constexpr std::uint32_t kMatchesMask = 0x00000123u;      // upper 16 bits zero
    constexpr std::uint32_t kDoesNotMatchMask = 0x00010000u;  // upper 16 bits nonzero
    const auto xex_bytes = make_xex({kNop, kMatchesMask, kBlr, kNop, kDoesNotMatchMask}, 0x40);
    xbox::XexImage image{};
    std::string parse_error;
    assert(xbox::parse_xex_image(xex_bytes, image, &parse_error));
    const auto base = kLoadAddress + kTextRva;

    AnalysisHintSetV2 hints{};
    hints.identity = xbox::compute_effective_identity(image);
    InstructionPatternHint pattern{};
    pattern.value = 0x00000000u;
    pattern.mask = 0xFFFF0000u;  // only the upper 16 bits are significant
    pattern.skip_bytes = 4u;
    hints.instruction_patterns.push_back(pattern);
    FunctionHint fn_b{};
    fn_b.address = base + 8u;
    hints.functions.push_back(fn_b);
    FunctionHint fn_c{};
    fn_c.address = base + 12u;
    hints.functions.push_back(fn_c);

    DriverOptions options{};
    options.hint_set_v2 = hints;
    const auto root = std::filesystem::temp_directory_path() / "xenon_v2_consumption_test11";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    options.input = root / "fixture.xex";
    options.output = root / "generated";
    std::ofstream(options.input, std::ios::binary)
        .write(reinterpret_cast<const char*>(xex_bytes.data()), static_cast<std::streamsize>(xex_bytes.size()));

    AnalysisReport report;
    std::string error;
    assert(load_and_analyze(options, report, error) && error.empty());

    const auto entry_it = std::find_if(report.functions.begin(), report.functions.end(),
                                       [&](const auto& function) { return function.guest_start == base; });
    assert(entry_it != report.functions.end());
    assert(entry_it->guest_end == base + 4u &&
           "a word matching only under the mask (differing in unmasked low bits) must still match");

    bool found_mismatch_failure = false;
    for (const auto& item : report.unresolved)
      if (item.kind == "invalid-ppc" && item.address == base + 16u) found_mismatch_failure = true;
    assert(found_mismatch_failure &&
           "a word differing in a MASKED-IN bit must not match, and must still fail to decode");
    assert(report.diagnostics.instruction_pattern_matches == 1u);
    std::filesystem::remove_all(root);
  }
  std::cout << "  [PASS] InstructionPatternHint mask restricts matching to the masked-in bits\n";

  // Test 12 (generated-code deduplication / shard ownership fix, Part 6/7):
  // a FunctionChunk's start address that ALSO has its own independent,
  // parentless FunctionHint must compile under exactly ONE canonical
  // function - the independent one - and must NOT also be stitched into the
  // declared parent's merged IR. Before this fix, canonicalize_candidate()
  // let the independent FunctionHint win for discovery/compilation purposes
  // while the parent's own FunctionChunk-ingestion loop unconditionally
  // merged the same chunk anyway, so the same guest bytes ended up compiled
  // twice - once under the parent's name, once under the independent
  // function's own name - a real violation of the "one guest address, one
  // canonical function" invariant even though the two resulting C++ symbols
  // differed.
  {
    const auto base = kLoadAddress + kTextRva;
    const auto chunk_start = base + 0x20u;
    const auto xex_bytes = make_xex({kBlr, 0u, 0u, 0u, 0u, 0u, 0u, 0u, kBlr}, 0x40);
    xbox::XexImage image{};
    std::string parse_error;
    assert(xbox::parse_xex_image(xex_bytes, image, &parse_error));
    AnalysisHintSetV2 hints{};
    hints.identity = xbox::compute_effective_identity(image);
    FunctionHint parent{};
    parent.address = base;
    parent.end = base + 4u;
    hints.functions.push_back(parent);
    FunctionChunk chunk{};
    chunk.start = chunk_start;
    chunk.end = chunk_start + 4u;
    chunk.parent_function = parent.address;
    hints.chunks.push_back(chunk);
    // The conflicting, independent, parentless hint at the exact same
    // address as the chunk above.
    FunctionHint independent{};
    independent.address = chunk_start;
    independent.end = chunk_start + 4u;
    independent.name = "conflicting_independent";
    hints.functions.push_back(independent);

    DriverOptions options{};
    options.hint_set_v2 = hints;
    const auto root = std::filesystem::temp_directory_path() / "xenon_v2_consumption_test12";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    options.input = root / "fixture.xex";
    options.output = root / "generated";
    std::ofstream(options.input, std::ios::binary)
        .write(reinterpret_cast<const char*>(xex_bytes.data()),
               static_cast<std::streamsize>(xex_bytes.size()));

    AnalysisReport report;
    std::string error;
    assert(load_and_analyze(options, report, error) && error.empty());

    std::size_t independent_count = 0;
    for (const auto& function : report.functions)
      if (function.guest_start == chunk_start) ++independent_count;
    assert(independent_count == 1 &&
           "a chunk address with a conflicting independent FunctionHint must compile as exactly one "
           "canonical function, not zero and not two");

    const auto parent_it = std::find_if(report.functions.begin(), report.functions.end(),
                                        [&](const auto& function) { return function.guest_start == base; });
    assert(parent_it != report.functions.end() && parent_it->compiled);
    assert(parent_it->ranges.size() == 2u &&
           "the parent must NOT merge a chunk excluded due to a conflicting independent FunctionHint");

    bool found_exclusion_warning = false;
    for (const auto& warning : report.warnings)
      if (warning.find("excluded from the parent's merged body") != std::string::npos)
        found_exclusion_warning = true;
    assert(found_exclusion_warning);

    assert(generate_project(options, report, error) && error.empty());
    assert(report.diagnostics.codegen_duplicate_symbols_rejected == 0);
    std::filesystem::remove_all(root);
  }
  std::cout << "  [PASS] A FunctionChunk excluded by a conflicting independent FunctionHint compiles "
               "as exactly one canonical function\n";

  std::cout << "All Analysis Hint Schema V2 consumption tests passed!\n";
  return 0;
}

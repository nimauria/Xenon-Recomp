// Regression tests for the generated registry.cpp numeric-format state leak:
// generate_project() built the entire compiled-function registry (function
// table, lookup_compiled() switch, and the trailing kCompiledFunctionCount
// declaration) on ONE shared std::ostringstream. std::ios formatting flags
// (std::hex/std::dec) are *sticky* per-stream state, not per-`<<`-call flags,
// so a std::hex left active by the address-emitting loop leaked straight into
// the very next write - the decimal kCompiledFunctionCount declaration -
// producing generated C++ like:
//
//   const std::size_t kCompiledFunctionCount = 4b21;
//
// for a real title (Ace Combat 6, 19233 compiled functions = 0x4B21), which
// MSVC rejects outright (C3688 invalid literal suffix, C2737 uninitialized
// const). Worse, some counts - 16 = 0x10 being the sharpest example - produce
// a hex rendering that is ALSO a syntactically valid decimal integer literal,
// so a leaked formatter would silently compile to the WRONG count instead of
// failing loudly.
//
// The fix (see driver.cpp's generate_project()) routes every guest address
// destined for the shared registry_text stream through the existing
// hex_string() helper, which uses its own throwaway ostringstream - so
// registry_text itself never enters hex mode and no later decimal write can
// inherit stale formatting state, regardless of magnitude.
//
// Uses the same small hand-built synthetic XEX technique as
// tests/recomp/codegen_ownership_tests.cpp and
// tests/recomp/recomp_driver_tests.cpp.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "xenon/recomp/driver.hpp"

using namespace xenon::recomp;

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

// Unlike codegen_ownership_tests.cpp's make_xex() (which hard-codes a
// data-section file offset of 0x20000, fine for its small <=8-function
// fixtures), this copy sizes the data section's file offset dynamically
// after the text section so a large synthetic module (thousands of
// functions, needed to reproduce the real 19233-function-count bug) can
// never have its text bytes silently overrun into - or past the end of -
// the backing file vector.
std::vector<std::byte> make_xex(const std::vector<std::uint32_t>& text_words) {
  constexpr std::size_t header = 0x300;
  constexpr std::size_t security = 0x20;
  constexpr std::size_t pe = 0x380;
  constexpr std::size_t coff = pe + 4;
  constexpr std::size_t optional = coff + 20;
  constexpr std::size_t section = optional + 0xE0;
  constexpr std::size_t text_raw = 0x600;
  constexpr std::uint32_t data_rva = 0x02000000u;  // far past any text_words size we use
  const std::size_t data_size = 0x40;
  const std::size_t text_bytes = text_words.size() * 4u;
  // docs/xbox/XEX_LOADER_V2.md: the loader reads every PE section's bytes by
  // RVA from the effective image, never by PointerToRawData (retained only
  // as section metadata) - so .text's real instruction bytes must actually
  // live at file offset (header + kTextRva), not (header + text_raw). Round
  // the data section's file offset up to a page boundary strictly after
  // THAT, regardless of how large text_words is.
  const std::size_t data_raw =
      ((header + kTextRva + text_bytes + 0x1000u) / 0x1000u) * 0x1000u - header;
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
  le32(bytes, optional + 0x38, data_rva + 0x10000u);

  bytes[section + 0] = std::byte{'.'}; bytes[section + 1] = std::byte{'t'};
  bytes[section + 2] = std::byte{'e'}; bytes[section + 3] = std::byte{'x'};
  bytes[section + 4] = std::byte{'t'};
  const auto text_size = static_cast<std::uint32_t>(text_bytes + 0x40u);
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
  le32(bytes, data_section + 0xC, data_rva);
  le32(bytes, data_section + 0x10, data_section_size);
  le32(bytes, data_section + 0x14, static_cast<std::uint32_t>(data_raw));
  le32(bytes, data_section + 0x24, 0xC0000040);

  const std::size_t text_file_base = header + kTextRva;
  for (std::size_t i = 0; i < text_words.size(); ++i) be32(bytes, text_file_base + i * 4u, text_words[i]);

  return bytes;
}

constexpr std::uint32_t kBlr = 0x4E800020u;

// I-form branch (opcode 18): `offset` is the byte displacement from this
// instruction's own address to its target; `link` sets LK (a `bl`).
std::uint32_t branch_word(std::int32_t offset, bool link) {
  return 0x48000000u | (static_cast<std::uint32_t>(offset) & 0x03FFFFFCu) | (link ? 1u : 0u);
}

struct Fixture {
  std::filesystem::path root;
  std::filesystem::path input;
  std::filesystem::path output;
};

Fixture make_fixture(const std::string& name, const std::vector<std::byte>& xex_bytes) {
  Fixture fixture;
  fixture.root = std::filesystem::temp_directory_path() / ("xenon_registry_numeric_format_" + name);
  std::filesystem::remove_all(fixture.root);
  std::filesystem::create_directories(fixture.root);
  fixture.input = fixture.root / "fixture.xex";
  fixture.output = fixture.root / "generated";
  std::ofstream(fixture.input, std::ios::binary)
      .write(reinterpret_cast<const char*>(xex_bytes.data()), static_cast<std::streamsize>(xex_bytes.size()));
  return fixture;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::size_t occurrences(const std::string& haystack, const std::string& needle) {
  std::size_t count = 0, pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string::npos) { ++count; pos += needle.size(); }
  return count;
}

// Builds a synthetic module with a two-level call tree - entry point `bl`s to
// a set of "dispatcher" functions, each of which `bl`s to a set of leaf
// (single-instruction `blr`) functions - so that entry, plus every dispatcher,
// plus every leaf, is auto-discovered and compiled and
// `total_compiled_count` == 1 (entry) + dispatcher_count + leaf_count.
//
// A flat one-level tree (entry directly calling every leaf) was tried first
// and rejected: driver.cpp caps how many instruction words a single
// function's analysis will scan (see analyze_function_candidate()'s
// `max_words = std::min<std::size_t>(..., 4096)`) - an unrelated, pre-existing
// guard against runaway single-function analysis, not part of this fix. A
// flat tree's entry function would itself contain one instruction per callee,
// so for the large (19233) fixture only the first ~4096 calls were ever
// discovered. Splitting fan-out across a middle dispatcher tier keeps every
// individual function's instruction count small (capped at kFanOut+1) no
// matter how large `total_compiled_count` is, while still letting the caller
// pin the exact generated literal (15, 16, 19233, ...) the way the real
// production bug was pinned to a real title's function count.
struct GeneratedProject {
  Fixture fixture;
  AnalysisReport report;
  std::string registry_text;
};

GeneratedProject build_project_with_compiled_count(const std::string& name, std::size_t total_compiled_count) {
  assert(total_compiled_count >= 1);
  constexpr std::size_t kFanOut = 100;  // well under the 4096-word per-function analysis cap
  const std::size_t remaining = total_compiled_count - 1;  // budget for dispatchers + leaves
  // Smallest dispatcher_count such that dispatcher_count * kFanOut covers
  // (remaining - dispatcher_count) leaves, i.e. every dispatcher's own call
  // count stays within kFanOut.
  const std::size_t dispatcher_count = (remaining + kFanOut) / (kFanOut + 1);
  const std::size_t leaf_count = remaining - dispatcher_count;

  // Evenly distribute leaf_count leaves across dispatcher_count dispatchers
  // (each dispatcher's own leaf share never exceeds kFanOut by construction).
  std::vector<std::size_t> leaves_per_dispatcher(dispatcher_count, 0);
  for (std::size_t i = 0; i < leaf_count; ++i) ++leaves_per_dispatcher[i % std::max<std::size_t>(dispatcher_count, 1)];

  // Word layout: [entry: dispatcher_count calls + 1 blr]
  //              [dispatcher 0: leaves_per_dispatcher[0] calls + 1 blr] ...
  //              [leaf 0] [leaf 1] ...
  const std::size_t entry_start = 0;
  const std::size_t entry_size = dispatcher_count + 1;
  std::vector<std::size_t> dispatcher_start(dispatcher_count);
  std::size_t cursor = entry_start + entry_size;
  for (std::size_t i = 0; i < dispatcher_count; ++i) {
    dispatcher_start[i] = cursor;
    cursor += leaves_per_dispatcher[i] + 1;
  }
  const std::size_t leaves_region_start = cursor;
  const std::size_t total_words = leaves_region_start + leaf_count;

  std::vector<std::uint32_t> words(total_words, kBlr);
  // Entry: bl to each dispatcher, then its own blr (already kBlr from init).
  for (std::size_t i = 0; i < dispatcher_count; ++i) {
    const auto source = entry_start + i;
    const auto target = dispatcher_start[i];
    words[source] = branch_word(static_cast<std::int32_t>((target - source) * 4), true);
  }
  // Each dispatcher: bl to its share of leaves, then its own blr.
  std::size_t next_leaf = leaves_region_start;
  for (std::size_t i = 0; i < dispatcher_count; ++i) {
    for (std::size_t m = 0; m < leaves_per_dispatcher[i]; ++m) {
      const auto source = dispatcher_start[i] + m;
      const auto target = next_leaf++;
      words[source] = branch_word(static_cast<std::int32_t>((target - source) * 4), true);
    }
  }
  assert(next_leaf == total_words);

  const auto xex_bytes = make_xex(words);
  auto fixture = make_fixture(name, xex_bytes);

  DriverOptions options{};
  options.input = fixture.input;
  options.output = fixture.output;

  AnalysisReport report;
  std::string error;
  if (!load_and_analyze(options, report, error)) {
    std::cerr << "load_and_analyze failed for " << name << ": " << error << "\n";
    std::abort();
  }
  if (report.functions.size() != total_compiled_count) {
    std::cerr << "expected " << total_compiled_count << " discovered functions for " << name
               << ", got " << report.functions.size() << "\n";
    std::abort();
  }
  if (!generate_project(options, report, error)) {
    std::cerr << "generate_project failed for " << name << ": " << error << "\n";
    std::abort();
  }
  if (report.diagnostics.functions_compiled != total_compiled_count) {
    std::cerr << "expected functions_compiled == " << total_compiled_count << " for " << name
               << ", got " << report.diagnostics.functions_compiled << "\n";
    std::abort();
  }

  GeneratedProject result;
  result.fixture = fixture;
  result.report = std::move(report);
  result.registry_text = read_file(fixture.output / "registry.cpp");
  return result;
}

void assert_correct_decimal_count(const std::string& registry_text, std::size_t count,
                                   const std::string& case_name) {
  const std::string expected_decl = "const std::size_t kCompiledFunctionCount = " + std::to_string(count) + ";";
  assert(registry_text.find(expected_decl) != std::string::npos &&
         "kCompiledFunctionCount must be emitted as the exact decimal count, not a leaked-hex rendering");
  assert(occurrences(registry_text, "kCompiledFunctionCount = ") == 1);

  const std::string expected_array = "kCompiledFunctions[" + std::to_string(count) + "]";
  assert(registry_text.find(expected_array) != std::string::npos &&
         "kCompiledFunctions[] array size must also be the exact decimal count");

  // No stray, syntactically-invalid literal suffix (C3688) - the direct
  // symptom the real AC6 build hit ("4b21").
  std::ostringstream hex_of_count;
  hex_of_count << std::hex << count;
  const std::string hex_form = hex_of_count.str();
  if (hex_form != std::to_string(count)) {
    const std::string leaked = "kCompiledFunctionCount = " + hex_form + ";";
    if (registry_text.find(leaked) != std::string::npos) {
      std::cerr << "case " << case_name << ": kCompiledFunctionCount leaked hex form \"" << hex_form
                << "\"\n";
      std::abort();
    }
  }
}

}  // namespace

int main() {
  std::cout << "Testing generated registry.cpp numeric-format state leak fix...\n";

  // Test 1: count = 15. A plain, unremarkable decimal count - establishes
  // the baseline (no hex digits happen to look like anything decimal).
  {
    auto project = build_project_with_compiled_count("count_15", 15);
    assert_correct_decimal_count(project.registry_text, 15, "count=15");
    std::filesystem::remove_all(project.fixture.root);
  }
  std::cout << "  [PASS] kCompiledFunctionCount = 15 (baseline)\n";

  // Test 2 (CRITICAL): count = 16 = 0x10. This is the case that makes the
  // bug dangerous rather than merely a build break: a leaked std::hex
  // formatter renders 16 as "10", which is ALSO a syntactically valid
  // decimal integer literal - so the generated C++ would silently compile
  // to a WRONG compiled-function count (10 instead of 16) rather than
  // failing loudly like the AC6 "4b21" case did.
  {
    auto project = build_project_with_compiled_count("count_16", 16);
    assert_correct_decimal_count(project.registry_text, 16, "count=16");
    assert(project.registry_text.find("kCompiledFunctionCount = 10;") == std::string::npos &&
           "16 must never be silently misrendered as its hex form \"10\"");
    std::filesystem::remove_all(project.fixture.root);
  }
  std::cout << "  [PASS] kCompiledFunctionCount = 16, never silently misrendered as \"10\"\n";

  // Test 3: count = 19233 = 0x4B21, the exact real-world Ace Combat 6
  // function count that produced the original MSVC C3688/C2737 failure
  // (`const std::size_t kCompiledFunctionCount = 4b21;`).
  {
    auto project = build_project_with_compiled_count("count_19233", 19233);
    assert_correct_decimal_count(project.registry_text, 19233, "count=19233");
    assert(project.registry_text.find("4b21") == std::string::npos);
    assert(project.registry_text.find("4B21") == std::string::npos);
    std::filesystem::remove_all(project.fixture.root);
  }
  std::cout << "  [PASS] kCompiledFunctionCount = 19233 (the exact real Ace Combat 6 reproduction), "
               "never \"4b21\"\n";

  // Test 4: hexadecimal guest addresses immediately preceding the count in
  // the emitted table must remain hexadecimal - proves the fix isolates
  // address formatting rather than just turning it off everywhere.
  {
    auto project = build_project_with_compiled_count("addresses_then_count", 16);
    const auto& text = project.registry_text;
    const auto table_pos = text.find("kCompiledFunctions[16] = {");
    const auto count_pos = text.find("kCompiledFunctionCount = 16;");
    assert(table_pos != std::string::npos && count_pos != std::string::npos && table_pos < count_pos);
    // At least one "{0x..." guest-address entry must appear between the
    // table open and the count declaration, and it must actually contain
    // hex digits (not have been flattened to decimal by an overzealous fix).
    const auto entry_pos = text.find("  {0x", table_pos);
    assert(entry_pos != std::string::npos && entry_pos < count_pos &&
           "guest addresses must still be emitted as hexadecimal literals");
    std::filesystem::remove_all(project.fixture.root);
  }
  std::cout << "  [PASS] Hexadecimal guest addresses immediately before the count remain hexadecimal, "
               "and do not affect the following decimal count\n";

  // Test 5: the small (count=15) fixture is compiled for real via a nested
  // CMake configure+build against xenon_game, the way
  // tests/recomp/recomp_driver_tests.cpp already proves a generated project
  // builds - so this suite doesn't just check the *text* of registry.cpp,
  // it proves MSVC/the active toolchain actually accepts it as valid C++.
  // Only run for the small fixture: the 19233-function fixture's entry
  // function alone would emit tens of thousands of call-instruction lines
  // in one function body, which is unnecessary compile time to pay here -
  // the fix is magnitude-independent (it removes std::hex from
  // registry_text entirely), so the small fixture already proves the
  // generated C++ is syntactically and semantically valid.
#ifdef XENON_SOURCE_ROOT
  {
    auto project = build_project_with_compiled_count("compile_check", 15);
    const auto build = project.fixture.root / "build";
    const auto quote = [](const std::filesystem::path& value) {
      return std::string("\"") + value.string() + "\"";
    };
    const auto configure = "cmake -S " + quote(project.fixture.output) + " -B " + quote(build) +
                           " -DXENON_RECOMP_ROOT=" + quote(XENON_SOURCE_ROOT);
    assert(std::system(configure.c_str()) == 0);
    const auto compile = "cmake --build " + quote(build) + " --target xenon_game";
    assert(std::system(compile.c_str()) == 0);
    std::filesystem::remove_all(project.fixture.root);
  }
  std::cout << "  [PASS] Generated registry.cpp for kCompiledFunctionCount = 15 actually compiles\n";
#else
  std::cout << "  [SKIP] Compile check (XENON_SOURCE_ROOT not defined for this build)\n";
#endif

  std::cout << "All registry.cpp numeric-format tests passed!\n";
  return 0;
}

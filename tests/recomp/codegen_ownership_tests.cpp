// Regression tests for the generated-code deduplication / shard ownership
// fix. These exercise the actual production bug that made a real title's
// (Ace Combat 6) generated module fail to compile with MSVC C2084 "function
// already has a body" errors: generate_project()'s per-function codegen
// cache was keyed ONLY by a content hash of guest instruction bytes, with no
// dependency on which guest address (and therefore which generated C++
// name) those bytes belonged to. Two different, unrelated functions with
// byte-identical machine code - extremely common for trivial stub bodies
// such as a bare `blr` across a real title's tens of thousands of functions
// - collided on that hash: whichever function reached the shared cache path
// first "won", and every other colliding function silently inherited the
// first function's own emitted text (defining the first function's symbols
// a second time under a different shard slot) instead of its own, while its
// own symbols were never emitted at all despite registry.cpp still
// declaring and referencing them.
//
// Uses the same small hand-built synthetic XEX technique as
// tests/recomp/recomp_driver_tests.cpp and
// tests/recomp/analysis_v2_consumption_tests.cpp.

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
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

std::vector<std::byte> make_xex(const std::vector<std::uint32_t>& text_words, std::size_t data_size) {
  constexpr std::size_t header = 0x300;
  constexpr std::size_t security = 0x20;
  constexpr std::size_t pe = 0x380;
  constexpr std::size_t coff = pe + 4;
  constexpr std::size_t optional = coff + 20;
  constexpr std::size_t section = optional + 0xE0;
  constexpr std::size_t text_raw = kTextRva;
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

constexpr std::uint32_t kBlr = 0x4E800020u;

// I-form branch (opcode 18): `offset` is the byte displacement from this
// instruction's own address to its target; `link` sets LK (a `bl`).
std::uint32_t branch_word(std::int32_t offset, bool link) {
  return 0x48000000u | (static_cast<std::uint32_t>(offset) & 0x03FFFFFCu) | (link ? 1u : 0u);
}

std::string hex_of(std::uint32_t address) {
  std::ostringstream out;
  out << std::hex << address;
  return out.str();
}

std::size_t occurrences(const std::string& haystack, const std::string& needle) {
  std::size_t count = 0, pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string::npos) { ++count; pos += needle.size(); }
  return count;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), {});
}

std::string read_all_shards(const std::filesystem::path& output) {
  std::string text;
  for (const auto& entry : std::filesystem::directory_iterator(output / "functions")) {
    if (entry.path().extension() != ".cpp") continue;
    std::ifstream source(entry.path());
    text.append(std::istreambuf_iterator<char>(source), {});
  }
  return text;
}

struct Fixture {
  std::filesystem::path root;
  std::filesystem::path input;
  std::filesystem::path output;
};

Fixture make_fixture(const std::string& name, const std::vector<std::byte>& xex_bytes) {
  Fixture fixture;
  fixture.root = std::filesystem::temp_directory_path() / ("xenon_codegen_ownership_" + name);
  std::filesystem::remove_all(fixture.root);
  std::filesystem::create_directories(fixture.root);
  fixture.input = fixture.root / "fixture.xex";
  fixture.output = fixture.root / "generated";
  std::ofstream(fixture.input, std::ios::binary)
      .write(reinterpret_cast<const char*>(xex_bytes.data()), static_cast<std::streamsize>(xex_bytes.size()));
  return fixture;
}

}  // namespace

int main() {
  std::cout << "Testing generated-code deduplication / shard ownership fix...\n";

  // Test 1 (the actual production bug): several functions with
  // byte-identical single-instruction bodies (`blr`), reached only via
  // direct calls from the entry point so every one is auto-discovered and
  // auto-named (xenon_fn_<address>) rather than hint-named - matching how
  // the real AC6 duplicates included both hinted (rex_sub_*) and
  // auto-named (xenon_fn_*) symbols. Before the cache-key fix, only ONE of
  // these functions' actual text would ever be emitted (duplicated across
  // shard slots under its own name); every other one silently vanished
  // while registry.cpp kept declaring/referencing its symbol. After the
  // fix, every function's own name must appear in the generated shards
  // exactly once.
  {
    constexpr int kFunctionCount = 6;
    std::vector<std::uint32_t> words;
    // entry: bl to each function in turn, then blr.
    for (int i = 0; i < kFunctionCount; ++i) words.push_back(0u);  // patched below
    words.push_back(kBlr);  // entry's own terminal, at word index kFunctionCount
    for (int i = 0; i < kFunctionCount; ++i) words.push_back(kBlr);  // the identical-body functions

    for (int i = 0; i < kFunctionCount; ++i) {
      const std::int32_t call_site_offset = static_cast<std::int32_t>(i) * 4;
      const std::int32_t target_offset =
          static_cast<std::int32_t>((kFunctionCount + 1 + i) * 4);
      words[static_cast<std::size_t>(i)] = branch_word(target_offset - call_site_offset, true);
    }

    const auto xex_bytes = make_xex(words, 0x40);
    const auto fixture = make_fixture("cache_collision", xex_bytes);

    DriverOptions options{};
    options.input = fixture.input;
    options.output = fixture.output;
    AnalysisReport report;
    std::string error;
    assert(load_and_analyze(options, report, error) && error.empty());
    assert(report.functions.size() == static_cast<std::size_t>(kFunctionCount) + 1u);
    assert(report.diagnostics.codegen_duplicate_addresses_merged == 0u);

    assert(generate_project(options, report, error) && error.empty());
    assert(report.diagnostics.codegen_duplicate_symbols_rejected == 0u);
    assert(report.diagnostics.codegen_unique_functions == report.diagnostics.codegen_input_functions);

    const auto generated_source = read_all_shards(fixture.output);
    const auto base = kLoadAddress + kTextRva;
    for (int i = 0; i < kFunctionCount; ++i) {
      const auto address = base + static_cast<std::uint32_t>((kFunctionCount + 1 + i) * 4);
      const std::string base_symbol = "xenon_fn_" + hex_of(address);
      // Uppercase hex, matching cpp_name()'s std::uppercase.
      std::string upper = base_symbol;
      std::transform(upper.begin() + 9, upper.end(), upper.begin() + 9, ::toupper);
      const auto count_v2 = occurrences(generated_source, "ExecutionResult " + upper + "_v2(");
      const auto count_base =
          occurrences(generated_source, "ExecutionResult " + upper + "([[maybe_unused]] CpuState");
      assert(count_v2 == 1 &&
             "each byte-identical function must emit its OWN _v2 symbol exactly once, never zero "
             "(silently dropped in favor of a colliding function's cached text) or more than once "
             "(duplicate C2084 body)");
      assert(count_base == 1);
    }
    std::filesystem::remove_all(fixture.root);
  }
  std::cout << "  [PASS] Byte-identical functions at different addresses each emit their own symbols "
               "exactly once (the actual AC6 C2084 root cause)\n";

  // Test 2: jobs=1 vs jobs=N codegen must produce byte-identical shard and
  // registry output for the same (already-analyzed) function set - no
  // duplication or divergence introduced by parallel scheduling.
  {
    constexpr int kFunctionCount = 8;
    std::vector<std::uint32_t> words;
    for (int i = 0; i < kFunctionCount; ++i) words.push_back(0u);
    words.push_back(kBlr);
    for (int i = 0; i < kFunctionCount; ++i) words.push_back(kBlr);
    for (int i = 0; i < kFunctionCount; ++i) {
      const std::int32_t call_site_offset = static_cast<std::int32_t>(i) * 4;
      const std::int32_t target_offset = static_cast<std::int32_t>((kFunctionCount + 1 + i) * 4);
      words[static_cast<std::size_t>(i)] = branch_word(target_offset - call_site_offset, true);
    }
    const auto xex_bytes = make_xex(words, 0x40);
    const auto fixture = make_fixture("parallel_determinism", xex_bytes);

    DriverOptions base_options{};
    base_options.input = fixture.input;
    base_options.output = fixture.output / "serial";
    base_options.codegen_jobs = 1;
    AnalysisReport report;
    std::string error;
    assert(load_and_analyze(base_options, report, error) && error.empty());
    assert(generate_project(base_options, report, error) && error.empty());
    const auto serial_shards = read_all_shards(base_options.output);
    const auto serial_shard_count = report.diagnostics.codegen_shards;

    DriverOptions parallel_options = base_options;
    parallel_options.output = fixture.output / "parallel";
    parallel_options.codegen_jobs = 8;
    AnalysisReport report2 = report;  // reuse the same analyzed function set
    assert(generate_project(parallel_options, report2, error) && error.empty());
    const auto parallel_shards = read_all_shards(parallel_options.output);

    assert(serial_shards == parallel_shards &&
           "codegen_jobs=1 and codegen_jobs=8 must produce byte-identical generated shard text");
    assert(report2.diagnostics.codegen_shards == serial_shard_count);
    assert(report2.diagnostics.codegen_duplicate_symbols_rejected == 0u);
    std::filesystem::remove_all(fixture.root);
  }
  std::cout << "  [PASS] codegen_jobs=1 and codegen_jobs=8 produce identical, non-duplicated shard output\n";

  // Test 3: two DIFFERENT guest addresses whose hint data assigns the exact
  // same generated name must fail codegen loudly (Part 3/13 - never
  // silently rename/suppress/merge unrelated functions), not emit two C++
  // definitions under one symbol.
  {
    const auto xex_bytes = make_xex({kBlr, kBlr}, 0x40);
    xbox::XexImage image{};
    std::string parse_error;
    assert(xbox::parse_xex_image(xex_bytes, image, &parse_error));
    const auto base = kLoadAddress + kTextRva;

    AnalysisHintSetV2 hints{};
    hints.identity = xbox::compute_effective_identity(image);
    FunctionHint fn_a{};
    fn_a.address = base;
    fn_a.end = base + 4u;
    fn_a.name = "colliding_name";
    hints.functions.push_back(fn_a);
    FunctionHint fn_b{};
    fn_b.address = base + 4u;
    fn_b.end = base + 8u;
    fn_b.name = "colliding_name";
    hints.functions.push_back(fn_b);

    const auto fixture = make_fixture("symbol_collision", xex_bytes);
    DriverOptions options{};
    options.hint_set_v2 = hints;
    options.input = fixture.input;
    options.output = fixture.output;

    AnalysisReport report;
    std::string error;
    assert(load_and_analyze(options, report, error) && error.empty());
    assert(report.functions.size() == 2 &&
           "two distinct addresses must still both be discovered/compiled - the collision is purely "
           "in their generated NAME, not their identity");

    const bool ok = generate_project(options, report, error);
    assert(!ok && "generate_project() must refuse to emit two functions under one colliding symbol name");
    assert(!error.empty());
    assert(report.diagnostics.codegen_duplicate_symbols_rejected >= 1u);
    std::filesystem::remove_all(fixture.root);
  }
  std::cout << "  [PASS] Two different addresses assigned the same generated name fail codegen early, "
               "with no C++ emitted\n";

  // Test 4a (Part 10 item 2 - "SAME ADDRESS, DIFFERENT NAMES"): Analysis
  // Hint Schema V2 already refuses two FunctionHint entries at the same
  // address outright (analysis_schema.cpp's validate_hint_set(), "duplicate
  // function hint at 0x...") rather than silently picking one - a stronger,
  // earlier guarantee than a runtime merge would be. This locks that
  // behavior in as part of the alias story this fix completes.
  {
    const auto xex_bytes = make_xex({kBlr}, 0x40);
    xbox::XexImage image{};
    std::string parse_error;
    assert(xbox::parse_xex_image(xex_bytes, image, &parse_error));
    const auto base = kLoadAddress + kTextRva;

    AnalysisHintSetV2 hints{};
    hints.identity = xbox::compute_effective_identity(image);
    FunctionHint alias_a{};
    alias_a.address = base;
    alias_a.end = base + 4u;
    alias_a.name = "alias_one";
    hints.functions.push_back(alias_a);
    FunctionHint alias_b{};
    alias_b.address = base;
    alias_b.end = base + 4u;
    alias_b.name = "alias_two";
    hints.functions.push_back(alias_b);

    const auto fixture = make_fixture("address_aliases_v2_rejected", xex_bytes);
    DriverOptions options{};
    options.hint_set_v2 = hints;
    options.input = fixture.input;
    options.output = fixture.output;

    AnalysisReport report;
    std::string error;
    const bool ok = load_and_analyze(options, report, error);
    assert(!ok && !error.empty() &&
           "Analysis Hint Schema V2 must reject two FunctionHint aliases at the same address as "
           "ambiguous hint data, not silently pick one");
    std::filesystem::remove_all(fixture.root);
  }
  std::cout << "  [PASS] Two Schema V2 FunctionHint aliases at the same address are rejected as "
               "ambiguous hint data\n";

  // Test 4b: the legacy V1 ModuleHint::known_symbols path has no such
  // schema-level duplicate-address guard (it is a simple address->name
  // map), so it is the one place two aliases at the same address can
  // actually reach the driver - and it must still resolve to exactly one
  // canonical function/body, never two.
  {
    const auto xex_bytes = make_xex({kBlr}, 0x40);
    const auto base = kLoadAddress + kTextRva;
    const auto fixture = make_fixture("address_aliases_v1", xex_bytes);

    ModuleHint hint{};
    hint.name = "aliases";
    hint.known_symbols.push_back({base, "alias_one"});
    hint.known_symbols.push_back({base, "alias_two"});

    DriverOptions options{};
    options.hints.push_back(hint);
    options.input = fixture.input;
    options.output = fixture.output;

    AnalysisReport report;
    std::string error;
    assert(load_and_analyze(options, report, error) && error.empty());
    std::size_t count_at_address = 0;
    for (const auto& function : report.functions)
      if (function.guest_start == base) ++count_at_address;
    assert(count_at_address == 1 && "two legacy known_symbols aliases at the same address must produce "
                                    "exactly one canonical function record, not two");
    assert(generate_project(options, report, error) && error.empty());
    assert(report.diagnostics.codegen_duplicate_symbols_rejected == 0u);
    std::filesystem::remove_all(fixture.root);
  }
  std::cout << "  [PASS] Two legacy known_symbols aliases at the same guest address compile as exactly "
               "one canonical function\n";

  // Test 5: executable direct control flow must not reach codegen without a
  // local block or a separately compiled entry. This simulates a malformed
  // analysis result at the exact boundary where the runtime would otherwise
  // emit an unmaterialized branch.
  {
    const auto xex_bytes = make_xex({kBlr}, 0x40);
    const auto fixture = make_fixture("unmaterialized_control_flow", xex_bytes);
    DriverOptions options{};
    options.input = fixture.input;
    options.output = fixture.output;

    AnalysisReport report;
    std::string error;
    assert(load_and_analyze(options, report, error) && error.empty());
    assert(!report.functions.empty());
    report.functions.front().ir.blocks.front().successors.push_back(
        {kLoadAddress + kTextRva + 0x100u, xenon::cpu::ir::EdgeKind::Branch, false});
    assert(!generate_project(options, report, error));
    assert(error.find("without a materialized local CFG block or dispatchable compiled entry") !=
           std::string::npos);
    std::filesystem::remove_all(fixture.root);
  }
  std::cout << "  [PASS] Unmaterialized executable control flow fails before code generation\n";

  // Test 6: failed regeneration must never make stale analysis.json look like
  // current output. Root project files are staged and generation-status.json
  // explicitly marks the failed attempt incomplete while preserving the last
  // successfully promoted analysis.
  {
    const auto xex_bytes = make_xex({kBlr}, 0x40);
    const auto fixture = make_fixture("transactional_generation_status", xex_bytes);
    DriverOptions options{};
    options.input = fixture.input;
    options.output = fixture.output;

    AnalysisReport report;
    std::string error;
    assert(load_and_analyze(options, report, error) && error.empty());
    assert(generate_project(options, report, error) && error.empty());
    const auto analysis_before = read_file(fixture.output / "analysis.json");
    const auto status_before = read_file(fixture.output / "generation-status.json");
    assert(status_before.find("\"complete\": true") != std::string::npos);

    report.functions.front().ir.blocks.front().successors.push_back(
        {kLoadAddress + kTextRva + 0x100u, xenon::cpu::ir::EdgeKind::Branch, false});
    error.clear();
    assert(!generate_project(options, report, error));
    const auto analysis_after = read_file(fixture.output / "analysis.json");
    const auto status_after = read_file(fixture.output / "generation-status.json");
    assert(analysis_after == analysis_before &&
           "a failed generation must preserve the last successfully promoted analysis.json");
    assert(status_after.find("\"complete\": false") != std::string::npos);
    assert(status_after.find("\"phase\": \"control-flow\"") != std::string::npos);
    std::filesystem::remove_all(fixture.root);
  }
  std::cout << "  [PASS] Failed regeneration is transactional and explicitly marked incomplete\n";

  std::cout << "All generated-code deduplication / shard ownership tests passed!\n";
  return 0;
}

// Regression tests for the import-thunk misclassification fix: a `bl` to an
// XEX-native import record's callable guest_thunk address used to be queued
// for normal PPC decoding like any other DirectCall target, even though the
// bytes there are loader-owned placeholder metadata (ordinal/attributes/
// record-type), never guest PPC. Real-world repro: Ace Combat 6's
// default.xex has a callable xboxkrnl.exe import thunk at guest address
// 0x823D014C whose 16 bytes are exactly:
//
//   0x010100BD  (record_type=1 FunctionThunk placeholder, ordinal=0xBD)
//   0x020100BD  (a second, never-independently-addressed placeholder word
//                for the same ordinal - record_type=2)
//   0x7D6903A6  (mtctr r11 - already-implemented, real PPC)
//   0x4E800420  (bctr - already-implemented, real PPC)
//
// decoding word 0/1 as PPC produced a false "unsupported"/"invalid PPC
// encoding" diagnostic at analysis time, even though
// XenonSession::call() (src/core/session/execution/runtime_services.cpp) already resolves calls to
// that exact address correctly at runtime via XexImage::imports[].
// guest_thunk matching. The fix (analyze_function_candidate()'s
// find_callable_import_thunk() short-circuit in driver.cpp) recognizes the
// address before any decode is attempted and deliberately leaves it
// uncompiled, so the existing runtime dispatch remains unshadowed.
//
// Second failure mode, found by actually running the real AC6 default.xex
// through Project-Gracemeria's build after the above fix: a PLAIN
// (non-linked) branch into the thunk - a tail-call the compiler emitted
// instead of bl+blr - made generate_project()'s validate_region_entry_
// integrity() fail outright ("control-flow edge 0x821f4118 -> 0x823d014c
// leaves owner 0x821f4110 without a dispatchable guest entry"), because
// import-thunk addresses were never published into report.entries. Fixed by
// giving DiscoveredFunction::import_thunk entries their own
// GuestEntryKind::ImportThunk entry (same "no compiled owner needed"
// treatment as GuestEntryKind::RuntimeHelper already gets).
//
// Third failure mode, found immediately after fixing the second: a
// DIFFERENT real AC6 import thunk (xboxkrnl.exe ordinal 197, guest address
// 0x823d00cc) is reached ONLY by a lone non-linked branch (0x821f4120, with
// no `bl` anywhere providing corroborating evidence) - and
// analyze_function_candidate()'s promote_terminal_target()/
// has_independent_discovery_evidence() require independent evidence before
// ever promoting a plain-branch target to a real discovery candidate at
// all, so this thunk was never even analyzed (worse, queue_local_target()'s
// inferred-local-range materializer could try to inline its placeholder
// bytes as local code inside the branching function's own body). Fixed by
// teaching has_independent_discovery_evidence() that a callable import
// thunk (find_callable_import_thunk()) is always independently, structurally
// confirmed - never a guess - regardless of how it's reached.
//
// Uses the same small hand-built synthetic XEX technique as
// tests/recomp/recomp_driver_tests.cpp and
// tests/recomp/registry_numeric_format_tests.cpp, extended with a native
// XEX_HEADER_IMPORT_LIBRARIES optional header entry (key 0x000103FF) so the
// fixture exercises the real xex_loader.cpp import-table parser, not a
// hand-built XexImage.

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
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

void be16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset] = static_cast<std::byte>(value >> 8);
  bytes[offset + 1] = static_cast<std::byte>(value);
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

void write_string(std::vector<std::byte>& bytes, std::size_t offset, const std::string& value) {
  for (std::size_t i = 0; i < value.size(); ++i) bytes[offset + i] = static_cast<std::byte>(value[i]);
  bytes[offset + value.size()] = std::byte{0};
}

constexpr std::uint32_t kLoadAddress = 0x80000000u;
constexpr std::uint32_t kTextRva = 0x10000u;

// The exact real Ace Combat 6 byte pattern at guest address 0x823D014C,
// confirmed against the real, decrypted/decompressed default.xex.
constexpr std::uint32_t kThunkPlaceholderWord0 = 0x01000001u;  // record_type=1, attr=0, ordinal=1
constexpr std::uint32_t kThunkPlaceholderWord1 = 0x02000001u;  // record_type=2, same ordinal
constexpr std::uint32_t kThunkMtctrR11 = 0x7D6903A6u;
constexpr std::uint32_t kThunkBctr = 0x4E800420u;
// Thunk #2's placeholder words - same shape, ordinal 2 (matching xboxkrnl.exe
// ordinal 197's real thunk at 0x823d00cc, reached only by a lone non-linked
// branch in the real AC6 XEX).
constexpr std::uint32_t kThunk2PlaceholderWord0 = 0x01000002u;
constexpr std::uint32_t kThunk2PlaceholderWord1 = 0x02000002u;
constexpr std::uint32_t kBlr = 0x4E800020u;

std::uint32_t branch_word(std::int32_t offset, bool link) {
  return 0x48000000u | (static_cast<std::uint32_t>(offset) & 0x03FFFFFCu) | (link ? 1u : 0u);
}

// Word layout in .text. Note function bodies are scanned as CONTIGUOUS
// instruction streams from their start until a terminal instruction - a
// function can never "skip over" another region's bytes - so entry's own
// terminal blr must sit before either thunk's placeholder words physically,
// and tail_caller/lone_tail_caller (whose own branch instructions are
// themselves terminal) are placed after them so no function's linear scan
// ever tries to decode a thunk's non-PPC placeholder bytes as its own
// instructions:
//   [0] bl -> thunk (word index 4)        (entry: a direct bl into thunk #1,
//                                           exactly like recomp_driver_tests.cpp's
//                                           original repro - guarantees thunk #1
//                                           gets DirectCall discovery evidence
//                                           independent of tail_caller below.
//                                           The real AC6 XEX has 4 real `bl`
//                                           callers of 0x823d014c providing
//                                           exactly this evidence.)
//   [1] bl -> tail_caller (word index 8)
//   [2] bl -> lone_tail_caller (word index 9)
//   [3] blr                               (entry function: 4 words: [0]-[3])
//   [4] thunk #1 placeholder word 0       -> guest_thunk address (record_address)
//   [5] thunk #1 placeholder word 1
//   [6] mtctr r11
//   [7] bctr
//   [8] b -> thunk #1 (word index 4), NOT linked  (tail_caller: single-
//                                                    instruction plain-branch
//                                                    tail call into thunk #1 -
//                                                    the second failure mode:
//                                                    generation-status.json
//                                                    reported "control-flow
//                                                    edge 0x821f4118 ->
//                                                    0x823d014c leaves owner
//                                                    0x821f4110 without a
//                                                    dispatchable guest
//                                                    entry" because import-
//                                                    thunk addresses were
//                                                    never published into
//                                                    report.entries)
//   [9] b -> thunk #2 (word index 10), NOT linked  (lone_tail_caller: reaches
//                                                     thunk #2 by a NON-LINKED
//                                                     branch ONLY - no `bl`
//                                                     anywhere targets thunk
//                                                     #2 - the third failure
//                                                     mode: generation-
//                                                     status.json next
//                                                     reported "control-flow
//                                                     edge 0x821f4120 ->
//                                                     0x823d00cc leaves owner
//                                                     0x821f4120 without a
//                                                     dispatchable guest
//                                                     entry" for xboxkrnl.exe
//                                                     ordinal 197's real
//                                                     thunk, which was never
//                                                     even analyzed because
//                                                     has_independent_
//                                                     discovery_evidence()
//                                                     didn't know import
//                                                     thunks are always
//                                                     independently confirmed)
//   [10] thunk #2 placeholder word 0 (ordinal 2) -> guest_thunk address
//   [11] thunk #2 placeholder word 1
//   [12] mtctr r11
//   [13] bctr
constexpr std::size_t kThunkWordIndex = 4;
constexpr std::size_t kTailCallerWordIndex = 8;
constexpr std::size_t kLoneTailCallerWordIndex = 9;
constexpr std::size_t kThunk2WordIndex = 10;
constexpr std::size_t kTotalWords = 14;

std::vector<std::byte> make_xex() {
  constexpr std::size_t header = 0x300;
  constexpr std::size_t security = 0x20;
  constexpr std::size_t import_table_offset = 0x1A4;  // after security_info (0x20 + 0x184 = 0x1A4)
  constexpr std::size_t pe = 0x380;
  constexpr std::size_t coff = pe + 4;
  constexpr std::size_t optional = coff + 20;
  constexpr std::size_t section = optional + 0xE0;
  constexpr std::size_t text_raw = 0x600;
  const std::size_t text_bytes = kTotalWords * 4u;

  const std::size_t file_size = header + kTextRva + text_bytes + 0x100;
  std::vector<std::byte> bytes(file_size, std::byte{0});

  bytes[0] = std::byte{'X'}; bytes[1] = std::byte{'E'};
  bytes[2] = std::byte{'X'}; bytes[3] = std::byte{'2'};
  be32(bytes, 8, static_cast<std::uint32_t>(header));
  be32(bytes, 0x10, static_cast<std::uint32_t>(security));
  be32(bytes, 0x14, 1);  // optional_header_count = 1

  // One XEX optional header entry: XEX_HEADER_IMPORT_LIBRARIES (0x000103FF),
  // pointing at import_table_offset (an absolute file/header byte offset,
  // matching parse_native_import_libraries()'s own coordinate space).
  be32(bytes, 0x18, 0x000103FFu);
  be32(bytes, 0x1C, static_cast<std::uint32_t>(import_table_offset));

  be32(bytes, security + 0x000, 0x184);
  be32(bytes, security + 0x004, static_cast<std::uint32_t>(bytes.size() - header));
  be32(bytes, security + 0x110, kLoadAddress);

  // Native import-libraries table: one library ("xboxkrnl.exe"), two
  // callable (type-1) import records whose guest_thunk addresses are the two
  // thunks' word addresses.
  const std::string module_name = "xboxkrnl.exe";  // 12 chars + NUL = 13, rounds to 16
  constexpr std::size_t string_table_size = 0x10;
  constexpr std::size_t string_table_offset = import_table_offset + 0xC;
  constexpr std::size_t library_offset = string_table_offset + string_table_size;
  constexpr std::size_t library_size = 0x28 + 2 * 4;  // header + 2 import entries
  constexpr std::size_t table_size = (library_offset + library_size) - import_table_offset;

  be32(bytes, import_table_offset + 0x0, static_cast<std::uint32_t>(table_size));
  be32(bytes, import_table_offset + 0x4, static_cast<std::uint32_t>(string_table_size));
  be32(bytes, import_table_offset + 0x8, 1);  // string_table_count
  write_string(bytes, string_table_offset, module_name);

  const std::uint32_t thunk_address =
      kLoadAddress + kTextRva + static_cast<std::uint32_t>(kThunkWordIndex * 4u);
  const std::uint32_t thunk2_address =
      kLoadAddress + kTextRva + static_cast<std::uint32_t>(kThunk2WordIndex * 4u);
  be32(bytes, library_offset + 0x0, static_cast<std::uint32_t>(library_size));
  be16(bytes, library_offset + 0x24, 0);  // name_index
  be16(bytes, library_offset + 0x26, 2);  // import_count
  be32(bytes, library_offset + 0x28, thunk_address);   // import_table[0] = record_address (ordinal 1)
  be32(bytes, library_offset + 0x2C, thunk2_address);  // import_table[1] = record_address (ordinal 2)

  bytes[header] = std::byte{'M'}; bytes[header + 1] = std::byte{'Z'};
  le32(bytes, header + 0x3C, static_cast<std::uint32_t>(pe - header));
  bytes[pe] = std::byte{'P'}; bytes[pe + 1] = std::byte{'E'};
  le16(bytes, coff, 0x14C);
  le16(bytes, coff + 2, 1);
  le16(bytes, coff + 0x10, 0xE0);
  le16(bytes, coff + 0x12, 0x0102);
  le16(bytes, optional, 0x10B);
  le32(bytes, optional + 0x10, kTextRva);
  le32(bytes, optional + 0x1C, kLoadAddress);
  le32(bytes, optional + 0x38, 0x2000);

  bytes[section + 0] = std::byte{'.'}; bytes[section + 1] = std::byte{'t'};
  bytes[section + 2] = std::byte{'e'}; bytes[section + 3] = std::byte{'x'};
  bytes[section + 4] = std::byte{'t'};
  const auto text_size = static_cast<std::uint32_t>(text_bytes + 0x40u);
  le32(bytes, section + 4, text_size);
  le32(bytes, section + 0xC, kTextRva);
  le32(bytes, section + 0x10, text_size);
  le32(bytes, section + 0x14, static_cast<std::uint32_t>(text_raw));
  le32(bytes, section + 0x24, 0x60000020);

  // Byte displacement (as a std::int32_t) from instruction word `from` to
  // word `to`, for branch_word()'s `offset` parameter.
  const auto rel = [](std::size_t from, std::size_t to) {
    return static_cast<std::int32_t>((static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from)) * 4);
  };

  const std::size_t text_file_base = header + kTextRva;
  be32(bytes, text_file_base + 0 * 4, branch_word(rel(0, kThunkWordIndex), true));
  be32(bytes, text_file_base + 1 * 4, branch_word(rel(1, kTailCallerWordIndex), true));
  be32(bytes, text_file_base + 2 * 4, branch_word(rel(2, kLoneTailCallerWordIndex), true));
  be32(bytes, text_file_base + 3 * 4, kBlr);
  be32(bytes, text_file_base + kThunkWordIndex * 4, kThunkPlaceholderWord0);
  be32(bytes, text_file_base + (kThunkWordIndex + 1) * 4, kThunkPlaceholderWord1);
  be32(bytes, text_file_base + (kThunkWordIndex + 2) * 4, kThunkMtctrR11);
  be32(bytes, text_file_base + (kThunkWordIndex + 3) * 4, kThunkBctr);
  be32(bytes, text_file_base + kTailCallerWordIndex * 4,
       branch_word(rel(kTailCallerWordIndex, kThunkWordIndex), false));
  be32(bytes, text_file_base + kLoneTailCallerWordIndex * 4,
       branch_word(rel(kLoneTailCallerWordIndex, kThunk2WordIndex), false));
  be32(bytes, text_file_base + kThunk2WordIndex * 4, kThunk2PlaceholderWord0);
  be32(bytes, text_file_base + (kThunk2WordIndex + 1) * 4, kThunk2PlaceholderWord1);
  be32(bytes, text_file_base + (kThunk2WordIndex + 2) * 4, kThunkMtctrR11);
  be32(bytes, text_file_base + (kThunk2WordIndex + 3) * 4, kThunkBctr);

  return bytes;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
  std::cout << "Testing import-thunk static-analysis misclassification fix...\n";

  const auto root = std::filesystem::temp_directory_path() / "xenon_import_thunk_analysis_test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto input = root / "fixture.xex";
  const auto output = root / "generated";
  {
    const auto xex_bytes = make_xex();
    std::ofstream(input, std::ios::binary)
        .write(reinterpret_cast<const char*>(xex_bytes.data()), static_cast<std::streamsize>(xex_bytes.size()));
  }

  const std::uint32_t thunk_address =
      kLoadAddress + kTextRva + static_cast<std::uint32_t>(kThunkWordIndex * 4u);
  const std::uint32_t thunk2_address =
      kLoadAddress + kTextRva + static_cast<std::uint32_t>(kThunk2WordIndex * 4u);
  const std::uint32_t entry_address = kLoadAddress + kTextRva;
  const std::uint32_t tail_caller_address =
      kLoadAddress + kTextRva + static_cast<std::uint32_t>(kTailCallerWordIndex * 4u);
  const std::uint32_t lone_tail_caller_address =
      kLoadAddress + kTextRva + static_cast<std::uint32_t>(kLoneTailCallerWordIndex * 4u);

  DriverOptions options{};
  options.input = input;
  options.output = output;

  AnalysisReport report;
  std::string error;
  if (!load_and_analyze(options, report, error)) {
    std::cerr << "load_and_analyze failed: " << error << "\n";
    return 1;
  }

  // Both imports were actually parsed by the real xex_loader.cpp parser.
  assert(report.image.imports.size() == 2);
  bool found_import1 = false, found_import2 = false;
  for (const auto& import : report.image.imports) {
    assert(import.module == "xboxkrnl.exe");
    assert(import.callable());
    if (import.guest_thunk == thunk_address && import.ordinal == 1) found_import1 = true;
    if (import.guest_thunk == thunk2_address && import.ordinal == 2) found_import2 = true;
  }
  assert(found_import1 && found_import2);
  std::cout << "  [PASS] Native import-libraries table parsed: xboxkrnl.exe guest_thunk=0x" << std::hex
             << thunk_address << " and 0x" << thunk2_address << std::dec << "\n";

  // No false "unsupported"/"invalid PPC encoding" diagnostic for either
  // thunk address - this is the exact bug: decoding loader-placeholder
  // metadata as PPC.
  for (const auto& unresolved : report.unresolved) {
    assert(unresolved.address != thunk_address && unresolved.address != thunk2_address &&
           "import-thunk address must never appear in report.unresolved (false decode-failure diagnostic)");
  }
  std::cout << "  [PASS] No false decode-failure diagnostic for either import-thunk address\n";

  // report.functions must contain exactly: entry, tail_caller and
  // lone_tail_caller (all compiled), and the two recognized-but-uncompiled
  // import-thunk entries.
  assert(report.functions.size() == 5);
  const DiscoveredFunction* entry_fn = nullptr;
  const DiscoveredFunction* tail_caller_fn = nullptr;
  const DiscoveredFunction* lone_tail_caller_fn = nullptr;
  const DiscoveredFunction* thunk_fn = nullptr;
  const DiscoveredFunction* thunk2_fn = nullptr;
  for (const auto& function : report.functions) {
    if (function.guest_start == entry_address) entry_fn = &function;
    if (function.guest_start == tail_caller_address) tail_caller_fn = &function;
    if (function.guest_start == lone_tail_caller_address) lone_tail_caller_fn = &function;
    if (function.guest_start == thunk_address) thunk_fn = &function;
    if (function.guest_start == thunk2_address) thunk2_fn = &function;
  }
  assert(entry_fn != nullptr && entry_fn->compiled && !entry_fn->import_thunk.has_value());
  assert(tail_caller_fn != nullptr && tail_caller_fn->compiled &&
         !tail_caller_fn->import_thunk.has_value());
  assert(lone_tail_caller_fn != nullptr && lone_tail_caller_fn->compiled &&
         !lone_tail_caller_fn->import_thunk.has_value());
  assert(thunk_fn != nullptr && thunk2_fn != nullptr);
  assert(!thunk_fn->compiled && !thunk2_fn->compiled &&
         "an import thunk must never be marked compiled - it has no real PPC body");
  assert(thunk_fn->import_thunk.has_value() && thunk_fn->import_thunk->module == "xboxkrnl.exe" &&
         thunk_fn->import_thunk->ordinal == 1);
  assert(thunk2_fn->import_thunk.has_value() && thunk2_fn->import_thunk->module == "xboxkrnl.exe" &&
         thunk2_fn->import_thunk->ordinal == 2);
  std::cout << "  [PASS] Both import thunks recorded as recognized, deliberately-uncompiled "
               "DiscoveredFunctions\n";

  // The second failure mode found in the real AC6 XEX: tail_caller's
  // control-flow edge into thunk #1 is a plain (non-linked) branch, not a
  // bl - confirm it was actually recorded that way.
  bool found_nonlinked_edge_to_thunk = false;
  for (const auto& branch : tail_caller_fn->branches) {
    if (branch.target == thunk_address && !branch.linked) found_nonlinked_edge_to_thunk = true;
  }
  assert(found_nonlinked_edge_to_thunk &&
         "tail_caller's tail call into the thunk must be recorded as a non-linked branch edge");
  std::cout << "  [PASS] Non-linked (tail-call) branch edge into thunk #1 recorded correctly\n";

  // The third failure mode found in the real AC6 XEX: lone_tail_caller
  // reaches thunk #2 ONLY via a non-linked branch - confirm thunk #2 was
  // still discovered/recognized (has_independent_discovery_evidence()) even
  // though nothing else (no `bl`) ever targets it.
  bool found_nonlinked_edge_to_thunk2 = false;
  for (const auto& branch : lone_tail_caller_fn->branches) {
    if (branch.target == thunk2_address && !branch.linked) found_nonlinked_edge_to_thunk2 = true;
  }
  assert(found_nonlinked_edge_to_thunk2 &&
         "lone_tail_caller's tail call into thunk #2 must be recorded as a non-linked branch edge");
  std::cout << "  [PASS] Thunk #2 (reachable only by a lone non-linked branch, no bl anywhere) was "
               "still correctly discovered and recognized\n";

  assert(report.diagnostics.import_thunks_recognized == 2);
  assert(report.diagnostics.functions_compiled == 3);
  assert(report.diagnostics.analysis_errors == 0 &&
         "a recognized import thunk must never be counted as an analysis error");
  std::cout << "  [PASS] Diagnostics: import_thunks_recognized=2, functions_compiled=3, analysis_errors=0\n";

  // Both thunks must be published into report.entries as first-class,
  // dispatchable ImportThunk entries - this is what makes
  // validate_region_entry_integrity() accept the non-linked edges above
  // instead of reporting "leaves owner ... without a dispatchable guest
  // entry" (the exact real-AC6 generation failures this regression test
  // reproduces: generation-status.json reported "control-flow edge
  // 0x821f4118 -> 0x823d014c leaves owner 0x821f4110 without a dispatchable
  // guest entry", then after fixing that, "control-flow edge 0x821f4120 ->
  // 0x823d00cc leaves owner 0x821f4120 without a dispatchable guest entry",
  // both before this fix).
  for (const auto addr : {thunk_address, thunk2_address}) {
    const auto thunk_entry = std::find_if(report.entries.begin(), report.entries.end(),
                                          [&](const auto& e) { return e.address == addr; });
    assert(thunk_entry != report.entries.end());
    assert(thunk_entry->kind == GuestEntryKind::ImportThunk);
  }
  std::cout << "  [PASS] Both import thunks published into report.entries as GuestEntryKind::ImportThunk\n";

  // Codegen must never emit a lookup_compiled() case (or a compiled-function
  // table entry) for either thunk address - the runtime's existing
  // lookup_compiled-miss -> runtime.call() fallback must remain unshadowed.
  // generate_project() succeeding at all is itself the entry-integrity
  // regression check: before this fix, these exact scenarios (non-linked
  // edges into unpublished/undiscovered import-thunk addresses) made
  // validate_region_entry_integrity() fail generation outright.
  if (!generate_project(options, report, error)) {
    std::cerr << "generate_project failed: " << error << "\n";
    return 1;
  }
  const auto registry_text = read_file(output / "registry.cpp");
  for (const auto addr : {thunk_address, thunk2_address}) {
    std::ostringstream thunk_hex;
    thunk_hex << std::hex << addr;
    const std::string case_needle = "case 0x" + thunk_hex.str() + ":";
    const std::string table_needle = "{0x" + thunk_hex.str();
    assert(registry_text.find(case_needle) == std::string::npos &&
           "no lookup_compiled() case may exist for an import-thunk address");
    assert(registry_text.find(table_needle) == std::string::npos &&
           "no kCompiledFunctions[] entry may exist for an import-thunk address");
  }
  std::cout << "  [PASS] Generated registry.cpp emits no compiled-function entry for either import-thunk "
               "address\n";

  std::filesystem::remove_all(root);
  std::cout << "All import-thunk analysis tests passed!\n";
  return 0;
}

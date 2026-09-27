// Regression test for a tail-call discovery gap found by actually running
// the real Ace Combat 6 default.xex through Project-Gracemeria's build
// (after fixing the unrelated import-thunk misclassification bug - see
// tests/recomp/import_thunk_analysis_tests.cpp): a real guest function can
// tail-call a genuine, valid PPC code block that:
//   - no `bl` anywhere in the title ever targets directly (so
//     has_independent_discovery_evidence() finds no DirectCall/etc.
//     evidence), and
//   - does not start with a recognized function prologue (looks_like_
//     function_prologue() only matches stwu(x) r1,r1 or mflr - a shared/
//     outlined tail-call target commonly reuses the CALLER's stack frame
//     and so has no prologue of its own), which also makes
//     load_and_analyze()'s separate gap-recovery pass (driver.cpp's
//     recover_unowned_gaps loop) skip it.
//
// Before the fix, analyze_function_candidate()'s promote_terminal_target()
// therefore never promoted the target to a discovery candidate at all, so
// it was silently left as a "branch-into-unknown-code" unresolved warning -
// and generate_project()'s validate_region_entry_integrity() then failed
// generation outright: real AC6 repro was "control-flow edge 0x8224c9cc ->
// 0x8224c458 leaves owner 0x8224c9a0 without a dispatchable guest entry",
// where 0x8224c458's real first instruction is `cmplwi cr?,r5,0`
// (0x2B050000) - not a prologue, and reached only by that one tail call.
//
// The fix: promote_terminal_target() also accepts a terminal branch's
// target when it decodes into a fully valid, terminator-reaching
// instruction stream (decodes_to_valid_terminated_block() in driver.cpp) -
// the same structural validation gap-recovery already trusts elsewhere,
// applied to one specific, already-evidenced control-flow edge instead of a
// blind scan, so no more permissive than a check this codebase already
// relies on.
//
// Uses the same small hand-built synthetic XEX technique as
// tests/recomp/recomp_driver_tests.cpp and
// tests/recomp/import_thunk_analysis_tests.cpp.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
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
constexpr std::uint32_t kBlr = 0x4E800020u;
// The exact real Ace Combat 6 first-instruction byte pattern at guest
// address 0x8224C458 - `cmplwi cr6,r5,0` - confirmed against the real,
// decrypted/decompressed default.xex. Deliberately NOT a recognized
// prologue (stwu(x) r1,r1 / mflr).
constexpr std::uint32_t kSharedBlockFirstWord = 0x2B050000u;

std::uint32_t branch_word(std::int32_t offset, bool link) {
  return 0x48000000u | (static_cast<std::uint32_t>(offset) & 0x03FFFFFCu) | (link ? 1u : 0u);
}

// Word layout in .text. shared_block is placed BEFORE (at a lower address
// than) tail_caller, so the branch reaching it is BACKWARD - matching the
// real AC6 repro exactly (0x8224c458 < 0x8224c9a0, the tail-calling
// function's own start). This distinction matters: a FORWARD non-linked
// branch target (>= the branching function's own start) is handled by a
// different, pre-existing mechanism (queue_local_target()'s "inferred local
// CFG range" inlining, driver.cpp's loop `if (branch.linked || branch.target
// < function.guest_start) continue; queue_local_target(branch.target);`),
// which would absorb a forward target into the branching function's own
// body before promote_terminal_target() ever runs - masking the exact gap
// this test exists to catch. Only a backward target genuinely exercises
// promote_terminal_target()'s new decodes_to_valid_terminated_block() path.
//   [0] cmplwi cr6,r5,0                (shared_block instr 0 - not a
//                                       prologue)
//   [1] blr                            (shared_block instr 1 - terminator)
//   [2] bl -> tail_caller (word index 4)
//   [3] blr                           (entry: 2 words)
//   [4] b -> shared_block (word index 0), NOT linked, terminal, BACKWARD
//                                      (tail_caller: single instruction -
//                                       the ONLY thing anywhere that
//                                       reaches shared_block)
constexpr std::size_t kSharedBlockWordIndex = 0;
constexpr std::size_t kEntryWordIndex = 2;
constexpr std::size_t kTailCallerWordIndex = 4;
constexpr std::size_t kTotalWords = 5;

std::vector<std::byte> make_xex() {
  constexpr std::size_t header = 0x300;
  constexpr std::size_t security = 0x20;
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
  be32(bytes, 0x14, 0);  // optional_header_count = 0 (no import table needed here)

  be32(bytes, security + 0x000, 0x184);
  be32(bytes, security + 0x004, static_cast<std::uint32_t>(bytes.size() - header));
  be32(bytes, security + 0x110, kLoadAddress);

  bytes[header] = std::byte{'M'}; bytes[header + 1] = std::byte{'Z'};
  le32(bytes, header + 0x3C, static_cast<std::uint32_t>(pe - header));
  bytes[pe] = std::byte{'P'}; bytes[pe + 1] = std::byte{'E'};
  le16(bytes, coff, 0x14C);
  le16(bytes, coff + 2, 1);
  le16(bytes, coff + 0x10, 0xE0);
  le16(bytes, coff + 0x12, 0x0102);
  le16(bytes, optional, 0x10B);
  le32(bytes, optional + 0x10, kTextRva + static_cast<std::uint32_t>(kEntryWordIndex * 4u));
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
  be32(bytes, text_file_base + kSharedBlockWordIndex * 4, kSharedBlockFirstWord);
  be32(bytes, text_file_base + (kSharedBlockWordIndex + 1) * 4, kBlr);
  be32(bytes, text_file_base + kEntryWordIndex * 4, branch_word(rel(kEntryWordIndex, kTailCallerWordIndex), true));
  be32(bytes, text_file_base + (kEntryWordIndex + 1) * 4, kBlr);
  be32(bytes, text_file_base + kTailCallerWordIndex * 4,
       branch_word(rel(kTailCallerWordIndex, kSharedBlockWordIndex), false));

  return bytes;
}

}  // namespace

int main() {
  std::cout << "Testing tail-call-only shared-block discovery fix...\n";

  const auto root = std::filesystem::temp_directory_path() / "xenon_tail_call_gap_discovery_test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto input = root / "fixture.xex";
  const auto output = root / "generated";
  {
    const auto xex_bytes = make_xex();
    std::ofstream(input, std::ios::binary)
        .write(reinterpret_cast<const char*>(xex_bytes.data()), static_cast<std::streamsize>(xex_bytes.size()));
  }

  const std::uint32_t entry_address =
      kLoadAddress + kTextRva + static_cast<std::uint32_t>(kEntryWordIndex * 4u);
  const std::uint32_t tail_caller_address =
      kLoadAddress + kTextRva + static_cast<std::uint32_t>(kTailCallerWordIndex * 4u);
  const std::uint32_t shared_block_address =
      kLoadAddress + kTextRva + static_cast<std::uint32_t>(kSharedBlockWordIndex * 4u);

  DriverOptions options{};
  options.input = input;
  options.output = output;

  AnalysisReport report;
  std::string error;
  if (!load_and_analyze(options, report, error)) {
    std::cerr << "load_and_analyze failed: " << error << "\n";
    return 1;
  }

  // shared_block must have been discovered and compiled, not left as an
  // unresolved "branch-into-unknown-code" hole.
  for (const auto& unresolved : report.unresolved) {
    assert(unresolved.address != shared_block_address &&
           "the tail-call-only shared block must not be left unresolved");
  }
  const DiscoveredFunction* entry_fn = nullptr;
  const DiscoveredFunction* tail_caller_fn = nullptr;
  const DiscoveredFunction* shared_block_fn = nullptr;
  for (const auto& function : report.functions) {
    if (function.guest_start == entry_address) entry_fn = &function;
    if (function.guest_start == tail_caller_address) tail_caller_fn = &function;
    if (function.guest_start == shared_block_address) shared_block_fn = &function;
  }
  assert(entry_fn != nullptr && entry_fn->compiled);
  assert(tail_caller_fn != nullptr && tail_caller_fn->compiled);
  assert(shared_block_fn != nullptr &&
         "the shared block (reachable only by a lone non-linked tail call to a non-prologue "
         "instruction) must still be discovered");
  assert(shared_block_fn->compiled &&
         "the shared block must actually compile once discovered - its bytes are genuine, valid PPC");
  std::cout << "  [PASS] Tail-call-only shared block (no bl anywhere, no recognized prologue) was "
               "discovered and compiled\n";

  // Confirm the discovery path was DirectBranch (promote_terminal_target()),
  // not some other mechanism (e.g. gap recovery, which would also have
  // skipped this address since it likewise requires
  // looks_like_function_prologue()).
  bool via_direct_branch = false;
  for (const auto source : shared_block_fn->sources)
    if (source == DiscoverySource::DirectBranch) via_direct_branch = true;
  assert(via_direct_branch &&
         "the shared block must be attributed to DirectBranch (promote_terminal_target()'s new "
         "decodes_to_valid_terminated_block() acceptance path), not gap recovery");
  std::cout << "  [PASS] Discovery attributed to DirectBranch (promote_terminal_target), confirming "
               "gap recovery was not what found it\n";

  // generate_project() succeeding at all is the entry-integrity regression
  // check: before this fix, this exact scenario made
  // validate_region_entry_integrity() fail generation outright.
  if (!generate_project(options, report, error)) {
    std::cerr << "generate_project failed: " << error << "\n";
    return 1;
  }
  std::cout << "  [PASS] generate_project() succeeded (entry-integrity validation passed)\n";

  std::filesystem::remove_all(root);
  std::cout << "All tail-call gap discovery tests passed!\n";
  return 0;
}

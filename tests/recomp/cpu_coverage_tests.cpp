// CPU instruction coverage audit (recomp::audit_cpu_coverage).
//
// The audit exists because Ace Combat 6's first real run ended on an instruction
// (rldicl) that the dynamic fallback could not execute - a gap nothing reported
// until the game hit that exact code. These tests check that the audit finds such
// a gap, counts and locates it correctly, and does not report false gaps.

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "xenon/cpu/decoder.hpp"
#include "xenon/recomp/cpu_coverage.hpp"

using namespace xenon;
using namespace xenon::recomp;

namespace {

constexpr std::uint32_t kAddi = 0x38600001u;    // li r3,1
constexpr std::uint32_t kRldicl = 0x794A0020u;  // rldicl r10,r10,0,32 (the AC6 word)
constexpr std::uint32_t kBlr = 0x4E800020u;
constexpr std::uint32_t kLwz = 0x80830000u;     // lwz r4,0(r3)
constexpr std::uint32_t kAdd = 0x7C632214u;     // add r3,r3,r4
constexpr std::uint32_t kInvalid = 0x00000000u;  // padding / data

xbox::XexSection make_section(std::uint32_t base, bool executable,
                              const std::vector<std::uint32_t>& words) {
  xbox::XexSection section;
  section.name = executable ? ".text" : ".data";
  section.virtual_address = base;
  section.executable = executable;
  for (const auto word : words) {
    section.bytes.push_back(static_cast<std::byte>(word >> 24));
    section.bytes.push_back(static_cast<std::byte>(word >> 16));
    section.bytes.push_back(static_cast<std::byte>(word >> 8));
    section.bytes.push_back(static_cast<std::byte>(word));
  }
  section.virtual_size = static_cast<std::uint32_t>(section.bytes.size());
  section.raw_size = section.virtual_size;
  return section;
}

xbox::XexImage make_image() {
  xbox::XexImage image;
  image.sections.push_back(make_section(
      0x82000000u, true, {kAddi, kRldicl, kBlr, kInvalid, kRldicl, kLwz, kAdd, kRldicl, kBlr, kAddi}));
  // A non-executable section full of rotate words must be ignored entirely.
  image.sections.push_back(make_section(0x82100000u, false, {kRldicl, kRldicl, kRldicl, kRldicl}));
  return image;
}

// With the real oracles, ordinary integer/memory/branch instructions are all
// supported by both the native backend and the fallback, and the counts add up.
void test_real_oracles_report_no_gap_for_common_code() {
  const auto report = audit_cpu_coverage(make_image());
  assert(report.words_scanned == 10u && "only the executable section is scanned");
  assert(report.undecodable_words == 1u && "the zero word is padding, not an instruction");
  assert(report.instructions_decoded == 9u);
  assert(report.distinct_mnemonics == 5u);  // addi, rldicl, bclr, lwz, add
  assert(report.gaps.empty() && "rldicl, li, blr, lwz and add are all supported");
  assert(format_cpu_coverage(report).find("no gaps") != std::string::npos);
}

// The individual oracles agree on the AC6 instruction that used to trap.
void test_the_original_ac6_instruction_is_supported() {
  cpu::Decoder decoder;
  const auto insn = decoder.decode(0x821DCC80u, kRldicl);
  assert(insn.valid() && insn.mnemonic() == "rldiclx");
  assert(aot_supports_instruction(insn) && "the compiled path always had it");
  assert(fallback_supports_instruction(insn) && "and the fallback now does");
  assert(!aot_supports_instruction(decoder.decode(0x821DCC80u, kInvalid)));
  assert(!fallback_supports_instruction(decoder.decode(0x821DCC80u, kInvalid)));
}

// A denied instruction is reported with its true count, first address, one
// concrete encoding and which side lacks it; gaps are ordered most frequent first.
void test_gaps_are_counted_located_and_ordered() {
  CpuCoverageOracles oracles;
  oracles.fallback = [](const cpu::DecodedInstruction& insn) {
    return insn.mnemonic() != "rldiclx";  // pretend the fallback still lacks rldicl
  };
  oracles.aot = [](const cpu::DecodedInstruction& insn) { return insn.mnemonic() != "addi"; };
  const auto report = audit_cpu_coverage(make_image(), oracles);
  assert(report.gaps.size() == 2u);

  // rldicl appears 3 times, addi twice: rldicl sorts first.
  const auto& rldicl = report.gaps[0];
  assert(rldicl.mnemonic == "rldiclx" && rldicl.count == 3u);
  assert(rldicl.first_address == 0x82000004u && rldicl.example_word == kRldicl);
  assert(rldicl.aot_supported && !rldicl.fallback_supported);

  const auto& addi = report.gaps[1];
  assert(addi.mnemonic == "addi" && addi.count == 2u && addi.first_address == 0x82000000u);
  assert(!addi.aot_supported && addi.fallback_supported);

  assert(report.fallback_gap_count() == 1u && report.aot_gap_count() == 1u);
  const auto text = format_cpu_coverage(report);
  assert(text.find("rldiclx x3") != std::string::npos);
  assert(text.find("NO-FALLBACK") != std::string::npos && text.find("NO-AOT") != std::string::npos);
  assert(text.find("0x82000004") != std::string::npos && text.find("0x794A0020") != std::string::npos);
}

// A mnemonic unsupported on both sides is one entry carrying both flags.
void test_unsupported_on_both_sides() {
  CpuCoverageOracles oracles;
  oracles.aot = oracles.fallback = [](const cpu::DecodedInstruction& insn) {
    return insn.mnemonic() != "lwz";
  };
  const auto report = audit_cpu_coverage(make_image(), oracles);
  assert(report.gaps.size() == 1u);
  assert(report.gaps[0].mnemonic == "lwz" && !report.gaps[0].aot_supported &&
         !report.gaps[0].fallback_supported && report.gaps[0].count == 1u);
}

// An image with no executable section, or an empty one, audits cleanly.
void test_empty_images() {
  xbox::XexImage empty;
  const auto report = audit_cpu_coverage(empty);
  assert(report.words_scanned == 0u && report.gaps.empty() && report.distinct_mnemonics == 0u);
  xbox::XexImage data_only;
  data_only.sections.push_back(make_section(0x82000000u, false, {kRldicl}));
  assert(audit_cpu_coverage(data_only).instructions_decoded == 0u);
}

}  // namespace

int main() {
  std::cout << "Testing CPU coverage audit...\n";
  test_real_oracles_report_no_gap_for_common_code();
  test_the_original_ac6_instruction_is_supported();
  test_gaps_are_counted_located_and_ordered();
  test_unsupported_on_both_sides();
  test_empty_images();
  std::cout << "All CPU coverage audit tests passed!\n";
  return 0;
}

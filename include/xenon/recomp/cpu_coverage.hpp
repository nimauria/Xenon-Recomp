#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "xenon/cpu/instruction.hpp"
#include "xenon/xbox/xex_loader.hpp"

namespace xenon::recomp {

// CPU instruction coverage audit.
//
// A recompiled title can end in two ways that have nothing to do with the kernel:
//   * AOT gap      - an instruction the native backend cannot lower fails code
//                    generation (or generates a stub) for the whole function.
//   * Fallback gap - code the recompiler never discovered (reached only through
//                    an indirect call at runtime) executes in the dynamic
//                    fallback interpreter, and an instruction it cannot execute
//                    ends the run with "unsupported instruction".
// The second is the dangerous one because nothing shows it until the game hits
// that exact code. Ace Combat 6 ended its first real run that way: the fallback
// had no `rldicl`. This audit decodes every executable word of an image, asks the
// real backend and the real interpreter whether each distinct instruction is
// supported (by running them, not by consulting a hand-maintained list that could
// drift), and reports the gaps with counts and the first address, so a missing
// instruction is known - and can be added - before a title depends on it.

struct CpuCoverageEntry {
  std::string mnemonic;
  std::uint64_t count{};           // occurrences in executable sections
  std::uint32_t first_address{};   // first guest address it appears at
  std::uint32_t example_word{};    // one concrete encoding, for reproduction
  bool aot_supported{true};
  bool fallback_supported{true};
};

struct CpuCoverageReport {
  std::uint64_t words_scanned{};
  std::uint64_t instructions_decoded{};
  // Words that are not a valid instruction: alignment padding and inline data
  // in code sections. Counted, never reported as gaps.
  std::uint64_t undecodable_words{};
  std::size_t distinct_mnemonics{};
  // Only mnemonics unsupported by the AOT backend or the fallback, most
  // frequent first.
  std::vector<CpuCoverageEntry> gaps;

  [[nodiscard]] std::size_t aot_gap_count() const noexcept;
  [[nodiscard]] std::size_t fallback_gap_count() const noexcept;
};

// The ground truth for one instruction. Exposed so tests (and other tools) ask the
// same questions the audit does.
//   AOT:      lift to IR and run the real C++ backend; a rejected op means the
//             function containing it cannot be compiled natively.
//   Fallback: cpu::dynamic_fallback_supports() - the interpreter's own decision.
[[nodiscard]] bool aot_supports_instruction(const cpu::DecodedInstruction& insn);
[[nodiscard]] bool fallback_supports_instruction(const cpu::DecodedInstruction& insn);

// Support oracles, injectable so the audit's bookkeeping can be tested without
// depending on today's exact coverage. The defaults are the two functions above.
struct CpuCoverageOracles {
  std::function<bool(const cpu::DecodedInstruction&)> aot = aot_supports_instruction;
  std::function<bool(const cpu::DecodedInstruction&)> fallback = fallback_supports_instruction;
};

[[nodiscard]] CpuCoverageReport audit_cpu_coverage(const xbox::XexImage& image,
                                                   const CpuCoverageOracles& oracles = {});

// One line per gap plus a summary, suitable for analysis reports and logs.
[[nodiscard]] std::string format_cpu_coverage(const CpuCoverageReport& report);

}  // namespace xenon::recomp

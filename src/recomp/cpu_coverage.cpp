#include "xenon/recomp/cpu_coverage.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <sstream>
#include <string_view>
#include <unordered_map>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/dynamic_fallback.hpp"
#include "xenon/cpu/flat_memory.hpp"
#include "xenon/cpu/ir.hpp"
#include "xenon/cpu/lifter.hpp"

namespace xenon::recomp {

std::size_t CpuCoverageReport::aot_gap_count() const noexcept {
  return static_cast<std::size_t>(
      std::count_if(gaps.begin(), gaps.end(), [](const auto& g) { return !g.aot_supported; }));
}

std::size_t CpuCoverageReport::fallback_gap_count() const noexcept {
  return static_cast<std::size_t>(
      std::count_if(gaps.begin(), gaps.end(), [](const auto& g) { return !g.fallback_supported; }));
}

bool aot_supports_instruction(const cpu::DecodedInstruction& insn) {
  if (!insn.valid()) return false;
  try {
    cpu::ir::Block block{};
    block.guest_address = insn.address;
    block.end_address = insn.address + 4u;
    cpu::ir::Builder builder(block);
    builder.set_guest(&insn);
    if (!cpu::Lifter{}.lift(insn, builder)) return false;
    // The lifter also accepts operations it recognizes but has not expanded (they
    // become semantic placeholders the backend must reject), so lifting alone does
    // not prove native support: run the real backend, exactly as code generation
    // does, and treat a rejected op as unsupported.
    static_cast<void>(cpu::backend::CppAotBackend{}.emit_function(block, "audit_probe"));
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

bool fallback_supports_instruction(const cpu::DecodedInstruction& insn) {
  // A small zeroed flat memory: enough to be a valid port. Loads/stores through
  // zero registers fault on it, which the oracle counts as "recognized".
  static thread_local cpu::FlatMemory scratch(0x1000u, 0u);
  return cpu::dynamic_fallback_supports(insn, scratch);
}

CpuCoverageReport audit_cpu_coverage(const xbox::XexImage& image, const CpuCoverageOracles& oracles) {
  CpuCoverageReport report;
  cpu::Decoder decoder;

  struct Seen {
    std::uint64_t count{};
    cpu::DecodedInstruction first{};
  };
  std::unordered_map<std::string_view, Seen> seen;

  for (const auto& section : image.sections) {
    if (!section.executable) continue;
    const auto words = section.bytes.size() / 4u;
    for (std::size_t index = 0; index < words; ++index) {
      const auto* p = reinterpret_cast<const unsigned char*>(section.bytes.data()) + index * 4u;
      const std::uint32_t word = (std::uint32_t{p[0]} << 24u) | (std::uint32_t{p[1]} << 16u) |
                                 (std::uint32_t{p[2]} << 8u) | std::uint32_t{p[3]};
      const auto address = section.virtual_address + static_cast<std::uint32_t>(index * 4u);
      ++report.words_scanned;
      const auto insn = decoder.decode(address, word);
      if (!insn.valid()) {
        ++report.undecodable_words;
        continue;
      }
      ++report.instructions_decoded;
      auto& entry = seen[insn.mnemonic()];
      if (entry.count++ == 0u) entry.first = insn;
    }
  }
  report.distinct_mnemonics = seen.size();

  for (const auto& [mnemonic, info] : seen) {
    const bool aot = oracles.aot(info.first);
    const bool fallback = oracles.fallback(info.first);
    if (aot && fallback) continue;
    CpuCoverageEntry entry;
    entry.mnemonic = std::string(mnemonic);
    entry.count = info.count;
    entry.first_address = info.first.address;
    entry.example_word = info.first.word;
    entry.aot_supported = aot;
    entry.fallback_supported = fallback;
    report.gaps.push_back(std::move(entry));
  }
  std::sort(report.gaps.begin(), report.gaps.end(), [](const auto& a, const auto& b) {
    return a.count != b.count ? a.count > b.count : a.mnemonic < b.mnemonic;
  });
  return report;
}

std::string format_cpu_coverage(const CpuCoverageReport& report) {
  std::ostringstream out;
  out << "CPU instruction coverage: " << report.instructions_decoded << " instructions ("
      << report.distinct_mnemonics << " distinct) scanned, " << report.undecodable_words
      << " non-instruction words skipped\n";
  if (report.gaps.empty()) {
    out << "  no gaps: every instruction is supported by the native backend and the dynamic fallback\n";
    return out.str();
  }
  out << "  " << report.aot_gap_count() << " instruction kind(s) unsupported by the native backend, "
      << report.fallback_gap_count() << " by the dynamic fallback\n";
  for (const auto& gap : report.gaps) {
    char where[64];
    std::snprintf(where, sizeof(where), "first at 0x%08X, e.g. word 0x%08X", gap.first_address,
                  gap.example_word);
    out << "  " << gap.mnemonic << " x" << gap.count << " [" << (gap.aot_supported ? "" : "NO-AOT ")
        << (gap.fallback_supported ? "" : "NO-FALLBACK") << "] " << where << "\n";
  }
  return out.str();
}

}  // namespace xenon::recomp

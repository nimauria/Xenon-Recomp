#pragma once

// Function discovery inputs: the executable-range index the analyzer
// queries and the evidence (DiscoverySource) behind each candidate.

#include <cstdint>
#include <vector>
#include "xenon/xbox/xex_loader.hpp"

namespace xenon::recomp {

// Sorted, indexed description of a XexImage's executable ranges (Part 7 of
// the Recomp Analysis V2 pass) - replaces repeated linear scans over
// `image.sections` (xbox::XexImage::sections is a flat, unsorted vector) in
// every hot per-candidate/per-address check the analyzer performs. Built
// once per analysis run from the immutable image, then queried read-only
// (safe to share across analysis worker threads).
class ExecutableRangeIndex {
 public:
  explicit ExecutableRangeIndex(const xbox::XexImage& image);

  // The containing section for `address` if one exists (executable or not),
  // else nullptr.
  [[nodiscard]] const xbox::XexSection* containing_section(std::uint32_t address) const noexcept;
  // True iff `address` falls inside a section marked executable.
  [[nodiscard]] bool is_executable_address(std::uint32_t address) const noexcept;
  // True iff `address` falls inside any known section (executable or not) -
  // i.e. the address is part of the mapped image at all.
  [[nodiscard]] bool is_mapped_address(std::uint32_t address) const noexcept;
  // True iff `address` satisfies PPC's mandatory 4-byte instruction
  // alignment. A function/branch candidate failing this can never be real
  // code and should be rejected before any decode is attempted.
  [[nodiscard]] static bool is_aligned_ppc_address(std::uint32_t address) noexcept;

 private:
  struct Entry {
    std::uint32_t begin;
    std::uint32_t end;  // exclusive
    const xbox::XexSection* section;
  };
  std::vector<Entry> entries_;  // sorted by `begin`, non-overlapping per input section
};

enum class DiscoverySource : std::uint8_t {
  EntryPoint,
  Export,
  DirectCall,
  DirectBranch,
  UnwindMetadata,
  ModuleHint,
  // Recomp Analysis V3 (discovery-quality pass) additions - see
  // docs/recomp/RECOMP_ANALYSIS_V3.md for the full provenance/confidence model.
  TlsCallback,         // XEX TLS directory callback address (Part 2) - a real,
                       // generic loader-exposed entry point, not title-specific.
  ResolvedIndirect,    // legacy/coarse indirect-target provenance retained for
                       // report compatibility. New analysis records the more
                       // precise call/branch variants below; this source alone
                       // is NOT proof of a semantic function boundary.
  ResolvedIndirectCall,   // statically resolved linked/indirect call target.
                          // Strong callable evidence and eligible to anchor a
                          // semantic function boundary.
  ResolvedIndirectBranch, // statically resolved non-linked indirect branch or
                          // switch target. Dispatchable entry evidence only;
                          // never self-promotes an internal block to a function.
  ControlFlowHint,        // module-provided branch/switch target. The hint proves
                          // the edge/entry, not that the target begins a distinct
                          // semantic function.
  ValidatedTailCall,   // a non-linked branch whose target's control-flow
                       // shape corroborates it being a genuine tail call to a
                       // separate function rather than an internal jump
                       // (Part 4/10).
  PrologueHeuristic,   // recognized compiler prologue/epilogue pattern (Part
                       // 4) - supporting evidence only; Xenon never creates a
                       // function candidate from this alone.
  PointerTable,        // target recovered from a generic static function-pointer/vtable
                       // run in non-executable image data. Entry evidence, not by itself
                       // authoritative proof of a semantic function boundary.
  RuntimeObservation,  // executable guest entry observed by a prior runtime/trace pass.
                       // Strong evidence that the address must be dispatchable, but not
                       // necessarily that it begins a distinct semantic function.
  GapRecovery,         // orphan executable branch target recovered after graph closure
  KnowledgeMatch,      // Gen 9 normalized cross-revision fingerprint corroboration.
                       // Never a semantic boundary on its own.
                       // because multiple independent owners reference it. Low authority;
                       // eligible for absorption into a stronger overlapping owner.
};

// Part 11: confidence is derived from evidence (DiscoverySource), not an
// arbitrary single-source magic number. Returns the strongest single piece
// of evidence's base confidence, plus a small corroboration bonus when two
// or more independent sources agree on the same candidate (capped at 100).
// Ordering here matches the tiers real Xbox 360 static analysis evidence
// actually has: entry point/explicit module hint/verified unwind metadata
// are as strong as generic evidence gets; a bare heuristic pattern with no
// other corroboration is the weakest signal this scheme ever trusts.
[[nodiscard]] std::uint32_t discovery_source_base_confidence(DiscoverySource source) noexcept;
[[nodiscard]] std::uint32_t confidence_for_sources(const std::vector<DiscoverySource>& sources) noexcept;
[[nodiscard]] const char* discovery_source_name(DiscoverySource source) noexcept;

}  // namespace xenon::recomp

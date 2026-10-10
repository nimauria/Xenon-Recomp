#pragma once

// Per-function analysis results: discovered functions, dispatchable guest
// entries, branch facts, return behaviour and unresolved references.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "xenon/cpu/ir.hpp"
#include "xenon/recomp/analysis_schema.hpp"
#include "xenon/recomp/compilation_graph.hpp"
#include "xenon/recomp/discovery.hpp"
#include "xenon/recomp/knowledge_base.hpp"

namespace xenon::recomp {

struct UnresolvedReference {
  std::uint32_t address{};
  std::uint32_t target{};
  std::string kind;
  std::string detail;
};

// Function identity and dispatchable guest entry identity are deliberately
// separate. Xbox 360 code routinely has shared suffixes, outlined blocks,
// funclets and indirect-entry blocks that are valid control-flow destinations
// without being independent C/C++ functions.
enum class FunctionAuthority : std::uint8_t {
  GapRecovery,
  InferredBranch,
  PointerTable,
  RuntimeObservation,
  ResolvedIndirect,
  DirectCall,
  Metadata,
  ModuleHint,
  EntryPoint,
};

enum class GuestEntryKind : std::uint8_t {
  Function,
  AlternateBlock,
  RuntimeHelper,
  NativeReplacement,
  // A recognized XEX-native import thunk (DiscoveredFunction::import_thunk).
  // Like RuntimeHelper, this has no compiled owner - resolution happens at
  // runtime via XenonSession::call()'s guest_thunk match, never via a
  // materialized local CFG block or a lookup_compiled() case. Entered into
  // report.entries purely so entry-integrity validation recognizes a `bl` or
  // plain-branch edge into one as landing on a legitimate dispatchable
  // target, not a hole.
  ImportThunk,
};

// Gen 6 call/return fixed-point result. MayReturn means at least one proven
// normal return path exists; NoReturn means all currently-known terminal exits
// are proven non-returning (or the module explicitly declared NoReturn).
enum class ReturnBehavior : std::uint8_t {
  Unknown,
  MayReturn,
  NoReturn,
};

struct BranchReference {
  std::uint32_t site{};
  std::uint32_t target{};
  // `terminal` means the edge terminates the current basic block without a
  // fallthrough path. It is deliberately separate from `conditional`: a
  // conditional branch ends a block for CFG purposes but is not a semantic
  // tail call. Keeping these facts separate prevents post-analysis
  // reconciliation from re-promoting loop headers / shared suffixes into
  // functions merely because they cross a tentative function extent.
  bool terminal{};
  bool indirect{};
  bool linked{};
  bool conditional{};
  bool fallthrough{};
};

struct GuestEntryPoint {
  std::uint32_t address{};
  std::uint32_t owner_function{};
  std::uint32_t block{};
  GuestEntryKind kind{GuestEntryKind::Function};
  std::vector<DiscoverySource> sources;
  std::uint32_t confidence{};
};

struct DiscoveredFunction {
  std::uint32_t guest_start{};
  std::uint32_t guest_end{};
  std::vector<std::uint32_t> ranges;
  std::string name;
  std::vector<DiscoverySource> sources;
  std::vector<std::uint32_t> calls;
  std::vector<std::uint32_t> callers;
  std::vector<std::uint32_t> branch_references; // compatibility/summary target list
  std::vector<BranchReference> branches;          // exact source-site control-flow facts
  FunctionAuthority authority{FunctionAuthority::InferredBranch};
  std::uint32_t confidence{};
  std::uint64_t source_hash{};
  bool compiled{};
  std::string error;
  cpu::ir::Function ir;
  std::vector<graph::Node> compilation_nodes;
  bool ir_cache_hit{};

  // Gen 6 return/no-return analysis. has_explicit_return is a local decode fact
  // (an unconditional bclrx/blr-style return was observed). The fixed-point
  // pass may then propagate MayReturn/NoReturn through terminal tail calls.
  ReturnBehavior return_behavior{ReturnBehavior::Unknown};
  bool return_behavior_explicit{};
  bool has_explicit_return{};

  // Gen 9 universal knowledge-base identity and any confidence-scored
  // matches accepted for this function. These are report/evidence data; a
  // knowledge label never silently replaces a module-provided symbol name.
  FunctionFingerprint fingerprint{};
  std::vector<KnowledgeMatchReport> knowledge_matches;

  // Reserved for a FunctionChunk hint's declared parent address; currently
  // unset by every production path. A FunctionChunk's bytes ARE stitched
  // into the parent's own compiled IR (see driver.cpp's
  // analyze_function_candidate()), so a chunk never gets its own
  // DiscoveredFunction entry to record this on in the first place - the
  // association is recoverable from the parent's own `ranges` instead. See
  // docs/recomp/ANALYSIS_HINT_SCHEMA_V2.md's "known limitations" section.
  std::optional<std::uint32_t> chunk_parent;
  // Set when a NativeReplacement hint (Part 1.10) matched this address and
  // Xenon has a real implementation for it (native_replacements.hpp) -
  // generate_project() emits a call to that implementation instead of
  // compiling guest bytes at this address.
  std::optional<analysis::NativeReplacementKind> native_replacement;
  // Set when this candidate's start address is an XEX-native import record's
  // callable guest_thunk (xbox::XexImport::callable()) - the address is
  // loader-owned placeholder data (ordinal/attributes/record-type, see
  // xex_loader.cpp's parse_native_import_libraries()), never guest PPC
  // bytes, and must never be decoded/compiled. Xenon's runtime import
  // dispatch (XenonSession::call(), src/core/session/execution/runtime_services.cpp) already resolves
  // calls to this exact address by matching it against
  // XexImage::imports[].guest_thunk; this field only records the
  // recognition so static analysis never misclassifies the address as an
  // "unsupported"/"invalid" PPC function and never shadows that dispatch
  // with a bogus compiled entry. `compiled` stays false for these entries
  // deliberately - see analyze_function_candidate()'s import-thunk
  // short-circuit in driver.cpp.
  struct ImportThunkBinding {
    std::string module;
    std::string symbol;
    std::uint16_t ordinal{};
  };
  std::optional<ImportThunkBinding> import_thunk;
};

[[nodiscard]] const char* function_authority_name(FunctionAuthority authority) noexcept;
[[nodiscard]] const char* guest_entry_kind_name(GuestEntryKind kind) noexcept;
[[nodiscard]] const char* return_behavior_name(ReturnBehavior behavior) noexcept;

}  // namespace xenon::recomp

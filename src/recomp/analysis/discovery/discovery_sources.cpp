#include <algorithm>

#include "recomp/analysis/analysis_internal.hpp"

namespace xenon::recomp {

const char* discovery_source_name(DiscoverySource source) noexcept {
  switch (source) {
    case DiscoverySource::EntryPoint: return "entry";
    case DiscoverySource::Export: return "export";
    case DiscoverySource::DirectCall: return "direct-call";
    case DiscoverySource::DirectBranch: return "direct-branch";
    case DiscoverySource::UnwindMetadata: return "unwind-metadata";
    case DiscoverySource::ModuleHint: return "module-hint";
    case DiscoverySource::TlsCallback: return "tls-callback";
    case DiscoverySource::ResolvedIndirect: return "resolved-indirect";
    case DiscoverySource::ResolvedIndirectCall: return "resolved-indirect-call";
    case DiscoverySource::ResolvedIndirectBranch: return "resolved-indirect-branch";
    case DiscoverySource::ControlFlowHint: return "control-flow-hint";
    case DiscoverySource::ValidatedTailCall: return "validated-tail-call";
    case DiscoverySource::PrologueHeuristic: return "prologue-heuristic";
    case DiscoverySource::PointerTable: return "pointer-table";
    case DiscoverySource::RuntimeObservation: return "runtime-observation";
    case DiscoverySource::GapRecovery: return "gap-recovery";
    case DiscoverySource::KnowledgeMatch: return "knowledge-match";
  }
  return "unknown";
}

std::uint32_t discovery_source_base_confidence(DiscoverySource source) noexcept {
  switch (source) {
    case DiscoverySource::EntryPoint: return 100;
    case DiscoverySource::ModuleHint: return 100;
    case DiscoverySource::UnwindMetadata: return 95;
    case DiscoverySource::TlsCallback: return 95;
    case DiscoverySource::Export: return 90;
    case DiscoverySource::DirectCall: return 85;
    case DiscoverySource::ResolvedIndirect: return 72;
    case DiscoverySource::ResolvedIndirectCall: return 88;
    case DiscoverySource::ResolvedIndirectBranch: return 72;
    case DiscoverySource::ControlFlowHint: return 85;
    case DiscoverySource::ValidatedTailCall: return 65;
    case DiscoverySource::DirectBranch: return 60;
    case DiscoverySource::PointerTable: return 72;
    case DiscoverySource::RuntimeObservation: return 88;
    case DiscoverySource::GapRecovery: return 55;
    case DiscoverySource::KnowledgeMatch: return 58;
    case DiscoverySource::PrologueHeuristic: return 40;
  }
  return 50;
}

const char* function_authority_name(FunctionAuthority authority) noexcept {
  switch (authority) {
    case FunctionAuthority::GapRecovery: return "gap-recovery";
    case FunctionAuthority::InferredBranch: return "inferred-branch";
    case FunctionAuthority::RuntimeObservation: return "runtime-observation";
    case FunctionAuthority::PointerTable: return "pointer-table";
    case FunctionAuthority::ResolvedIndirect: return "resolved-indirect";
    case FunctionAuthority::DirectCall: return "direct-call";
    case FunctionAuthority::Metadata: return "metadata";
    case FunctionAuthority::ModuleHint: return "module-hint";
    case FunctionAuthority::EntryPoint: return "entry-point";
  }
  return "unknown";
}

const char* guest_entry_kind_name(GuestEntryKind kind) noexcept {
  switch (kind) {
    case GuestEntryKind::Function: return "function";
    case GuestEntryKind::AlternateBlock: return "alternate-block";
    case GuestEntryKind::RuntimeHelper: return "runtime-helper";
    case GuestEntryKind::NativeReplacement: return "native-replacement";
    case GuestEntryKind::ImportThunk: return "import-thunk";
  }
  return "unknown";
}

const char* return_behavior_name(ReturnBehavior behavior) noexcept {
  switch (behavior) {
    case ReturnBehavior::Unknown: return "unknown";
    case ReturnBehavior::MayReturn: return "may-return";
    case ReturnBehavior::NoReturn: return "no-return";
  }
  return "unknown";
}

std::uint32_t confidence_for_sources(const std::vector<DiscoverySource>& sources) noexcept {
  if (sources.empty()) return 50;
  std::uint32_t best = 0;
  for (const auto source : sources) best = std::max(best, discovery_source_base_confidence(source));
  // Corroboration bonus (Part 11): two or more independent pieces of
  // evidence agreeing on the same start address are stronger together than
  // either alone - e.g. a direct call target that is ALSO the target of a
  // validated tail call from elsewhere.
  if (sources.size() >= 2) best = std::min<std::uint32_t>(100, best + 5);
  return best;
}

namespace detail {

bool source_contains(const DiscoveredFunction& function, DiscoverySource source) {
  return std::find(function.sources.begin(), function.sources.end(), source) != function.sources.end();
}

void add_source(DiscoveredFunction& function, DiscoverySource source) {
  if (!source_contains(function, source)) function.sources.push_back(source);
}

void add_source(std::vector<DiscoverySource>& sources, DiscoverySource source) {
  if (std::find(sources.begin(), sources.end(), source) == sources.end()) sources.push_back(source);
}

[[nodiscard]] bool source_is_semantic_boundary(DiscoverySource source) noexcept {
  switch (source) {
    case DiscoverySource::EntryPoint:
    case DiscoverySource::Export:
    case DiscoverySource::DirectCall:
    case DiscoverySource::UnwindMetadata:
    case DiscoverySource::ModuleHint:
    case DiscoverySource::TlsCallback:
    case DiscoverySource::ResolvedIndirectCall:
      return true;
    case DiscoverySource::ResolvedIndirect:
    case DiscoverySource::ResolvedIndirectBranch:
    case DiscoverySource::ControlFlowHint:
    case DiscoverySource::DirectBranch:
    case DiscoverySource::ValidatedTailCall:
    case DiscoverySource::PrologueHeuristic:
    case DiscoverySource::PointerTable:
    case DiscoverySource::RuntimeObservation:
    case DiscoverySource::GapRecovery:
    case DiscoverySource::KnowledgeMatch:
      return false;
  }
  return false;
}

[[nodiscard]] bool has_semantic_boundary_evidence(const DiscoveredFunction& function) noexcept {
  return std::any_of(function.sources.begin(), function.sources.end(), source_is_semantic_boundary);
}

[[nodiscard]] FunctionAuthority authority_for_sources(
    const std::vector<DiscoverySource>& sources) noexcept {
  FunctionAuthority authority = FunctionAuthority::InferredBranch;
  for (const auto source : sources) {
    FunctionAuthority candidate = FunctionAuthority::InferredBranch;
    switch (source) {
      case DiscoverySource::EntryPoint: candidate = FunctionAuthority::EntryPoint; break;
      case DiscoverySource::ModuleHint: candidate = FunctionAuthority::ModuleHint; break;
      case DiscoverySource::UnwindMetadata:
      case DiscoverySource::TlsCallback:
      case DiscoverySource::Export: candidate = FunctionAuthority::Metadata; break;
      case DiscoverySource::DirectCall:
      case DiscoverySource::ResolvedIndirectCall:
        candidate = FunctionAuthority::DirectCall;
        break;
      case DiscoverySource::ResolvedIndirect:
      case DiscoverySource::ResolvedIndirectBranch:
      case DiscoverySource::ControlFlowHint:
        candidate = FunctionAuthority::ResolvedIndirect;
        break;
      case DiscoverySource::PointerTable: candidate = FunctionAuthority::PointerTable; break;
      case DiscoverySource::RuntimeObservation: candidate = FunctionAuthority::RuntimeObservation; break;
      case DiscoverySource::GapRecovery: candidate = FunctionAuthority::GapRecovery; break;
      case DiscoverySource::KnowledgeMatch: candidate = FunctionAuthority::InferredBranch; break;
      case DiscoverySource::DirectBranch:
      case DiscoverySource::ValidatedTailCall:
      case DiscoverySource::PrologueHeuristic: candidate = FunctionAuthority::InferredBranch; break;
    }
    if (static_cast<unsigned>(candidate) > static_cast<unsigned>(authority)) authority = candidate;
  }
  return authority;
}

}  // namespace detail

}  // namespace xenon::recomp

#include "recomp/analysis/validation/codegen_validation.hpp"

#include <algorithm>
#include <set>
#include <unordered_map>

#include "recomp/analysis/analysis_internal.hpp"
#include "recomp/driver/driver_support.hpp"
#include "xenon/cpu/backend/cpp_aot.hpp"

namespace xenon::recomp::detail {

// Part 2/3/9 (generated-code deduplication / shard ownership fix):
// pre-emission validation. The hard invariant this whole pass exists to
// enforce is ONE guest address -> ONE canonical function -> ONE emitted C++
// definition. `codegen_items` is already guaranteed address-unique by this
// point (report.functions was deduplicated by guest_start in
// load_and_analyze() - see deduplicate_functions_by_address() - well before
// generate_project() ever runs), so the address-collision branch below is a
// second, independent check of the same invariant right before it actually
// matters (belt-and-suspenders, per Part 9's "do not silently discard
// conflicting function definitions"). The symbol-collision branch catches
// the one class of conflict address-uniqueness alone can't: two DIFFERENT
// addresses whose generated names collide (a hint-file authoring bug, since
// Xenon derives every other name deterministically from its own address).
// Per Part 13, a genuine name collision must fail codegen loudly - never be
// silently renamed, suppressed, or worked around - so a real conflict here
// is reported and generate_project() returns false before any C++ is
// written for this run.
bool validate_codegen_uniqueness(const std::vector<const DiscoveredFunction*>& codegen_items,
                                 const AnalysisReport& report, AnalysisDiagnostics& diagnostics,
                                 std::string& error) {
  std::unordered_map<GuestAddress, const DiscoveredFunction*> by_address;
  std::unordered_map<std::string, const DiscoveredFunction*> by_symbol;
  by_address.reserve(codegen_items.size());
  by_symbol.reserve(codegen_items.size() * 3u);
  for (const auto* function : codegen_items) {
    if (const auto existing = by_address.find(function->guest_start); existing != by_address.end()) {
      error = "codegen: guest address 0x" + hex_string(function->guest_start) +
              " has more than one canonical function record ('" + existing->second->name + "' and '" +
              function->name +
              "') reaching code generation - this indicates a bug earlier in the analysis pipeline, not a "
              "legitimate alias";
      return false;
    }
    by_address.emplace(function->guest_start, function);
    const std::string symbols[] = {function->name, function->name + "_v2", function->name + "_dispatch_v2"};
    for (const auto& symbol : symbols) {
      if (const auto existing = by_symbol.find(symbol); existing != by_symbol.end()) {
        ++diagnostics.codegen_duplicate_symbols_rejected;
        error = "codegen: generated symbol '" + symbol + "' would be emitted for both guest address 0x" +
                hex_string(existing->second->guest_start) + " (first owner, name '" + existing->second->name +
                "') and 0x" + hex_string(function->guest_start) + " (second attempted owner, name '" +
                function->name +
                "') - refusing to emit conflicting C++ definitions under one symbol name; check the "
                "module hint data for two different addresses assigned the same name";
        return false;
      }
      by_symbol.emplace(symbol, function);
    }
  }
  std::set<GuestAddress> entry_addresses;
  for (const auto& entry : report.entries) {
    if (!entry_addresses.insert(entry.address).second) {
      error = "codegen: guest entry address 0x" + hex_string(entry.address) +
              " appears more than once after entry reconciliation";
      return false;
    }
    if (entry.kind != GuestEntryKind::AlternateBlock) continue;
    const auto owner = std::find_if(codegen_items.begin(), codegen_items.end(), [&](const auto* function) {
      return function->guest_start == entry.owner_function;
    });
    if (owner == codegen_items.end()) {
      error = "codegen: alternate entry 0x" + hex_string(entry.address) +
              " names missing canonical owner 0x" + hex_string(entry.owner_function);
      return false;
    }
    const auto symbol = cpu::backend::alternate_entry_symbol((*owner)->name, entry.address);
    if (const auto existing = by_symbol.find(symbol); existing != by_symbol.end()) {
      ++diagnostics.codegen_duplicate_symbols_rejected;
      error = "codegen: alternate-entry symbol '" + symbol +
              "' collides with generated symbol owned by guest function 0x" +
              hex_string(existing->second->guest_start);
      return false;
    }
    by_symbol.emplace(symbol, *owner);
  }
  diagnostics.codegen_input_functions = codegen_items.size();
  diagnostics.codegen_unique_functions = by_address.size();
  return true;
}

bool validate_codegen_control_flow(const std::vector<const DiscoveredFunction*>& functions,
                                   const AnalysisReport& report, std::string& error) {
  std::set<GuestAddress> dispatchable_entries;
  for (const auto& entry : report.entries) dispatchable_entries.insert(entry.address);

  for (const auto* function : functions) {
    for (const auto& block : function->ir.blocks) {
      for (const auto& edge : block.successors) {
        if (edge.local || edge.kind == cpu::ir::EdgeKind::Call) continue;
        if (dispatchable_entries.contains(edge.target)) continue;
        error = "codegen: direct " +
                std::string(edge.kind == cpu::ir::EdgeKind::Fallthrough ? "fallthrough" : "branch") +
                " from guest function 0x" + hex_string(function->guest_start) +
                " targets 0x" + hex_string(edge.target) +
                " without a materialized local CFG block or dispatchable compiled entry";
        return false;
      }
    }
  }
  return true;
}

bool validate_region_entry_integrity(const AnalysisReport& report,
                                     AnalysisDiagnostics& diagnostics,
                                     std::string& error) {
  std::unordered_map<GuestAddress, const DiscoveredFunction*> functions_by_start;
  functions_by_start.reserve(report.functions.size());
  for (const auto& function : report.functions)
    if (function.compiled) functions_by_start.emplace(function.guest_start, &function);

  std::set<GuestAddress> entries;
  for (const auto& entry : report.entries) {
    ++diagnostics.entry_integrity_checks;
    if (!entries.insert(entry.address).second) {
      ++diagnostics.entry_integrity_failures;
      error = "entry-integrity: duplicate guest entry 0x" + hex_string(entry.address);
      return false;
    }
    if (entry.kind == GuestEntryKind::RuntimeHelper || entry.kind == GuestEntryKind::ImportThunk) continue;

    const auto owner_it = functions_by_start.find(entry.owner_function);
    if (owner_it == functions_by_start.end()) {
      ++diagnostics.entry_integrity_failures;
      error = "entry-integrity: entry 0x" + hex_string(entry.address) +
              " names missing compiled owner 0x" + hex_string(entry.owner_function);
      return false;
    }
    const auto& owner = *owner_it->second;
    if (entry.kind == GuestEntryKind::AlternateBlock) {
      if (entry.block != entry.address || owner.native_replacement ||
          !owns_address(owner, entry.address) || !has_block_entry(owner, entry.address)) {
        ++diagnostics.entry_integrity_failures;
        error = "entry-integrity: alternate entry 0x" + hex_string(entry.address) +
                " is not a materialized block of canonical owner 0x" +
                hex_string(owner.guest_start);
        return false;
      }
    } else if (entry.address != owner.guest_start ||
               entry.owner_function != owner.guest_start) {
      ++diagnostics.entry_integrity_failures;
      error = "entry-integrity: semantic/native entry 0x" + hex_string(entry.address) +
              " does not match its canonical owner";
      return false;
    }
  }

  // Every non-linked executable transfer that leaves its canonical owner must
  // land on a published entry. This is intentionally address-centric (like
  // Xenia/N64Recomp runtime lookup) rather than assuming every legal target is
  // a separate semantic function.
  for (const auto& function : report.functions) {
    if (!function.compiled || function.native_replacement) continue;
    for (const auto& branch : function.branches) {
      if (branch.linked) continue;
      ++diagnostics.entry_integrity_checks;
      if (owns_address(function, branch.target)) continue;
      if (entries.contains(branch.target)) continue;
      ++diagnostics.entry_integrity_failures;
      error = "entry-integrity: control-flow edge 0x" + hex_string(branch.site) +
              " -> 0x" + hex_string(branch.target) +
              " leaves owner 0x" + hex_string(function.guest_start) +
              " without a dispatchable guest entry";
      return false;
    }
  }
  return true;
}

}  // namespace xenon::recomp::detail

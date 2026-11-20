#include <algorithm>
#include <set>
#include <span>

#include "recomp/compilation/project_generation_internal.hpp"
#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/recomp/worker_pool.hpp"

namespace xenon::recomp::detail {

AlternateEntriesByOwner collect_alternate_entries(const AnalysisReport& report) {
  std::map<GuestAddress, std::vector<GuestAddress>> alternate_entries_by_owner;
  for (const auto& entry : report.entries) {
    if (entry.kind != GuestEntryKind::AlternateBlock) continue;
    alternate_entries_by_owner[entry.owner_function].push_back(entry.address);
  }
  for (auto& [owner, entries] : alternate_entries_by_owner) {
    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
  }
  return alternate_entries_by_owner;
}

// Each item's shard membership is a pure function of its position in this
// already-deterministic (guest-address-sorted) list, decided up front -
// never by which worker happens to finish which item first (Part 16's
// "stable sorted function addresses determine shard membership").
// Direct-call symbols: the cases registry.cpp's lookup_compiled() returns
// (generated_registry.cpp), so calling the symbol directly reaches the same
// code as the lookup without a lookup per call. The register save/restore
// helpers are header templates the compiler can inline into the call site:
// AC6 made 5,574 calls into them through the lookup, and its audio mixer
// spent 3.4% in the restore helper alone.
DirectCallSymbols build_direct_call_symbols(
    const std::vector<const DiscoveredFunction*>& codegen_items,
    const AlternateEntriesByOwner& alternate_entries_by_owner, const AnalysisReport& report) {
  DirectCallSymbols direct_call_symbols;
  const auto bind = [&](GuestAddress address, std::string symbol, std::string header = {},
                        bool leaf = false) {
    direct_call_symbols.emplace(
        address, cpu::backend::DirectCallBinding{address, std::move(symbol), std::move(header), leaf});
  };
  for (const auto* function : codegen_items) bind(function->guest_start, function->name + "_v2");
  for (const auto& [owner, entries] : alternate_entries_by_owner) {
    const auto owner_it = std::find_if(codegen_items.begin(), codegen_items.end(),
                                       [&](const auto* function) { return function->guest_start == owner; });
    if (owner_it == codegen_items.end()) continue;
    for (const auto entry : entries)
      bind(entry, cpu::backend::alternate_entry_symbol((*owner_it)->name, entry));
  }
  if (report.hint_set_v2) {
    for (const auto& helper : analysis::expand_runtime_helpers(report.hint_set_v2->runtime_helpers)) {
      // setjmp/longjmp rewind guest frames; they keep the lookup and are
      // never tail-called.
      if (helper.kind == analysis::RuntimeHelperKind::SetJmp ||
          helper.kind == analysis::RuntimeHelperKind::LongJmp)
        continue;
      bind(helper.address, "xenon::recomp::runtime_helpers::" + runtime_helper_native_symbol(helper),
           "xenon/recomp/runtime_helpers.hpp", true);
    }
  }
  for (const auto& function : report.functions) {
    if (!function.native_replacement) continue;
    if (const auto* name = native_replacement_function_name(*function.native_replacement))
      bind(function.guest_start, std::string("xenon::recomp::native_replacements::") + name,
           "xenon/recomp/native_replacements.hpp");
  }
  return direct_call_symbols;
}

EmittedFunctionSources emit_function_sources(
    const std::vector<const DiscoveredFunction*>& codegen_items,
    const AlternateEntriesByOwner& alternate_entries_by_owner,
    const DirectCallSymbols& direct_call_symbols,
    std::span<const std::string> hook_symbols,
    const graph::Store& graph_store, const DriverOptions& options) {
  EmittedFunctionSources emitted;
  const auto worker_count = resolve_worker_count(options.codegen_jobs);
  const WorkerPool pool(worker_count);
  emitted.worker_count = worker_count;
  auto& function_sources = emitted.sources;
  auto& source_nodes = emitted.nodes;
  auto& source_hits = emitted.hits;
  function_sources.resize(codegen_items.size());
  source_nodes.resize(codegen_items.size());
  source_hits.resize(codegen_items.size());
  pool.parallel_for(codegen_items.size(), [&](std::size_t i) {
    // cpu::backend::CppAotBackend is stateless (no members, every method
    // const - see include/xenon/cpu/backend/cpp_aot.hpp), so a fresh
    // instance per call is both correct and cheap; sharing one instance
    // across workers would be equally safe but this avoids any doubt.
    cpu::backend::CppAotBackend backend;
    const auto& function = *codegen_items[i];
    const auto aliases_it = alternate_entries_by_owner.find(function.guest_start);
    const std::span<const GuestAddress> alternate_entries =
        aliases_it == alternate_entries_by_owner.end()
            ? std::span<const GuestAddress>{}
            : std::span<const GuestAddress>(aliases_it->second);
    std::string aliases;
    for (auto entry : alternate_entries) aliases += std::to_string(entry) + ";";
    std::vector<cpu::backend::DirectCallBinding> direct_calls;
    std::string direct_call_key;
    std::set<GuestAddress> bound;
    const auto bind = [&](GuestAddress target, bool tail_branch) {
      const auto symbol_it = direct_call_symbols.find(target);
      if (symbol_it == direct_call_symbols.end()) return;
      // A tail branch is bound only to a leaf runtime helper (see
      // DirectCallBinding::leaf); every other one stays a Branch result.
      if (tail_branch && !symbol_it->second.leaf) return;
      if (!bound.insert(target).second) return;
      direct_calls.push_back(symbol_it->second);
      direct_call_key += std::to_string(target) + "=" + symbol_it->second.native_symbol +
                         (symbol_it->second.leaf ? "!" : "") + ";";
    };
    for (const auto target : cpu::backend::CppAotBackend::static_call_targets(function.ir))
      bind(target, false);
    for (const auto target : cpu::backend::CppAotBackend::static_tail_branch_targets(function.ir))
      bind(target, true);
    std::sort(direct_calls.begin(), direct_calls.end(),
              [](const auto& a, const auto& b) { return a.guest_target < b.guest_target; });
    // The routine names bound to this function's hook ordinals.
    std::set<std::uint64_t> hook_ordinals;
    for (const auto& block : function.ir.blocks)
      for (const auto& instruction : block.instructions)
        if (instruction.op == cpu::ir::Op::HostHook) hook_ordinals.insert(instruction.imm0);
    std::string hook_key;
    for (const auto ordinal : hook_ordinals)
      hook_key += std::to_string(ordinal) + "=" +
                  (ordinal < hook_symbols.size() ? hook_symbols[ordinal] : std::string("?")) + ";";
    // Exact canonical IR, emitted symbol and aliases are all backend inputs.
    // Whole-image configuration/knowledge identity is intentionally absent.
    graph::Node node{"source", options.graph_versions.codegen,
        {{"ir", graph::digest(graph::serialize_ir(function.ir))},
         {"symbol", function.name}, {"aliases", aliases},
         {"direct_calls", direct_call_key},
         {"producer_build", graph::producer_identity("source")},
         {"cpu_semantics", options.graph_versions.semantics}}, {}};
    // Absent without hooks, so hook-free functions keep their keys.
    if (!hook_key.empty()) node.inputs.emplace("hooks", hook_key);
    // Output-based dependency permits cutoff when analysis changes but IR does not.
    node.dependencies.push_back(graph::digest(graph::serialize_ir(function.ir)));
    source_nodes[i] = node;
    auto cached = graph_store.lookup(node);
    std::string function_source;
    if (cached) { function_source = std::move(cached->bytes); source_hits[i] = 1; }
    else {
      function_source = backend.emit_translation_unit(function.ir, function.name, direct_calls,
                                                      alternate_entries, hook_symbols);
      (void)graph_store.publish(node, function_source);
    }
    function_sources[i] = std::move(function_source);
  });
  return emitted;
}

}  // namespace xenon::recomp::detail

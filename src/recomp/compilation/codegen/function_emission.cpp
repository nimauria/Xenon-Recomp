#include <algorithm>
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
// Direct-call symbols: exactly the compiled-function and alternate-entry
// cases registry.cpp's lookup_compiled() returns below (runtime helpers and
// native replacements keep the registry lookup). The generated registry is
// a static switch bound as ExecutionContext::compiled_lookup, so calling the
// symbol directly reaches the same code without a runtime lookup per call.
std::unordered_map<GuestAddress, std::string> build_direct_call_symbols(
    const std::vector<const DiscoveredFunction*>& codegen_items,
    const AlternateEntriesByOwner& alternate_entries_by_owner) {
  std::unordered_map<GuestAddress, std::string> direct_call_symbols;
  for (const auto* function : codegen_items)
    direct_call_symbols.emplace(function->guest_start, function->name + "_v2");
  for (const auto& [owner, entries] : alternate_entries_by_owner) {
    const auto owner_it = std::find_if(codegen_items.begin(), codegen_items.end(),
                                       [&](const auto* function) { return function->guest_start == owner; });
    if (owner_it == codegen_items.end()) continue;
    for (const auto entry : entries)
      direct_call_symbols.emplace(entry, cpu::backend::alternate_entry_symbol((*owner_it)->name, entry));
  }
  return direct_call_symbols;
}

EmittedFunctionSources emit_function_sources(
    const std::vector<const DiscoveredFunction*>& codegen_items,
    const AlternateEntriesByOwner& alternate_entries_by_owner,
    const std::unordered_map<GuestAddress, std::string>& direct_call_symbols,
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
    for (const auto target : cpu::backend::CppAotBackend::static_call_targets(function.ir)) {
      const auto symbol_it = direct_call_symbols.find(target);
      if (symbol_it == direct_call_symbols.end()) continue;
      direct_calls.push_back({target, symbol_it->second});
      direct_call_key += std::to_string(target) + "=" + symbol_it->second + ";";
    }
    // Exact canonical IR, emitted symbol and aliases are all backend inputs.
    // Whole-image configuration/knowledge identity is intentionally absent.
    graph::Node node{"source", options.graph_versions.codegen,
        {{"ir", graph::digest(graph::serialize_ir(function.ir))},
         {"symbol", function.name}, {"aliases", aliases},
         {"direct_calls", direct_call_key},
         {"producer_build", graph::producer_identity("source")},
         {"cpu_semantics", options.graph_versions.semantics}}, {}};
    // Output-based dependency permits cutoff when analysis changes but IR does not.
    node.dependencies.push_back(graph::digest(graph::serialize_ir(function.ir)));
    source_nodes[i] = node;
    auto cached = graph_store.lookup(node);
    std::string function_source;
    if (cached) { function_source = std::move(cached->bytes); source_hits[i] = 1; }
    else {
      function_source = backend.emit_translation_unit(function.ir, function.name, direct_calls,
                                                      alternate_entries);
      (void)graph_store.publish(node, function_source);
    }
    function_sources[i] = std::move(function_source);
  });
  return emitted;
}

}  // namespace xenon::recomp::detail

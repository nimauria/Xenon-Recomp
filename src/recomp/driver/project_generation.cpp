#include <algorithm>
#include <chrono>

#include "recomp/analysis/validation/codegen_validation.hpp"
#include "recomp/compilation/project_generation_internal.hpp"

namespace xenon::recomp {

// Generated project emission. Phases:
//   validation      analysis/validation/codegen_validation.cpp
//   emission        compilation/codegen/function_emission.cpp
//   shards          compilation/shards/shard_assembly.cpp
//   root files      compilation/project/generated_*.cpp (staged)
//   promotion       compilation/project/generation_staging.cpp
bool generate_project(const DriverOptions& options, AnalysisReport& report, std::string& error) {
  using namespace detail;
  const auto codegen_start = std::chrono::steady_clock::now();
  std::error_code ec;
  std::filesystem::create_directories(options.output / "functions", ec);
  if (ec) { error = "unable to create output directory: " + ec.message(); return false; }

  const auto generation_status_path = options.output / "generation-status.json";
  const auto generation_stage = options.output / ".generation-stage";
  const auto write_generation_status = [&](bool complete, std::string_view phase,
                                           std::string_view failure) {
    write_generation_status_file(generation_status_path, report.configuration_hash, complete, phase,
                                 failure);
  };
  const auto fail_generation = [&](std::string_view phase) {
    write_generation_status(false, phase, error);
    std::error_code cleanup_ec;
    std::filesystem::remove_all(generation_stage, cleanup_ec);
    return false;
  };

  // Root project metadata is transactional. Function shards and the content
  // cache may be written eagerly, but they are unreachable until the staged
  // registry/CMake/manifest set is promoted. `generation-status.json` is
  // written incomplete before validation so a failed run can never make a
  // stale analysis.json look like the result of the current generator.
  write_generation_status(false, "validating", {});
  std::filesystem::remove_all(generation_stage, ec);
  ec.clear();
  std::filesystem::create_directories(generation_stage, ec);
  if (ec) {
    error = "unable to create generation staging directory: " + ec.message();
    return fail_generation("staging");
  }
  const graph::Store graph_store(options.graph_cache.empty() ? options.output / ".graph" : options.graph_cache);
  // Phase K (Part 2/16): parallel native code generation. Native-replacement
  // entries have no IR/guest bytes to emit (only a registry.cpp
  // lookup_compiled() case, handled below), so they never occupy a codegen
  // slot or count toward shard membership - matches the previous serial
  // loop's `continue` exactly.
  std::vector<const DiscoveredFunction*> codegen_items;
  codegen_items.reserve(report.functions.size());
  for (const auto& function : report.functions)
    if (function.compiled && !function.native_replacement) codegen_items.push_back(&function);

  if (!validate_codegen_uniqueness(codegen_items, report, report.diagnostics, error))
    return fail_generation("entry-uniqueness");
  if (!validate_region_entry_integrity(report, report.diagnostics, error))
    return fail_generation("entry-integrity");
  if (!validate_codegen_control_flow(codegen_items, report, error))
    return fail_generation("control-flow");
  write_generation_status(false, "codegen", {});

  const auto alternate_entries_by_owner = collect_alternate_entries(report);

  if (options.progress)
    options.progress("[Xenon Recomp] Codegen: " + std::to_string(codegen_items.size()) + " functions");

  const auto direct_call_symbols = build_direct_call_symbols(codegen_items, alternate_entries_by_owner);
  const auto emitted = emit_function_sources(codegen_items, alternate_entries_by_owner,
                                             direct_call_symbols, graph_store, options);
  const auto& function_sources = emitted.sources;

  if (options.progress) options.progress("[Codegen] " + std::to_string(function_sources.size()) + " functions emitted, writing shards...");

  const auto shard_layout = assemble_shards(codegen_items, function_sources, options);
  const auto& shards = shard_layout.paths;
  report.diagnostics.codegen_shards = shards.size();
  report.diagnostics.codegen_max_functions_per_shard = shard_layout.max_functions_per_shard;
  report.diagnostics.codegen_max_shard_bytes = shard_layout.max_shard_bytes;
  if (options.progress) options.progress("[Codegen] " + std::to_string(shards.size()) + " source shards");

  // Every staged root file is written (and closed) even if an earlier one
  // failed; a single failure then fails the generation before promotion.
  bool staged = write_registry_header(generation_stage);
  staged = write_registry_source(generation_stage, report) && staged;
  staged = write_import_manifest(generation_stage, report) && staged;
  staged = write_generated_metadata(generation_stage, report) && staged;
  staged = write_generated_hooks(generation_stage, options, report) && staged;
  staged = write_analysis_json(generation_stage, report) && staged;
  staged = write_shard_manifest(generation_stage, report, shards) && staged;
  remove_stale_shards(options.output / "functions", shards);
  staged = write_module_export(generation_stage, report) && staged;
  staged = write_generated_cmake(generation_stage, options, shards) && staged;
  if (!staged) {
    error = "failed while writing staged generated project metadata";
    return fail_generation("staging-write");
  }

  write_generation_status(false, "promoting", {});
  if (!promote_staged_project(generation_stage, options.output, error))
    return fail_generation("promoting");
  std::filesystem::remove_all(generation_stage, ec);

  report.diagnostics.codegen_duration_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - codegen_start)
          .count();
  write_generation_status(true, "complete", {});
  // Deterministic topology is separate from run-specific hit/timing diagnostics.
  if (!write_compilation_graph_manifest(options.output, codegen_items, emitted, options.progress, error))
    return false;
  if (options.progress) options.progress("[Codegen] complete");
  return true;
}

}  // namespace xenon::recomp

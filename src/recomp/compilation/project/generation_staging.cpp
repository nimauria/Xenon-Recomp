#include <array>
#include <fstream>
#include <iterator>

#include "recomp/compilation/project_generation_internal.hpp"
#include "recomp/driver/driver_support.hpp"

namespace xenon::recomp::detail {

// generation-status.json is written incomplete before validation so a failed
// run can never make a stale analysis.json look current.
void write_generation_status_file(const std::filesystem::path& generation_status_path,
                                  std::uint64_t configuration_hash, bool complete,
                                  std::string_view phase, std::string_view failure) {
  std::ofstream status(generation_status_path, std::ios::trunc);
  if (!status) return;
  status << "{\n"
         << "  \"status_schema_version\": 1,\n"
         << "  \"report_schema_version\": " << kAnalysisReportSchemaVersion << ",\n"
         << "  \"analysis_engine_revision\": " << kAnalysisEngineRevision << ",\n"
         << "  \"configuration_hash\": " << configuration_hash << ",\n"
         << "  \"complete\": " << (complete ? "true" : "false") << ",\n"
         << "  \"phase\": \"" << json_escape(std::string(phase)) << "\",\n"
         << "  \"error\": \"" << json_escape(std::string(failure)) << "\"\n"
         << "}\n";
}

// Copies every staged root file over the live project, skipping unchanged
// files so an unchanged generation does not dirty the nested build.
bool promote_staged_project(const std::filesystem::path& generation_stage,
                            const std::filesystem::path& output, std::string& error) {
  std::error_code ec;
  const std::array<const char*, 9> staged_root_files = {
      "registry.hpp", "registry.cpp", "imports.cpp", "metadata.cpp", "hooks.cpp",
      "analysis.json", "manifest.txt", "module_export.cpp", "CMakeLists.txt"};
  for (const auto* file_name : staged_root_files) {
    const auto source = generation_stage / file_name;
    const auto destination = output / file_name;
    ec.clear();
    std::ifstream previous_file(destination, std::ios::binary), next_file(source, std::ios::binary);
    const std::string previous((std::istreambuf_iterator<char>(previous_file)), {});
    const std::string next((std::istreambuf_iterator<char>(next_file)), {});
    previous_file.close(); next_file.close();
    if (previous != next || !std::filesystem::exists(destination))
      std::filesystem::copy_file(source, destination,
                                 std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      error = std::string("unable to promote generated ") + file_name + ": " + ec.message();
      return false;
    }
  }
  return true;
}

bool write_compilation_graph_manifest(const std::filesystem::path& output,
                                      const std::vector<const DiscoveredFunction*>& codegen_items,
                                      const EmittedFunctionSources& emitted,
                                      const std::function<void(const std::string&)>& progress,
                                      std::string& error) {
  const auto& source_nodes = emitted.nodes;
  const auto& source_hits = emitted.hits;
  const auto& function_sources = emitted.sources;
  const auto worker_count = emitted.worker_count;
  std::ofstream graph_manifest(output / "compilation-graph.json", std::ios::binary);
  graph_manifest << "{\"schema\":1,\"regions\":[";
  std::size_t hits = 0, ir_hits = 0, reused_bytes = 0;
  for (std::size_t i=0;i<source_nodes.size();++i) {
    if(i) graph_manifest << ',';
    graph_manifest << "{\"address\":" << codegen_items[i]->guest_start << ",\"nodes\":[";
    for (const auto& n : codegen_items[i]->compilation_nodes) graph_manifest << n.canonical() << ',';
    graph_manifest << source_nodes[i].canonical() << "]}";
    hits += source_hits[i]; ir_hits += codegen_items[i]->ir_cache_hit;
    if(source_hits[i]) reused_bytes += function_sources[i].size();
  }
  graph_manifest << "]}"; graph_manifest.close();
  if (!graph_manifest) { error="failed to write compilation graph"; return false; }
  if(progress) progress("[Graph] IR hits="+std::to_string(ir_hits)+
      " source hits="+std::to_string(hits)+" misses="+std::to_string(source_nodes.size()-hits)+
      " reused bytes="+std::to_string(reused_bytes)+" workers="+std::to_string(worker_count));
  return true;
}

}  // namespace xenon::recomp::detail

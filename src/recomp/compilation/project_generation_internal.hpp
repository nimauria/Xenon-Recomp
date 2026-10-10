#pragma once

// Phases of generate_project() (driver/project_generation.cpp): native C++
// emission, shard assembly, the staged root project files, and transactional
// promotion. Private to xenon_recomp.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "xenon/recomp/compilation_graph.hpp"
#include "xenon/recomp/driver.hpp"

namespace xenon::recomp::detail {
using cpu::GuestAddress;

// --- Registry symbols (compilation/project/generated_registry.cpp) --------
// Text of the xenon::recomp::runtime_helpers:: symbol a given (already
// expanded, i.e. concrete-address) RuntimeHelper dispatches to.
// Register-range kinds are function templates parameterized on the
// concrete register_start (Part 1.5/1.6) - the driver bakes that value in
// as a literal template argument in the generated source text rather than
// emitting one hand-written function per possible register count.
std::string runtime_helper_native_symbol(const analysis::RuntimeHelper& helper);
// Qualified xenon::recomp::native_replacements function name for a kind that
// native_replacements::entry_for() actually implements - used to emit a
// direct call in the generated lookup_compiled() switch. Must be kept in
// sync with native_replacements.cpp's entry_for(); only ever invoked for a
// kind entry_for() returns non-null for (callers check that first).
const char* native_replacement_function_name(analysis::NativeReplacementKind kind);

// --- Native function emission (compilation/codegen/function_emission.cpp) --
using AlternateEntriesByOwner = std::map<GuestAddress, std::vector<GuestAddress>>;

// Sorted, de-duplicated alternate-block entries grouped by canonical owner.
AlternateEntriesByOwner collect_alternate_entries(const AnalysisReport& report);
std::unordered_map<GuestAddress, std::string> build_direct_call_symbols(
    const std::vector<const DiscoveredFunction*>& codegen_items,
    const AlternateEntriesByOwner& alternate_entries_by_owner);

struct EmittedFunctionSources {
  std::size_t worker_count{};
  std::vector<std::string> sources;
  std::vector<graph::Node> nodes;
  std::vector<unsigned> hits;
};
EmittedFunctionSources emit_function_sources(
    const std::vector<const DiscoveredFunction*>& codegen_items,
    const AlternateEntriesByOwner& alternate_entries_by_owner,
    const std::unordered_map<GuestAddress, std::string>& direct_call_symbols,
    const graph::Store& graph_store, const DriverOptions& options);

// --- Shards (compilation/shards/shard_assembly.cpp) ------------------------
struct ShardLayout {
  std::vector<std::filesystem::path> paths;
  std::size_t max_functions_per_shard{};
  std::size_t max_shard_bytes{};
};
ShardLayout assemble_shards(const std::vector<const DiscoveredFunction*>& codegen_items,
                            const std::vector<std::string>& function_sources,
                            const DriverOptions& options);
void remove_stale_shards(const std::filesystem::path& function_directory,
                         const std::vector<std::filesystem::path>& shards);

// --- Staged root project files ---------------------------------------------
// Each writes one file into the generation staging directory and reports
// whether the stream stayed good through close().
// (compilation/project/generated_registry.cpp)
bool write_registry_header(const std::filesystem::path& generation_stage);
bool write_registry_source(const std::filesystem::path& generation_stage, const AnalysisReport& report);
// (compilation/project/generated_project_files.cpp)
bool write_import_manifest(const std::filesystem::path& generation_stage, const AnalysisReport& report);
bool write_generated_metadata(const std::filesystem::path& generation_stage, const AnalysisReport& report);
bool write_generated_hooks(const std::filesystem::path& generation_stage, const DriverOptions& options,
                           const AnalysisReport& report);
bool write_analysis_json(const std::filesystem::path& generation_stage, const AnalysisReport& report);
bool write_shard_manifest(const std::filesystem::path& generation_stage, const AnalysisReport& report,
                          const std::vector<std::filesystem::path>& shards);
bool write_module_export(const std::filesystem::path& generation_stage, const AnalysisReport& report);
bool write_generated_cmake(const std::filesystem::path& generation_stage, const DriverOptions& options,
                           const std::vector<std::filesystem::path>& shards);

// --- Staging and promotion (compilation/project/generation_staging.cpp) ----
void write_generation_status_file(const std::filesystem::path& generation_status_path,
                                  std::uint64_t configuration_hash, bool complete,
                                  std::string_view phase, std::string_view failure);
bool promote_staged_project(const std::filesystem::path& generation_stage,
                            const std::filesystem::path& output, std::string& error);
bool write_compilation_graph_manifest(const std::filesystem::path& output,
                                      const std::vector<const DiscoveredFunction*>& codegen_items,
                                      const EmittedFunctionSources& emitted,
                                      const std::function<void(const std::string&)>& progress,
                                      std::string& error);

}  // namespace xenon::recomp::detail

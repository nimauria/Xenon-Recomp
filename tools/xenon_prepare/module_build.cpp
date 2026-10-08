// Preparing one XEX module: analysis, source generation, the nested CMake
// build and publication into the artifact cache.

#include "prepare_internal.hpp"

namespace xenon::prepare_tool {

ModuleBuildResult prepare_one_module(const xenon::xbox::XexImage& effective_image,
                                     const std::optional<xenon::recomp::analysis::AnalysisHintSetV2>& hint_set,
                                     const std::vector<xenon::recomp::AdaptiveObservation>& adaptive_observations,
                                     const std::vector<xenon::recomp::KnowledgeRecord>& knowledge_records,
                                     const xenon::recomp::ArtifactCacheKey& key,
                                     xenon::recomp::ArtifactCacheStore& store, const Options& options,
                                     const std::string& title_id_hex, const std::string& media_id_hex,
                                     const std::string& module_relative_path, bool is_primary,
                                     StatusReporter& status) {
  ModuleBuildResult result;

  if (cancellation_requested(options.stop_signal)) {
    status.report(Phase::Cancelled, 0, "Cancelled before analysis began");
    result.exit_code = 7;
    result.error = "cancelled";
    return result;
  }

  // --- Phase: AnalyzingExecutable ------------------------------------------
  status.report(Phase::AnalyzingExecutable, 15,
                is_primary ? "Discovering functions and applying analysis hints"
                           : "Discovering functions and applying analysis hints (" +
                                 module_relative_path + ")");
  xenon::recomp::DriverOptions driver_options;
  driver_options.graph_cache = options.cache_root / "graph-v1";
  driver_options.shard_function_count = 1;
  driver_options.pre_parsed_image = effective_image;
  driver_options.hint_set_v2 = hint_set;
  driver_options.adaptive_observations = adaptive_observations;
  driver_options.knowledge_records = knowledge_records;
  // Progress milestones (Part 18 of the Recomp Analysis V2 pass): a large
  // real-title analysis used to leave this tool's status file (and so any
  // UI reading it, e.g. the launcher) frozen at "15%" for the entire
  // discovery/compile pass with no visible movement. Forwarding the
  // driver's own progress strings as the task text - at the phase's
  // existing percent, which analysis/codegen alone do not otherwise
  // subdivide - at least proves forward progress is happening and shows
  // live function/wave counts, worker counts, etc.
  driver_options.progress = [&status](const std::string& message) {
    status.report(Phase::AnalyzingExecutable, 15, message);
  };

  xenon::recomp::AnalysisReport report;
  std::string driver_error;
  if (!xenon::recomp::load_and_analyze(driver_options, report, driver_error)) {
    status.report(Phase::Failed, 15, "Analyzing executable", driver_error);
    result.exit_code = 3;
    result.error = driver_error;
    return result;
  }

  if (cancellation_requested(options.stop_signal)) {
    status.report(Phase::Cancelled, 15, "Cancelled after analysis");
    result.exit_code = 7;
    result.error = "cancelled";
    return result;
  }

  // --- Phase: GeneratingSource ---------------------------------------------
  status.report(Phase::GeneratingSource, 40, "Generating native C++ source");
  driver_options.progress = [&status](const std::string& message) {
    status.report(Phase::GeneratingSource, 40, message);
  };
  auto staging = store.begin_staging(key);
  // Title/revision is not an object-cache identity. Workspace is merely the
  // stable CMake build location, exclusively held until publication ends.
  // Gen 11: the relative path is part of this key too, since two different
  // XEX modules of the same title each need their own independent nested
  // CMake build/generated project, never sharing one workspace directory.
  const auto workspace_key = xenon::recomp::graph::digest(
      title_id_hex + ":" + media_id_hex + ":" + module_relative_path + ":" + options.config + ":" +
      options.recomp_root.string());
  PreparationWorkspace workspace(options.cache_root / "workspaces" / workspace_key);
  driver_options.output = workspace.path / "generated";
  if (!xenon::recomp::generate_project(driver_options, report, driver_error)) {
    staging.discard();
    status.report(Phase::Failed, 40, "Generating native source", driver_error);
    result.exit_code = 4;
    result.error = driver_error;
    return result;
  }

  if (cancellation_requested(options.stop_signal)) {
    staging.discard();
    status.report(Phase::Cancelled, 40, "Cancelled after source generation");
    result.exit_code = 7;
    result.error = "cancelled";
    return result;
  }

  // --- Phase: Compiling -----------------------------------------------------
  const auto build_dir = workspace.path / "build";
  status.report(Phase::Compiling, 55, "Configuring native build");
  const std::vector<std::string> configure_args = {
      XENON_CMAKE_COMMAND, "-S", driver_options.output.string(), "-B", build_dir.string(), "-G", "Ninja",
      "-DCMAKE_CXX_COMPILER=" + std::string(XENON_NATIVE_COMPILER),
      "-DXENON_GRAPH_CACHE=" + (driver_options.graph_cache / "native").string(),
      "-DXENON_RECOMP_ROOT=" + options.recomp_root.string(),
      "-DCMAKE_BUILD_TYPE=" + options.config};
  bool cancelled = false;
  std::string command_error;
  if (!run_cancellable_command(configure_args, options.stop_signal, command_error, cancelled)) {
    staging.discard();
    status.report(cancelled ? Phase::Cancelled : Phase::Failed, 55,
                  "Configuring native build", cancelled ? std::string() : command_error);
    result.exit_code = cancelled ? 7 : 5;
    result.error = cancelled ? "cancelled" : command_error;
    return result;
  }

  status.report(Phase::Compiling, 70, "Compiling and linking native code");
  const std::vector<std::string> build_args = {
      XENON_CMAKE_COMMAND, "--build", build_dir.string(), "--target", "xenon_game_module",
      "--parallel", std::to_string(std::min<std::size_t>(8, xenon::recomp::resolve_worker_count({}))),
      "--config", options.config};
  if (!run_cancellable_command(build_args, options.stop_signal, command_error, cancelled)) {
    staging.discard();
    status.report(cancelled ? Phase::Cancelled : Phase::Failed, 70,
                  "Compiling native code", cancelled ? std::string() : command_error);
    result.exit_code = cancelled ? 7 : 5;
    result.error = cancelled ? "cancelled" : command_error;
    return result;
  }

  // --- Phase: Validating ------------------------------------------------------
  status.report(Phase::Validating, 90, "Validating compiled module");
  const auto built_module = find_built_module(build_dir, options.config);
  if (!built_module.has_value()) {
    staging.discard();
    const auto message = "native build succeeded but produced no locatable xenon_game_module "
                         "shared library under " + build_dir.string();
    status.report(Phase::Failed, 90, "Validating module", message);
    result.exit_code = 6;
    result.error = message;
    return result;
  }

#if defined(_WIN32)
  const std::string native_extension_name = "xenon_game_module.dll";
#elif defined(__APPLE__)
  const std::string native_extension_name = "xenon_game_module.dylib";
#else
  const std::string native_extension_name = "xenon_game_module.so";
#endif
  std::error_code copy_ec;
  std::filesystem::copy_file(*built_module, staging.directory() / native_extension_name,
                             std::filesystem::copy_options::overwrite_existing, copy_ec);
  if (copy_ec) {
    staging.discard();
    const auto message = "failed to stage the compiled module: " + copy_ec.message();
    status.report(Phase::Failed, 90, "Validating module", message);
    result.exit_code = 6;
    result.error = message;
    return result;
  }

  xenon::recomp::ArtifactCacheEntry final_entry;
  std::string commit_error;
  if (!staging.commit(native_extension_name, final_entry, commit_error)) {
    status.report(Phase::Failed, 90, "Validating module", commit_error);
    result.exit_code = 6;
    result.error = commit_error;
    return result;
  }

  if (is_primary) {
    status.set_native_extension_path(final_entry.native_extension_path.string());
  }
  result.ok = true;
  result.entry = final_entry;
  return result;
}

}  // namespace xenon::prepare_tool

// xenon-prepare: the out-of-process automatic game preparation worker.
//
// Turns "a content source (disc image / directory / loose XEX) + an
// optionally-installed game module" into a cached, loadable
// xenon_game_module native extension, without ever requiring the end user to
// run the recompiler by hand. See docs/development/GAME_PREPARATION.md for the full
// pipeline, the CLI contract below, and the cache-invalidation rules.
//
// This binary deliberately does the ISO/content-format-aware work itself
// (reading default.xex - and an optional title update - directly out of a
// mounted disc image or directory, with no full-ISO extraction) and hands
// XEX Loader V2 / the Recomp Driver only already-parsed, in-memory XexImage
// data (DriverOptions::pre_parsed_image) - those stay completely unaware of
// where the bytes came from.
//
// Gen 11 (Autonomous Game Intake, see xenon/recomp/game_intake.hpp and
// docs/recomp/GAME_INTAKE_GEN11.md): the content source is first scanned for
// EVERY executable XEX module it contains, not only the mandatory root
// default.xex. A title with exactly one XEX (the overwhelming majority) is
// prepared exactly as before. A title with additional discovered XEX modules
// has every one of them independently analyzed/compiled/cached, and the
// outcome of all of them is recorded in a deterministic
// <cache-root>/game-compilation-graph.json manifest; a secondary module's
// failure does not prevent the primary/playable module from being ready.
//
//   xenon-prepare --content <path> --cache-root <dir>
//                 [--module <hint-package-dir>] [--module-id <id>]
//                 [--title-update <path>] [--config Release|Debug]
//                 [--status-file <path>] [--stop-signal <path>]
//                 [--recomp-root <path>] [--force] [--query]
//
// --query: does not build anything. Prints one line of JSON to stdout and
// exits 0 (or non-zero with {"ok":false,"error":...}):
//   {"ok":true,"needsPreparation":bool,"status":"Fresh"|"Missing"|"Invalid",
//    "cacheKey":"...","titleId":"...","mediaId":"...",
//    "effectiveImageHash":"...","nativeExtensionPath":"..."}
//
// Prepare mode (default): progressively overwrites --status-file (if given)
// with {"phase":...,"percent":...,"task":...,"updatedAtEpochMs":...,
// "cacheKey":...,"titleId":...,"mediaId":...,"effectiveImageHash":...,
// "nativeExtensionPath":...,"error":...}. Creating --stop-signal requests
// cooperative cancellation; the active compiler child (and its full process
// tree) is terminated within a short, bounded time. Exit codes: 0 success
// (including "already Fresh, nothing to do"), 1 usage, 2 content/identity
// error, 3 analysis error, 4 source-generation error, 5 build error,
// 6 validation/commit error, 7 cancelled.

#include "prepare_internal.hpp"

using namespace xenon::prepare_tool;

int main(int argc, char** argv) {
  Options options;
  std::string error;
  if (!parse_args(argc, argv, options, error)) {
    std::cerr << "xenon-prepare: " << error << "\n";
    print_usage();
    return 1;
  }

  try {
    StatusReporter status(options.status_file);

    // --- Phase: Inspecting (Gen 11: discover every executable XEX module
    // the content source actually contains, not just default.xex) ---------
    status.report(Phase::Inspecting, 0, "Discovering executable modules in content source");
    std::vector<xenon::recomp::DiscoveredExecutable> discovered_modules;
    if (!xenon::recomp::discover_game_executables(options.content, discovered_modules, error)) {
      status.report(Phase::Failed, 0, "Inspecting game", error);
      std::cerr << "xenon-prepare: " << error << "\n";
      return 2;
    }

    xenon::recomp::ArtifactCacheStore store(options.cache_root);

    // ------------------------------------------------------------------
    // Single-module fast path: the overwhelming common case (one root
    // default.xex, no additional discovered executables) runs the exact
    // same sequence of operations xenon-prepare has always run for it, so
    // every existing single-module CLI/status.json/query-JSON contract is
    // unchanged byte-for-byte.
    // ------------------------------------------------------------------
    if (discovered_modules.size() == 1) {
      const auto& module = discovered_modules.front();
      status.report(Phase::Inspecting, 0, "Reading executable identity from content source");
      std::vector<std::byte> base_bytes;
      if (!xenon::recomp::read_game_executable_bytes(options.content, module.relative_path,
                                                      base_bytes, error)) {
        status.report(Phase::Failed, 0, "Inspecting game", error);
        std::cerr << "xenon-prepare: " << error << "\n";
        return 2;
      }

      xenon::xbox::XexImage base_image;
      if (!xenon::xbox::parse_xex_image(base_bytes, base_image, &error)) {
        const auto message = "default.xex is not a valid, supported XEX: " + error;
        status.report(Phase::Failed, 0, "Inspecting game", message);
        std::cerr << "xenon-prepare: " << message << "\n";
        return 2;
      }

      xenon::xbox::XexImage effective_image = base_image;
      bool title_update_applied = false;
      if (!options.title_update.empty()) {
        status.report(Phase::ApplyingTitleUpdate, 5, "Applying selected title update");
        std::vector<std::byte> update_bytes;
        if (!read_whole_host_file(options.title_update, update_bytes, error)) {
          status.report(Phase::Failed, 5, "Applying title update", error);
          std::cerr << "xenon-prepare: " << error << "\n";
          return 2;
        }
        xenon::xbox::XexImage patched;
        if (!xenon::xbox::apply_title_update(base_image, update_bytes, patched, &error)) {
          const auto message = "title update could not be applied: " + error;
          status.report(Phase::Failed, 5, "Applying title update", message);
          std::cerr << "xenon-prepare: " << message << "\n";
          return 2;
        }
        effective_image = std::move(patched);
        title_update_applied = true;
      }

      const auto identity = xenon::xbox::compute_effective_identity(
          base_image, title_update_applied ? &effective_image : nullptr);
      const auto title_id_hex = xenon::filesystem::format_xbox_id(identity.title_id);
      const auto media_id_hex = xenon::filesystem::format_xbox_id(identity.media_id);
      const auto effective_hash_hex =
          xenon::xbox::format_effective_image_hash(identity.effective_image_hash);
      status.set_identity(title_id_hex, media_id_hex, effective_hash_hex);

      // --- Resolve the (optional) module hint package -----------------------
      std::unique_ptr<xenon::recomp::FileModuleHintProvider> hint_provider;
      std::optional<xenon::recomp::analysis::AnalysisHintSetV2> hint_set;
      std::string module_id = options.module_id;
      std::string module_compatibility_version = "1";
      std::uint64_t hint_set_hash = 0;

      if (!options.module_dir.empty()) {
        hint_provider = std::make_unique<xenon::recomp::FileModuleHintProvider>(options.module_dir);
        if (!hint_provider->manifest_loaded()) {
          const auto message = "module '" + options.module_dir.string() + "': " + hint_provider->manifest_error();
          status.report(Phase::Failed, 5, "Resolving module", message);
          std::cerr << "xenon-prepare: " << message << "\n";
          return 2;
        }
        if (module_id.empty()) module_id = hint_provider->module_name();
        module_compatibility_version = hint_provider->compatibility_version();

        xenon::recomp::analysis::AnalysisHintSetV2 resolved{};
        std::string hint_error;
        if (!hint_provider->provide(identity, resolved, hint_error)) {
          status.report(Phase::Failed, 5, "Resolving module", hint_error);
          std::cerr << "xenon-prepare: " << hint_error << "\n";
          return 2;
        }
        hint_set_hash = hash_hint_set(resolved);
        hint_set = std::move(resolved);
      }

      // Runtime-learning feedback is part of preparation identity. The trace
      // is append-only and may not exist before the first launch; absence
      // means an empty fact set. Repeated hits are aggregated by the loader
      // and excluded from the cache fingerprint, so only NEW control-flow
      // facts cause a rebuild on the next Play.
      std::vector<xenon::recomp::AdaptiveObservation> adaptive_observations;
      if (!options.observations.empty()) {
        std::error_code observation_ec;
        if (std::filesystem::exists(options.observations, observation_ec) && !observation_ec) {
          std::string observation_error;
          if (!xenon::recomp::load_adaptive_observations(
                  options.observations, adaptive_observations, observation_error)) {
            const auto message = "adaptive observation trace could not be loaded: " +
                                 observation_error;
            status.report(Phase::Failed, 5, "Loading adaptive analysis feedback", message);
            std::cerr << "xenon-prepare: " << message << "\n";
            return 2;
          }
        }
      }
      adaptive_observations.erase(
          std::remove_if(adaptive_observations.begin(), adaptive_observations.end(),
                         [&](const auto& observation) {
                           return !observation.image_hash.empty() &&
                                  observation.image_hash != effective_hash_hex;
                         }),
          adaptive_observations.end());
      const auto adaptive_observation_hash =
          xenon::recomp::adaptive_observation_fingerprint(adaptive_observations);

      // Gen 9 universal knowledge base. Unlike address-scoped adaptive
      // traces, knowledge records are intentionally allowed to originate
      // from another executable revision; the matcher validates normalized
      // code/CFG identity before any record becomes evidence.
      std::vector<xenon::recomp::KnowledgeRecord> knowledge_records;
      if (!options.knowledge.empty()) {
        std::error_code knowledge_ec;
        if (std::filesystem::exists(options.knowledge, knowledge_ec) && !knowledge_ec) {
          std::string knowledge_error;
          if (!xenon::recomp::load_knowledge_base(
                  options.knowledge, knowledge_records, knowledge_error)) {
            const auto message = "knowledge base could not be loaded: " + knowledge_error;
            status.report(Phase::Failed, 5, "Loading universal analysis knowledge", message);
            std::cerr << "xenon-prepare: " << message << "\n";
            return 2;
          }
        }
      }
      const auto knowledge_base_hash =
          xenon::recomp::knowledge_base_fingerprint(knowledge_records);

      xenon::recomp::ArtifactCacheKey key;
      key.title_id = identity.title_id;
      key.media_id = identity.media_id;
      key.effective_image_hash = effective_hash_hex;
      key.xex_relative_path = module.relative_path;
      key.module_id = module_id;
      key.module_compatibility_version = module_compatibility_version;
      key.hint_set_hash = hint_set_hash;
      key.adaptive_observation_hash = adaptive_observation_hash;
      key.knowledge_base_hash = knowledge_base_hash;
      key.preparation_identity = xenon::recomp::graph::preparation_identity(
          options.recomp_root, XENON_CMAKE_COMMAND, XENON_NATIVE_COMPILER);
      key.target_arch = default_target_arch();
      key.build_config = options.config;
      status.set_cache_key(key.digest());

      // Staging belongs to its worker; never remove another process's active build.
      const auto existing = store.lookup(key);

      if (options.query) {
        xenon::core::JsonValue root = xenon::core::JsonValue::make_object();
        root.set("ok", true);
        root.set("needsPreparation", existing.status != xenon::recomp::ArtifactCacheStatus::Fresh);
        root.set("status", existing.status == xenon::recomp::ArtifactCacheStatus::Fresh ? "Fresh"
                          : existing.status == xenon::recomp::ArtifactCacheStatus::Invalid ? "Invalid"
                                                                                            : "Missing");
        root.set("cacheKey", key.digest());
        root.set("titleId", title_id_hex);
        root.set("mediaId", media_id_hex);
        root.set("effectiveImageHash", effective_hash_hex);
        root.set("adaptiveObservationHash", static_cast<double>(adaptive_observation_hash));
        root.set("adaptiveObservationCount", static_cast<double>(adaptive_observations.size()));
        root.set("knowledgeBaseHash", std::to_string(knowledge_base_hash));
        root.set("knowledgeRecordCount", static_cast<double>(knowledge_records.size()));
        root.set("nativeExtensionPath", json_string_or_empty(existing.native_extension_path));
        std::cout << root.dump() << "\n";
        return 0;
      }

      if (!options.force && existing.status == xenon::recomp::ArtifactCacheStatus::Fresh) {
        status.set_native_extension_path(existing.native_extension_path.string());
        status.report(Phase::Complete, 100, "Already prepared - launching cached module");
        std::cout << "xenon-prepare: already prepared: " << existing.native_extension_path << "\n";
        return 0;
      }

      const auto build = prepare_one_module(effective_image, hint_set, adaptive_observations,
                                            knowledge_records, key, store, options, title_id_hex,
                                            media_id_hex, module.relative_path, /*is_primary=*/true,
                                            status);
      if (!build.ok) {
        if (build.exit_code != 7) std::cerr << "xenon-prepare: " << build.error << "\n";
        return build.exit_code;
      }

      status.report(Phase::Complete, 100, "Preparation complete");
      std::cout << "xenon-prepare: prepared " << build.entry.native_extension_path << "\n";
      return 0;
    }

    // ------------------------------------------------------------------
    // Gen 11: more than one executable module was discovered (a title
    // shipping additional bootable/dispatchable XEX modules beyond the
    // mandatory root default.xex). Prepare every one of them and record the
    // outcome in a Game Compilation Graph manifest. The primary module's
    // failure remains a hard, fatal error exactly like the single-module
    // path above; a secondary module's failure is recorded and reported but
    // does not, by itself, prevent the primary/playable module from being
    // ready - "scan every executable, compile every required module" does
    // not mean one obscure secondary XEX can hold the whole title hostage.
    // ------------------------------------------------------------------
    std::unique_ptr<xenon::recomp::FileModuleHintProvider> hint_provider;
    if (!options.module_dir.empty()) {
      hint_provider = std::make_unique<xenon::recomp::FileModuleHintProvider>(options.module_dir);
      if (!hint_provider->manifest_loaded()) {
        const auto message = "module '" + options.module_dir.string() + "': " + hint_provider->manifest_error();
        status.report(Phase::Failed, 5, "Resolving module", message);
        std::cerr << "xenon-prepare: " << message << "\n";
        return 2;
      }
    }
    const std::string module_compatibility_version =
        hint_provider ? hint_provider->compatibility_version() : std::string("1");

    std::vector<xenon::recomp::AdaptiveObservation> all_adaptive_observations;
    if (!options.observations.empty()) {
      std::error_code observation_ec;
      if (std::filesystem::exists(options.observations, observation_ec) && !observation_ec) {
        std::string observation_error;
        if (!xenon::recomp::load_adaptive_observations(
                options.observations, all_adaptive_observations, observation_error)) {
          const auto message = "adaptive observation trace could not be loaded: " + observation_error;
          status.report(Phase::Failed, 5, "Loading adaptive analysis feedback", message);
          std::cerr << "xenon-prepare: " << message << "\n";
          return 2;
        }
      }
    }

    std::vector<xenon::recomp::KnowledgeRecord> knowledge_records;
    if (!options.knowledge.empty()) {
      std::error_code knowledge_ec;
      if (std::filesystem::exists(options.knowledge, knowledge_ec) && !knowledge_ec) {
        std::string knowledge_error;
        if (!xenon::recomp::load_knowledge_base(options.knowledge, knowledge_records, knowledge_error)) {
          const auto message = "knowledge base could not be loaded: " + knowledge_error;
          status.report(Phase::Failed, 5, "Loading universal analysis knowledge", message);
          std::cerr << "xenon-prepare: " << message << "\n";
          return 2;
        }
      }
    }
    const auto knowledge_base_hash = xenon::recomp::knowledge_base_fingerprint(knowledge_records);

    if (options.recomp_root.empty()) {
      const auto message = "no Xenon-Recomp source root available (pass --recomp-root explicitly)";
      status.report(Phase::Failed, 0, "Configuring native build", message);
      std::cerr << "xenon-prepare: " << message << "\n";
      return 5;
    }
    const auto preparation_identity_value = xenon::recomp::graph::preparation_identity(
        options.recomp_root, XENON_CMAKE_COMMAND, XENON_NATIVE_COMPILER);

    xenon::recomp::GameCompilationGraph graph;
    graph.modules.reserve(discovered_modules.size());
    auto query_modules = xenon::core::JsonValue::make_array();

    bool primary_ok = false;
    int primary_exit_code = 0;
    std::string primary_error;
    std::string primary_query_status = "Missing";
    std::string primary_query_cache_key, primary_query_title_id, primary_query_media_id,
        primary_query_hash, primary_query_native_path;
    bool primary_query_needs_preparation = true;

    for (const auto& module : discovered_modules) {
      xenon::recomp::ModuleCompilationRecord record;
      record.relative_path = module.relative_path;
      record.is_primary = module.is_primary;

      const auto fail_module = [&](const std::string& message) {
        record.status = xenon::recomp::ModulePreparationStatus::Failed;
        record.error = message;
        if (module.is_primary) {
          primary_exit_code = primary_exit_code != 0 ? primary_exit_code : 2;
          primary_error = message;
        } else {
          std::cerr << "xenon-prepare: secondary module '" << module.relative_path
                    << "' failed: " << message << "\n";
        }
      };

      std::vector<std::byte> base_bytes;
      std::string module_error;
      if (!xenon::recomp::read_game_executable_bytes(options.content, module.relative_path,
                                                      base_bytes, module_error)) {
        fail_module(module_error);
        graph.modules.push_back(record);
        continue;
      }

      xenon::xbox::XexImage base_image;
      if (!xenon::xbox::parse_xex_image(base_bytes, base_image, &module_error)) {
        fail_module("'" + module.relative_path + "' is not a valid, supported XEX: " + module_error);
        graph.modules.push_back(record);
        continue;
      }

      xenon::xbox::XexImage effective_image = base_image;
      bool title_update_applied = false;
      if (module.is_primary && !options.title_update.empty()) {
        status.report(Phase::ApplyingTitleUpdate, 5, "Applying selected title update");
        std::vector<std::byte> update_bytes;
        if (!read_whole_host_file(options.title_update, update_bytes, module_error)) {
          fail_module(module_error);
          graph.modules.push_back(record);
          continue;
        }
        xenon::xbox::XexImage patched;
        if (!xenon::xbox::apply_title_update(base_image, update_bytes, patched, &module_error)) {
          fail_module("title update could not be applied: " + module_error);
          graph.modules.push_back(record);
          continue;
        }
        effective_image = std::move(patched);
        title_update_applied = true;
      }

      const auto identity = xenon::xbox::compute_effective_identity(
          base_image, title_update_applied ? &effective_image : nullptr);
      const auto title_id_hex = xenon::filesystem::format_xbox_id(identity.title_id);
      const auto media_id_hex = xenon::filesystem::format_xbox_id(identity.media_id);
      const auto effective_hash_hex =
          xenon::xbox::format_effective_image_hash(identity.effective_image_hash);
      record.title_id = title_id_hex;
      record.media_id = media_id_hex;
      record.effective_image_hash = effective_hash_hex;
      if (module.is_primary) status.set_identity(title_id_hex, media_id_hex, effective_hash_hex);

      // A curated module-hint package with no data for a SECONDARY module's
      // specific revision is not fatal: most secondary/rare executables will
      // never have curated per-revision hint data, and requiring it would
      // defeat the entire point of automatic discovery. For the primary
      // module this stays exactly as fatal as it always was.
      std::string module_id = options.module_id;
      std::optional<xenon::recomp::analysis::AnalysisHintSetV2> hint_set;
      std::uint64_t hint_set_hash = 0;
      if (hint_provider) {
        if (module_id.empty()) module_id = hint_provider->module_name();
        xenon::recomp::analysis::AnalysisHintSetV2 resolved{};
        std::string hint_error;
        if (hint_provider->provide(identity, resolved, hint_error)) {
          hint_set_hash = hash_hint_set(resolved);
          hint_set = std::move(resolved);
        } else if (module.is_primary) {
          fail_module(hint_error);
          graph.modules.push_back(record);
          continue;
        } else {
          record.hints_unavailable = true;
        }
      }

      auto adaptive_observations = all_adaptive_observations;
      adaptive_observations.erase(
          std::remove_if(adaptive_observations.begin(), adaptive_observations.end(),
                         [&](const auto& observation) {
                           return !observation.image_hash.empty() &&
                                  observation.image_hash != effective_hash_hex;
                         }),
          adaptive_observations.end());
      const auto adaptive_observation_hash =
          xenon::recomp::adaptive_observation_fingerprint(adaptive_observations);

      xenon::recomp::ArtifactCacheKey key;
      key.title_id = identity.title_id;
      key.media_id = identity.media_id;
      key.effective_image_hash = effective_hash_hex;
      key.xex_relative_path = module.relative_path;
      key.module_id = module_id;
      key.module_compatibility_version = module_compatibility_version;
      key.hint_set_hash = hint_set_hash;
      key.adaptive_observation_hash = adaptive_observation_hash;
      key.knowledge_base_hash = knowledge_base_hash;
      key.preparation_identity = preparation_identity_value;
      key.target_arch = default_target_arch();
      key.build_config = options.config;
      record.cache_key = key.digest();
      if (module.is_primary) status.set_cache_key(key.digest());

      const auto existing = store.lookup(key);

      if (options.query) {
        auto entry_json = xenon::core::JsonValue::make_object();
        entry_json.set("relativePath", module.relative_path);
        entry_json.set("isPrimary", module.is_primary);
        const std::string entry_status =
            existing.status == xenon::recomp::ArtifactCacheStatus::Fresh ? "Fresh"
            : existing.status == xenon::recomp::ArtifactCacheStatus::Invalid ? "Invalid"
                                                                              : "Missing";
        entry_json.set("status", entry_status);
        entry_json.set("cacheKey", key.digest());
        entry_json.set("nativeExtensionPath", json_string_or_empty(existing.native_extension_path));
        query_modules.append(std::move(entry_json));
        record.status = existing.status == xenon::recomp::ArtifactCacheStatus::Fresh
                           ? xenon::recomp::ModulePreparationStatus::Fresh
                           : xenon::recomp::ModulePreparationStatus::Pending;
        record.native_extension_path = json_string_or_empty(existing.native_extension_path);
        if (module.is_primary) {
          primary_query_status = entry_status;
          primary_query_needs_preparation = existing.status != xenon::recomp::ArtifactCacheStatus::Fresh;
          primary_query_cache_key = key.digest();
          primary_query_title_id = title_id_hex;
          primary_query_media_id = media_id_hex;
          primary_query_hash = effective_hash_hex;
          primary_query_native_path = json_string_or_empty(existing.native_extension_path);
        }
        graph.modules.push_back(record);
        continue;
      }

      if (!options.force && existing.status == xenon::recomp::ArtifactCacheStatus::Fresh) {
        record.status = xenon::recomp::ModulePreparationStatus::Fresh;
        record.native_extension_path = existing.native_extension_path.string();
        if (module.is_primary) {
          primary_ok = true;
          status.set_native_extension_path(existing.native_extension_path.string());
        }
        graph.modules.push_back(record);
        continue;
      }

      const auto build = prepare_one_module(effective_image, hint_set, adaptive_observations,
                                            knowledge_records, key, store, options, title_id_hex,
                                            media_id_hex, module.relative_path, module.is_primary,
                                            status);
      if (!build.ok) {
        if (build.exit_code == 7) {
          record.status = xenon::recomp::ModulePreparationStatus::Skipped;
          graph.modules.push_back(record);
          primary_exit_code = 7;
          break;  // cancellation stops the whole batch, not just this module
        }
        fail_module(build.error);
        graph.modules.push_back(record);
        continue;
      }

      record.status = xenon::recomp::ModulePreparationStatus::Prepared;
      record.native_extension_path = build.entry.native_extension_path.string();
      if (module.is_primary) primary_ok = true;
      graph.modules.push_back(record);
    }

    // Always write the Game Compilation Graph manifest so a caller (the
    // launcher, a diagnostic tool, a later re-run) can see every discovered
    // module's outcome in one place, not only whichever one this specific
    // invocation happened to be asked about.
    {
      std::error_code ec;
      std::filesystem::create_directories(options.cache_root, ec);
      std::ofstream manifest(options.cache_root / "game-compilation-graph.json",
                             std::ios::binary | std::ios::trunc);
      manifest << graph.to_json().dump(2);
    }

    if (options.query) {
      xenon::core::JsonValue root = xenon::core::JsonValue::make_object();
      root.set("ok", true);
      root.set("needsPreparation", primary_query_needs_preparation);
      root.set("status", primary_query_status);
      root.set("cacheKey", primary_query_cache_key);
      root.set("titleId", primary_query_title_id);
      root.set("mediaId", primary_query_media_id);
      root.set("effectiveImageHash", primary_query_hash);
      root.set("nativeExtensionPath", primary_query_native_path);
      root.set("modules", std::move(query_modules));
      std::cout << root.dump() << "\n";
      return 0;
    }

    if (primary_exit_code == 7) {
      status.report(Phase::Cancelled, 0, "Cancelled");
      return 7;
    }
    if (!primary_ok) {
      status.report(Phase::Failed, 0, "Preparing primary module", primary_error);
      std::cerr << "xenon-prepare: " << primary_error << "\n";
      return primary_exit_code != 0 ? primary_exit_code : 2;
    }

    const auto primary_it = std::find_if(graph.modules.begin(), graph.modules.end(),
                                         [](const auto& record) { return record.is_primary; });
    status.report(Phase::Complete, 100, "Preparation complete");
    std::cout << "xenon-prepare: prepared "
              << (primary_it != graph.modules.end() ? primary_it->native_extension_path
                                                    : std::string())
              << " (" << graph.modules.size() << " module(s) discovered)\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "xenon-prepare: unexpected error: " << exception.what() << "\n";
    return 2;
  }
}

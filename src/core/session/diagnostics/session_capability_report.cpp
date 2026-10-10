#include <sstream>
#include <string>
#include <vector>

#include "xenon/core/import_classification.hpp"
#include "xenon/core/session.hpp"

namespace xenon::core {

namespace {

std::string format_xex_version(const xbox::XexVersion& version) {
  std::ostringstream out;
  out << static_cast<unsigned>(version.major()) << '.'
      << static_cast<unsigned>(version.minor()) << '.' << version.build() << '.'
      << static_cast<unsigned>(version.qfe());
  return out.str();
}

}  // namespace

JsonValue XenonSession::capability_report() const {
  RunFingerprint fingerprint{};
  if (effective_identity_) {
    fingerprint.effective_xex_sha1 =
        xbox::format_effective_image_hash(effective_identity_->effective_image_hash);
    fingerprint.tu_identity =
        effective_identity_->title_update_applied
            ? (format_xex_version(effective_identity_->base_version) + "+" +
               format_xex_version(effective_identity_->effective_version))
            : "none";
  } else {
    fingerprint.tu_identity = "none";
  }
  fingerprint.gpu_backend = config_.graphics_backend;
  fingerprint.host_os = host_os_identifier();
  fingerprint.host_cpu_arch = host_cpu_arch_identifier();
  fingerprint.diagnostic_mode = config_.enable_export_diagnostics ? "verbose" : "default";

  // Copy rather than mutate capability_report_builder_ in place: producing a
  // report is logically const (it does not change what any subsystem has
  // published), even though attaching the fingerprint uses the same
  // set_section() call a subsystem would use to publish its own section.
  CapabilityReportBuilder report = capability_report_builder_;
  report.set_run_fingerprint(fingerprint);

  // Part 14 of the AC6 Runtime Readiness pass ("Runtime Fallback
  // Accounting"), plus the reviewer's fallback_unique_pc_count/
  // fallback_hot_pc_top_n addition: a title can "run" while secretly
  // executing a large share of its guest code through the Gen 7 dynamic
  // fallback safety net rather than AOT-compiled code. Computed fresh here
  // (pull, not push) from the live atomics/sets each subsystem already
  // maintains for its own purposes - no subsystem needs to proactively call
  // set_section() on every fallback event, and nothing here is paid for
  // unless a caller actually requests a report.
  {
    JsonValue fallback = JsonValue::make_object();
    const std::uint64_t aot_blocks = code_cache_ ? code_cache_->aot_lookup_hits() : 0u;
    const std::uint64_t fallback_blocks =
        dynamic_fallback_ ? dynamic_fallback_->executed_blocks() : 0u;
    const std::uint64_t fallback_instructions =
        dynamic_fallback_ ? dynamic_fallback_->executed_instructions() : 0u;
    const std::uint64_t unsupported_instructions =
        dynamic_fallback_ ? dynamic_fallback_->unsupported_instructions() : 0u;
    const std::uint64_t source_invalidations =
        dynamic_fallback_ ? dynamic_fallback_->source_invalidations() : 0u;
    fallback.set("aotBlocksExecuted", static_cast<std::int64_t>(aot_blocks));
    fallback.set("fallbackBlocksExecuted", static_cast<std::int64_t>(fallback_blocks));
    fallback.set("fallbackInstructionsExecuted",
                 static_cast<std::int64_t>(fallback_instructions));
    fallback.set("unsupportedPpcInstructions",
                 static_cast<std::int64_t>(unsupported_instructions));
    fallback.set("fallbackSourceInvalidations",
                 static_cast<std::int64_t>(source_invalidations));
    fallback.set("unsupportedSprReads",
                 static_cast<std::int64_t>(
                     unsupported_spr_reads_.load(std::memory_order_relaxed)));
    fallback.set("unsupportedSprWrites",
                 static_cast<std::int64_t>(
                     unsupported_spr_writes_.load(std::memory_order_relaxed)));

    std::size_t new_indirect_targets_discovered = 0u;
    {
      std::lock_guard<std::mutex> observation_lock(adaptive_observation_mutex_);
      new_indirect_targets_discovered = adaptive_observation_seen_.size();
    }
    fallback.set("newIndirectTargetsDiscovered",
                 static_cast<std::int64_t>(new_indirect_targets_discovered));

    if (dynamic_fallback_) {
      fallback.set("fallbackUniquePcCount",
                   static_cast<std::int64_t>(dynamic_fallback_->fallback_unique_pc_count()));
      JsonValue hot_pcs = JsonValue::make_array();
      constexpr std::size_t kHotPcTopN = 16u;
      for (const auto& [pc, hits] : dynamic_fallback_->fallback_hot_pcs(kHotPcTopN)) {
        JsonValue entry = JsonValue::make_object();
        entry.set("pc", static_cast<std::int64_t>(pc));
        entry.set("hits", static_cast<std::int64_t>(hits));
        hot_pcs.append(std::move(entry));
      }
      fallback.set("fallbackHotPcTopN", std::move(hot_pcs));
    } else {
      fallback.set("fallbackUniquePcCount", static_cast<std::int64_t>(0));
      fallback.set("fallbackHotPcTopN", JsonValue::make_array());
    }

    report.set_section("fallback", std::move(fallback));
  }

  // Part 7 of the AC6 Runtime Readiness pass ("GPU capability / silent
  // fallback audit"): a "gpu" section built from the live backend counters
  // - see GpuUnsupportedCounters's doc comment on why these must reflect
  // reality rather than being tuned to read as zero. Omitted entirely (not
  // reported as zero) when no GPU backend exists yet, so a caller can tell
  // "not measured" apart from "measured and clean".
  if (gpu_) {
    JsonValue gpu_section = JsonValue::make_object();
    const auto unsupported = gpu_->unsupported_counters();
    gpu_section.set("unknownPackets", static_cast<std::int64_t>(unsupported.unknown_packets));
    gpu_section.set("unknownRegisters", static_cast<std::int64_t>(unsupported.unknown_registers));
    gpu_section.set("unsupportedFetchFormats",
                    static_cast<std::int64_t>(unsupported.unsupported_fetch_formats));
    gpu_section.set("unsupportedTextureFormats",
                    static_cast<std::int64_t>(unsupported.unsupported_texture_formats));
    gpu_section.set("unsupportedSamplerBehaviors",
                    static_cast<std::int64_t>(unsupported.unsupported_sampler_behaviors));
    gpu_section.set("unsupportedShaderInstructions",
                    static_cast<std::int64_t>(unsupported.unsupported_shader_instructions));
    gpu_section.set("unsupportedShaderFeatures",
                    static_cast<std::int64_t>(unsupported.unsupported_shader_features));
    gpu_section.set("unhandledResolveModes",
                    static_cast<std::int64_t>(unsupported.unhandled_resolve_modes));
    gpu_section.set("unhandledDepthStencilPaths",
                    static_cast<std::int64_t>(unsupported.unhandled_depth_stencil_paths));
    gpu_section.set("unexpectedOwnershipTransitions",
                    static_cast<std::int64_t>(unsupported.unexpected_ownership_transitions));
    gpu_section.set("failedResourceBarriers",
                    static_cast<std::int64_t>(unsupported.failed_resource_barriers));
    gpu_section.set("fallbackShaderUses",
                    static_cast<std::int64_t>(unsupported.fallback_shader_uses));
    gpu_section.set("unsupportedOperationsTotal", static_cast<std::int64_t>(unsupported.total()));

    const auto performance = gpu_->performance_counters();
    gpu_section.set("submissions", static_cast<std::int64_t>(performance.submissions));
    gpu_section.set("draws", static_cast<std::int64_t>(performance.draws));
    gpu_section.set("shaderCacheMisses", static_cast<std::int64_t>(performance.shader_cache_misses));
    gpu_section.set("resolveOperations", static_cast<std::int64_t>(performance.resolve_operations));
    gpu_section.set("textureCacheInvalidations",
                    static_cast<std::int64_t>(performance.texture_cache_invalidations));

    report.set_section("gpu", std::move(gpu_section));

    // Part 9 of the AC6 Runtime Readiness pass ("shader coverage report").
    // Shaders are discovered dynamically as the title streams
    // ir::ShaderLoad commands, so this is a live snapshot, not a
    // static "every shader known before boot" requirement.
    JsonValue shader_section = JsonValue::make_object();
    const auto coverage = gpu_->shader_coverage();
    shader_section.set("shadersDiscovered", static_cast<std::int64_t>(coverage.shaders_discovered));
    shader_section.set("shadersTranslated", static_cast<std::int64_t>(coverage.shaders_translated));
    shader_section.set("translationFailures",
                       static_cast<std::int64_t>(coverage.translation_failures));
    shader_section.set("cacheHits", static_cast<std::int64_t>(coverage.cache_hits));
    shader_section.set("cacheMisses", static_cast<std::int64_t>(coverage.cache_misses));
    report.set_section("shader", std::move(shader_section));
  }

  // Part 12 of the AC6 Runtime Readiness pass ("Title Update fidelity"):
  // "Base SHA1, TU identity, Effective SHA1, Effective version" as its own
  // section, distinct from runFingerprint's single effective_xex_sha1/
  // tu_identity (which intentionally only describes what actually ran).
  // Omitted (not zeroed) until a title is actually loaded.
  if (effective_identity_) {
    JsonValue title_update_section = JsonValue::make_object();
    title_update_section.set(
        "baseSha1", xbox::format_effective_image_hash(effective_identity_->base_image_hash));
    title_update_section.set(
        "effectiveSha1", xbox::format_effective_image_hash(effective_identity_->effective_image_hash));
    title_update_section.set("titleUpdateApplied", effective_identity_->title_update_applied);
    title_update_section.set(
        "tuIdentity",
        effective_identity_->title_update_applied
            ? (format_xex_version(effective_identity_->base_version) + "+" +
               format_xex_version(effective_identity_->effective_version))
            : std::string("none"));
    title_update_section.set("effectiveVersion",
                            format_xex_version(effective_identity_->effective_version));
    report.set_section("titleUpdate", std::move(title_update_section));
  }

  // Part 17 of the AC6 Runtime Readiness pass: the whole-XEX import
  // capability audit (Part 3/4's classify_import()/
  // compute_import_capability_verdict(), already real and tested via
  // tools/recomp_tools.cpp's standalone import-scanner and
  // tests/core/import_capability_report_tests.cpp) surfaced directly in
  // the live session report, so a caller does not need to separately run
  // the offline tool against the XEX file to get the same classification.
  // Omitted until a title is loaded, matching "titleUpdate"/"gpu"'s own
  // convention.
  if (loaded_xex_) {
    std::size_t implemented = 0u, safe_stub = 0u, partial = 0u, missing = 0u;
    JsonValue partial_notes = JsonValue::make_array();
    for (const auto& import : loaded_xex_->image.imports) {
      if (import.is_function_address()) continue;  // Not separately resolved - see resolve_xex_imports().

      const auto* descriptor = !import.symbol.empty()
                                    ? export_registry_.resolve(import.module, import.symbol)
                                    : export_registry_.resolve(import.module, import.ordinal);
      switch (classify_import(descriptor)) {
        case ImportClassification::Implemented: ++implemented; break;
        case ImportClassification::SafeStub: ++safe_stub; break;
        case ImportClassification::Partial: ++partial; break;
        case ImportClassification::Missing: ++missing; break;
      }
      if (descriptor != nullptr && !descriptor->partial_note.empty()) {
        JsonValue entry = JsonValue::make_object();
        entry.set("library", import.module);
        entry.set("name", descriptor->name);
        entry.set("note", descriptor->partial_note);
        partial_notes.append(std::move(entry));
      }
    }

    JsonValue imports_section = JsonValue::make_object();
    imports_section.set("implemented", static_cast<std::int64_t>(implemented));
    imports_section.set("safeStub", static_cast<std::int64_t>(safe_stub));
    imports_section.set("partial", static_cast<std::int64_t>(partial));
    imports_section.set("missing", static_cast<std::int64_t>(missing));
    imports_section.set(
        "verdict",
        std::string(to_string(
            compute_import_capability_verdict(implemented, safe_stub, partial, missing))));
    imports_section.set("partialNotes", std::move(partial_notes));
    report.set_section("imports", std::move(imports_section));
  }

  // Reviewer feedback addition 3 on the AC6 Runtime Readiness pass
  // ("RunFingerprint + kernel-object liveness accounting"): RunFingerprint
  // itself has existed since Phase 0 (see set_run_fingerprint() above); this
  // is the liveness half. Surfaces real, live counts from
  // kernel::ThreadManager/kernel::HandleTable - not a running total, a
  // point-in-time snapshot, so a leak (a session whose live count keeps
  // growing across many created-and-finished guest threads/objects instead
  // of returning to baseline) is actually observable. Omitted until a
  // kernel process exists, matching "gpu"/"shader"'s own convention.
  if (kernel_process_) {
    // Opportunistic reap before reading liveThreads below, so this figure
    // reflects real, current liveness rather than "threads ever created
    // minus threads reaped by unrelated activity elsewhere" - cleanup is
    // lazy (see ThreadManager::reap_finished_threads()'s doc comment), so a
    // session that hasn't triggered a reap via any other path recently
    // would otherwise report a stale, inflated count here.
    kernel_process_->thread_manager().reap_finished_threads();

    JsonValue kernel_objects_section = JsonValue::make_object();
    kernel_objects_section.set(
        "liveThreads",
        static_cast<std::int64_t>(kernel_process_->thread_manager().thread_count()));
    kernel_objects_section.set(
        "liveHandles",
        static_cast<std::int64_t>(kernel_process_->handle_table().size()));
    report.set_section("kernelObjects", std::move(kernel_objects_section));
  }

  // Part 15 of the AC6 Runtime Readiness pass ("boot phase checkpoints"):
  // always published (unlike "gpu"/"shader", this needs no subsystem to
  // exist) - the reached checkpoints in the order they actually happened,
  // so a stalled run makes the last real progress point obvious.
  {
    JsonValue boot_section = JsonValue::make_object();
    JsonValue checkpoints = JsonValue::make_array();
    for (const auto checkpoint : boot_checkpoints_.reached_in_order()) {
      checkpoints.append(JsonValue(std::string(to_string(checkpoint))));
    }
    boot_section.set("reached", std::move(checkpoints));
    report.set_section("boot", std::move(boot_section));
  }

  // Part 17 of the AC6 Runtime Readiness pass ("AC6 capability report"),
  // plus the reviewer's PASS/PASS_WITH_FALLBACK/FAIL addition: a single
  // top-level verdict synthesized from every section already published
  // above, rather than a separate subsystem of its own. This never invents
  // new telemetry - it only reads counters/state each subsystem already
  // maintains for its own diagnostic purposes, so the verdict can never
  // read cleaner than the sections it is built from.
  //
  // FAIL is reserved for signals that mean part of the run could not
  // execute at all (a recorded session failure, or a loaded title whose
  // native compiled code never got bound). Everything else that indicates
  // a *degraded but completed* run - unresolved imports that were never
  // actually called, guest code that ran through the Gen 7 dynamic
  // fallback instead of AOT-compiled code, unsupported GPU operations, or
  // shader translation failures - is PASS_WITH_FALLBACK, since the title
  // still produced output rather than crashing outright.
  {
    JsonValue verdict_section = JsonValue::make_object();
    std::vector<std::string> fail_reasons;
    std::vector<std::string> fallback_reasons;

    if (state() == SessionState::Failed) {
      const auto error = last_error();
      fail_reasons.push_back(error.empty() ? "session reported a failure with no message"
                                            : "session failed: " + error);
    }
    if (loaded_xex_ && !native_extension_bound_) {
      fail_reasons.push_back(
          native_extension_error_.empty()
              ? "native extension (compiled guest code) never bound"
              : "native extension not bound: " + native_extension_error_);
    }

    if (!unresolved_imports_.empty()) {
      fallback_reasons.push_back(std::to_string(unresolved_imports_.size()) +
                                  " unresolved import(s)");
    }
    if (dynamic_fallback_) {
      if (dynamic_fallback_->executed_blocks() > 0u) {
        fallback_reasons.push_back(
            std::to_string(dynamic_fallback_->executed_blocks()) +
            " guest code block(s) executed via the Gen 7 dynamic fallback instead of AOT");
      }
      if (dynamic_fallback_->unsupported_instructions() > 0u) {
        fallback_reasons.push_back(
            std::to_string(dynamic_fallback_->unsupported_instructions()) +
            " unsupported PPC instruction(s) encountered");
      }
    }
    {
      const auto spr_reads = unsupported_spr_reads_.load(std::memory_order_relaxed);
      const auto spr_writes = unsupported_spr_writes_.load(std::memory_order_relaxed);
      if (spr_reads > 0u) {
        fallback_reasons.push_back(std::to_string(spr_reads) +
                                    " unsupported SPR read(s) encountered");
      }
      if (spr_writes > 0u) {
        fallback_reasons.push_back(std::to_string(spr_writes) +
                                    " unsupported SPR write(s) encountered");
      }
    }
    if (const auto* imports_section = report.find_section("imports")) {
      const auto safe_stub = imports_section->get_number("safeStub");
      const auto partial = imports_section->get_number("partial");
      if (safe_stub > 0.0) {
        fallback_reasons.push_back(std::to_string(static_cast<std::int64_t>(safe_stub)) +
                                    " import(s) resolve to a safe stub");
      }
      if (partial > 0.0) {
        fallback_reasons.push_back(std::to_string(static_cast<std::int64_t>(partial)) +
                                    " import(s) resolve to a documented partial implementation");
      }
    }
    if (gpu_) {
      const auto unsupported = gpu_->unsupported_counters();
      if (unsupported.total() > 0u) {
        fallback_reasons.push_back(std::to_string(unsupported.total()) +
                                    " unsupported GPU operation(s)");
      }
      const auto coverage = gpu_->shader_coverage();
      if (coverage.translation_failures > 0u) {
        fallback_reasons.push_back(std::to_string(coverage.translation_failures) +
                                    " shader translation failure(s)");
      }
    }

    const char* verdict_state = !fail_reasons.empty()
                                     ? "FAIL"
                                     : (!fallback_reasons.empty() ? "PASS_WITH_FALLBACK" : "PASS");
    verdict_section.set("state", std::string(verdict_state));

    JsonValue fail_array = JsonValue::make_array();
    for (auto& reason : fail_reasons) fail_array.append(JsonValue(std::move(reason)));
    verdict_section.set("failReasons", std::move(fail_array));

    JsonValue fallback_array = JsonValue::make_array();
    for (auto& reason : fallback_reasons) fallback_array.append(JsonValue(std::move(reason)));
    verdict_section.set("fallbackReasons", std::move(fallback_array));

    report.set_section("verdict", std::move(verdict_section));
  }

  return report.build();
}

}  // namespace xenon::core

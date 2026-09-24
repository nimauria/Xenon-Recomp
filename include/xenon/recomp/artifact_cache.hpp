#pragma once

// Prepared native-game-module artifact cache (automatic game preparation
// pipeline). A "prepared game" is a compiled `xenon_game_module` shared
// library produced by Recomp Driver's generate_project() + a nested CMake
// build (see docs/development/GAME_PREPARATION.md); this header owns:
//
//  - ArtifactCacheKey: everything that can invalidate a compiled module
//    (effective executable identity, module/hint revision, Xenon's own
//    generated-code ABI, target arch/build config).
//  - ArtifactCacheStore: content-addressed on-disk storage for compiled
//    modules, keyed by that digest, with atomic stage-build-verify-promote
//    semantics so a crashed or failed build can never leave a half-built
//    module where a launcher would find and load it, and never destroys the
//    last known-good module until its replacement is verified in place.
//
// Deliberately independent of Qt/the launcher: this is a plain xenon_recomp
// library type, driven by the out-of-process preparation worker
// (tools/xenon_prepare.cpp) and covered directly by
// tests/recomp/artifact_cache_tests.cpp.

#include <cstdint>
#include <filesystem>
#include <string>

namespace xenon::recomp {

// Xenon's own generated-code/native-extension ABI compatibility version -
// distinct from the launcher/runtime release version (see
// docs/runtime/RUNTIME_HOST.md "Native extension contract"). Bump this whenever a
// change to generate_project()'s emitted code shape, module_export.cpp's
// exported entry points, or CPU V2's codegen would make an already-compiled
// xenon_game_module unsafe to keep using even though nothing about the game
// itself changed.
inline constexpr std::uint32_t kArtifactAbiVersion = 5;

// Everything that determines whether a previously prepared native module can
// still be used, or must be rebuilt. Two keys with identical field values are
// considered the same artifact; any single field difference invalidates the
// cache entry.
struct ArtifactCacheKey {
  std::uint32_t title_id{};
  std::uint32_t media_id{};
  // Lowercase hex xbox::compute_effective_image_hash() of the exact
  // executable bytes actually being run - already distinguishes "base XEX"
  // from "base + a specific title update" (a patched image hashes
  // differently whenever the patch changes code/data), so no separate title-
  // update identity field is needed.
  std::string effective_image_hash;
  // Gen 11: which executable inside the content source this artifact was
  // built from (game_intake.hpp's DiscoveredExecutable::relative_path) -
  // "default.xex" for every prior single-module preparation and for a
  // title's primary module. A title with more than one discovered XEX
  // module gets one independently cached artifact per module; this field is
  // what keeps two different modules from ever colliding on the same cache
  // entry even if they otherwise share every other identity field.
  std::string xex_relative_path{"default.xex"};
  std::string module_id;
  // The module's EXPLICIT preparation/codegen compatibility identity
  // (FileModuleHintProvider::compatibility_version(), manifest.json's
  // "compatibilityVersion") - deliberately NOT the module's cosmetic display
  // version. A module author bumps this only when something that can
  // invalidate an already-compiled native module actually changed (native
  // hook implementation, guest patch semantics); a metadata-only update
  // (cover art, README, a display-version bump with no compatibility impact)
  // leaves it untouched, so that alone must not force a rebuild (Part 16).
  std::string module_compatibility_version;
  // Stable hash of the exact AnalysisHintSetV2 content actually consumed for
  // this build (its JSON serialization, hashed with xbox::sha1). This is an
  // additional, automatic safety net alongside module_compatibility_version:
  // it changes whenever the analysis hints a module ships actually change,
  // even if a module author forgets to bump compatibility_version, but never
  // changes for a metadata/artwork-only update, matching Part 16 exactly.
  std::uint64_t hint_set_hash{};
  // Stable hash of distinct runtime-learned control-flow facts consumed by
  // analysis. Repeated hits of an already-known fact intentionally do not
  // change this value, while a newly observed target invalidates the artifact
  // so the next Play can re-analyze/recompile with that knowledge.
  std::uint64_t adaptive_observation_hash{};
  // Gen 9 normalized knowledge/signature facts consumed by analysis. Any
  // semantic database change must invalidate a prepared native artifact just
  // like a hint/adaptive-control-flow change does.
  std::uint64_t knowledge_base_hash{};
  std::uint32_t abi_version{kArtifactAbiVersion};
  std::string preparation_identity; // exact producer/toolchain/header/library environment
  std::string target_arch;   // e.g. "x86_64", "arm64"
  std::string build_config;  // e.g. "Release", "Debug"

  // Stable, deterministic digest (lowercase hex) of every field above, used
  // as the cache entry's directory name.
  [[nodiscard]] std::string digest() const;
};

enum class ArtifactCacheStatus {
  Fresh,    // a validated, matching artifact exists - safe to launch directly
  Missing,  // no entry for this key exists at all
  Invalid,  // an entry directory exists but failed validation (corrupt/partial/foreign)
};

struct ArtifactCacheEntry {
  ArtifactCacheStatus status{ArtifactCacheStatus::Missing};
  std::filesystem::path native_extension_path;  // meaningful only when status == Fresh
  std::string built_at_iso8601;                 // meaningful only when status == Fresh
};

class ArtifactCacheStore;

// A private, uncommitted build directory for one ArtifactCacheKey. Never
// visible to ArtifactCacheStore::lookup() until commit() succeeds. Exactly
// one of commit()/discard() must run before destruction; the destructor
// itself discards as a safety net (e.g. an exception unwinds past a worker
// that forgot to call either), never leaving an orphaned staging directory
// silently promoted.
class ArtifactStagingBuild {
 public:
  ArtifactStagingBuild(const ArtifactStagingBuild&) = delete;
  ArtifactStagingBuild& operator=(const ArtifactStagingBuild&) = delete;
  ArtifactStagingBuild(ArtifactStagingBuild&& other) noexcept;
  ArtifactStagingBuild& operator=(ArtifactStagingBuild&& other) noexcept;
  ~ArtifactStagingBuild();

  // Directory the caller should write the build's output tree into (the
  // generated project, its nested build directory, and the compiled shared
  // library) before calling commit().
  [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }

  // Validates `native_extension_relative_file` (a path relative to
  // directory()) - it must exist and be a loadable shared library exporting
  // `Xenon_BindCompiledRegistry` - and, only if that passes, atomically
  // promotes this staging directory to become the store's current entry for
  // the key it was created with. The previous entry (if any) is removed only
  // strictly after the new one is fully and durably in place; on ANY failure
  // (validation or promotion), the previous entry - if one exists - is left
  // completely untouched, this staging directory is removed, and false is
  // returned with `error` set.
  [[nodiscard]] bool commit(const std::filesystem::path& native_extension_relative_file,
                            ArtifactCacheEntry& out_entry, std::string& error);

  // Explicit cancellation/failure path: removes this staging directory and
  // leaves any existing committed entry for this key completely untouched.
  // Safe to call more than once and safe to skip if commit() already ran.
  void discard() noexcept;

 private:
  friend class ArtifactCacheStore;
  ArtifactStagingBuild(std::filesystem::path store_root, std::string key_digest,
                       std::filesystem::path directory) noexcept;

  std::filesystem::path store_root_;
  std::string key_digest_;
  std::filesystem::path directory_;
  bool resolved_{false};  // true once commit() or discard() has run
};

class ArtifactCacheStore {
 public:
  explicit ArtifactCacheStore(std::filesystem::path root);

  // Looks up the entry for `key`. When a directory exists for this key's
  // digest, it is re-validated on every call (module file present, loads,
  // exports the expected symbol) rather than trusted purely from its
  // presence - a directory that exists but fails validation reports
  // ArtifactCacheStatus::Invalid, never silently Fresh.
  [[nodiscard]] ArtifactCacheEntry lookup(const ArtifactCacheKey& key) const;

  // Begins a new staged build for `key` in a fresh, private directory under
  // the store root, distinct from that key's (possibly currently valid)
  // committed entry directory - a build in progress can never be observed by
  // lookup() and can never corrupt a still-valid entry if the process is
  // killed mid-build.
  [[nodiscard]] ArtifactStagingBuild begin_staging(const ArtifactCacheKey& key) const;

  // Removes any staging directories left behind by a previous run that never
  // reached commit()/discard() (process killed, crashed). Safe to call at
  // worker startup before any lookup()/begin_staging() call.
  void clean_stale_staging() const;

  [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

 private:
  std::filesystem::path root_;
};

// Real load-and-export-symbol check used by ArtifactStagingBuild::commit()
// and exposed directly so the preparation worker's own "Validating module"
// progress phase (docs/development/GAME_PREPARATION.md) can report the identical check
// independently of a commit(). Loads `path` with the host loader
// (LoadLibrary/dlopen), resolves `Xenon_BindCompiledRegistry`, then unloads
// it immediately - never left resident.
[[nodiscard]] bool validate_native_module(const std::filesystem::path& path, std::string& error);

}  // namespace xenon::recomp

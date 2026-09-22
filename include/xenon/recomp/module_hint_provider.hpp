#pragma once

// Production ModuleHintProviderV2 (Part 2 of the Gracemeria readiness pass):
// reads real, installed game-module package data from disk and supplies the
// Analysis Hint Schema V2 hint set for the effective executable revision the
// Recomp Driver is actually analyzing.
//
// Module package layout on disk (Part 2.1):
//
//   <module_root>/
//     manifest.json                        - {"moduleName": "...",
//                                              "compatibilityVersion": "...",
//                                              "revisions": ["<hash>", ...]}
//     revisions/<effective_image_hash_hex>/analysis.json  - one AnalysisHintSetV2 (see
//                                             analysis_schema_json.hpp), scoped to exactly
//                                             that effective executable revision
//
// "compatibilityVersion" (Part 16 of the automatic game preparation pass) is
// an explicit, author-controlled identity distinct from the module's
// cosmetic display version: a module author bumps it only when a change
// could invalidate an already-prepared native module in a way analysis.json's
// own content hash would not necessarily catch (native hook implementation
// changes, guest patch semantics, anything living in the module's own native
// code rather than in analysis.json data). A metadata-only update (cover art,
// README, a display-version bump with no compatibility-affecting change)
// leaves this field untouched, so the automatic preparation pipeline's
// artifact cache (xenon/recomp/artifact_cache.hpp) correctly does not rebuild
// for it. Defaults to "1" when absent, so a module that never needs this
// distinction can omit the field entirely.
//
// <effective_image_hash_hex> is the same lowercase hex form
// xbox::format_effective_image_hash() produces, so selecting the right
// revision is an exact directory-name match against the currently loaded
// executable's identity - never a "closest looking" fallback (Part 2.5).
// A module with no data for the running revision is a clear, reported
// failure, not a silent one.

#include <filesystem>
#include <string>

#include "xenon/recomp/driver.hpp"

namespace xenon::recomp {

class FileModuleHintProvider final : public ModuleHintProviderV2 {
 public:
  explicit FileModuleHintProvider(std::filesystem::path module_root);

  [[nodiscard]] bool provide(const xbox::XexEffectiveIdentity& identity,
                             analysis::AnalysisHintSetV2& out_hint_set,
                             std::string& error) const override;

  // The module's declared name (manifest.json's "moduleName"), or empty if
  // the manifest could not be read. Diagnostic use only.
  [[nodiscard]] const std::string& module_name() const noexcept { return module_name_; }
  // The module's explicit preparation/codegen compatibility identity (see
  // above) - "1" when the manifest does not declare one. This is what the
  // automatic preparation pipeline's ArtifactCacheKey uses, never the
  // module's cosmetic display version.
  [[nodiscard]] const std::string& compatibility_version() const noexcept {
    return compatibility_version_;
  }
  // True once the constructor successfully read manifest.json. provide()
  // always fails cleanly (never crashes) even when this is false.
  [[nodiscard]] bool manifest_loaded() const noexcept { return manifest_loaded_; }
  [[nodiscard]] const std::string& manifest_error() const noexcept { return manifest_error_; }

 private:
  std::filesystem::path module_root_;
  std::string module_name_;
  std::string compatibility_version_{"1"};
  bool manifest_loaded_{false};
  std::string manifest_error_;
};

}  // namespace xenon::recomp

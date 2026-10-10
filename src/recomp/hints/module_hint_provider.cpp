#include "xenon/recomp/module_hint_provider.hpp"

#include <fstream>
#include <sstream>

#include "xenon/recomp/analysis_schema_json.hpp"

namespace xenon::recomp {

namespace {

bool read_file(const std::filesystem::path& path, std::string& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) return false;
  std::ostringstream buffer;
  buffer << file.rdbuf();
  out = buffer.str();
  return true;
}

}  // namespace

FileModuleHintProvider::FileModuleHintProvider(std::filesystem::path module_root)
    : module_root_(std::move(module_root)) {
  std::string manifest_text;
  const auto manifest_path = module_root_ / "manifest.json";
  if (!read_file(manifest_path, manifest_text)) {
    manifest_error_ = "unable to open module manifest: " + manifest_path.string();
    return;
  }
  core::JsonValue manifest;
  std::string parse_error;
  if (!core::JsonValue::parse(manifest_text, manifest, &parse_error)) {
    manifest_error_ = "module manifest is not valid JSON: " + parse_error;
    return;
  }
  if (!manifest.is_object()) {
    manifest_error_ = "module manifest must be a JSON object";
    return;
  }
  module_name_ = manifest.get_string("moduleName");
  compatibility_version_ = manifest.get_string("compatibilityVersion", "1");
  manifest_loaded_ = true;
}

bool FileModuleHintProvider::provide(const xbox::XexEffectiveIdentity& identity,
                                     analysis::AnalysisHintSetV2& out_hint_set,
                                     std::string& error) const {
  if (!manifest_loaded_) {
    error = "module '" + module_root_.string() + "' has no usable manifest: " + manifest_error_;
    return false;
  }

  const auto revision_hex = xbox::format_effective_image_hash(identity.effective_image_hash);
  const auto analysis_path = module_root_ / "revisions" / revision_hex / "analysis.json";

  std::string analysis_text;
  if (!read_file(analysis_path, analysis_text)) {
    // Part 2.5: a hard, explicit failure - never a silent fallback to a
    // different revision's analysis data.
    error = "module '" + module_name_ + "' has no analysis data for effective executable revision " +
            revision_hex + " (expected " + analysis_path.string() + ")";
    return false;
  }

  core::JsonValue analysis_json;
  std::string parse_error;
  if (!core::JsonValue::parse(analysis_text, analysis_json, &parse_error)) {
    error = "module '" + module_name_ + "' revision " + revision_hex +
            " analysis.json is not valid JSON: " + parse_error;
    return false;
  }

  analysis::AnalysisHintSetV2 parsed{};
  if (!analysis::from_json(analysis_json, parsed, error)) {
    error = "module '" + module_name_ + "' revision " + revision_hex +
            " analysis.json could not be parsed: " + error;
    return false;
  }

  // The on-disk revision directory name is itself already an exact match by
  // construction (we looked it up by that exact hash), but the analysis
  // data's own declared identity must agree too - guards against a hand-
  // edited or copy-pasted analysis.json whose embedded identity silently
  // disagrees with the directory it lives in.
  if (!analysis::hint_set_matches_identity(parsed, identity)) {
    error = "module '" + module_name_ + "' revision " + revision_hex +
            " analysis.json's embedded identity does not match its own revision directory - "
            "the file may have been copied from a different revision";
    return false;
  }

  out_hint_set = std::move(parsed);
  return true;
}

}  // namespace xenon::recomp

#pragma once

// JSON (de)serialization for Analysis Hint Schema V2 (analysis_schema.hpp).
// Used by the production ModuleHintProviderV2 (module_hint_provider.hpp,
// which reads an installed module's on-disk revision data) and, from outside
// this repository, by the separate Project Gracemeria migration tool that
// converts ac6recomp_config.toml into this schema (see
// docs/recomp/ANALYSIS_HINT_SCHEMA_V2.md's "AC6 config migration tool lives in
// Project Gracemeria, not here" section - this header exposes only the
// generic, title-agnostic (de)serialization surface that tool depends on).
// Uses xenon::core::JsonValue (include/xenon/core/json.hpp) - a small,
// dependency-free, header-only value/parser/writer already used by the
// runtime host's own JSON boundary; including it here does not add a link
// dependency on xenon_core.

#include <string>

#include "xenon/core/json.hpp"
#include "xenon/recomp/analysis_schema.hpp"

namespace xenon::recomp::analysis {

[[nodiscard]] core::JsonValue to_json(const AnalysisHintSetV2& hint_set);

// Parses `json` (already-parsed JSON, e.g. from core::JsonValue::parse())
// into `out`. Returns false with `error` set on any structural problem
// (wrong shape, unparseable address, ...); never partially fills `out` on
// failure - `out` is reset to a fresh, empty AnalysisHintSetV2 at the start
// of a failed parse; a schema_version mismatch is NOT checked here (that is
// validate_hint_set()'s job, run by the driver after this).
[[nodiscard]] bool from_json(const core::JsonValue& json, AnalysisHintSetV2& out, std::string& error);

}  // namespace xenon::recomp::analysis

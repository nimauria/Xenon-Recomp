#pragma once

// CapabilityReportBuilder (Phase 0 of the AC6 Runtime Readiness / Platform
// Fidelity pass) aggregates named diagnostic sections - import capability
// classification, fallback-execution accounting, GPU telemetry, boot
// checkpoints, kernel-object liveness, title-update identity, and so on -
// from any subsystem into one JsonValue document. This is the single sink
// every diagnostic/telemetry addition in that pass feeds into, so no
// subsystem needs its own bespoke report-aggregation mechanism.
//
// Deliberately game-agnostic: nothing here accepts or stores a title-specific
// parameter, and it never branches on which title a session has loaded. A
// game module may still request/label a report for its own session; the
// genericity lives entirely in this builder's own code never knowing or
// caring which title that is.

#include <string>
#include <string_view>

#include "xenon/core/json.hpp"
#include "xenon/core/run_fingerprint.hpp"

namespace xenon::core {

class CapabilityReportBuilder {
 public:
  // Replaces (or creates) the named section. Sections are independent - a
  // subsystem publishing its own section does not need to know about any
  // other subsystem's section.
  CapabilityReportBuilder& set_section(std::string name, JsonValue value);

  [[nodiscard]] const JsonValue* find_section(std::string_view name) const;
  [[nodiscard]] bool has_section(std::string_view name) const;

  // Attaches the run fingerprint under the reserved "runFingerprint" section
  // name so every emitted report can be traced to the exact combination of
  // XEX/TU/build/analysis/config inputs that produced it.
  CapabilityReportBuilder& set_run_fingerprint(const RunFingerprint& fingerprint);

  // Serializes every registered section into one document:
  //   { "sections": { "<name>": <value>, ... } }
  [[nodiscard]] JsonValue build() const;

  // Removes every section (fingerprint included). Mainly for tests.
  void clear();

 private:
  JsonValue::Object sections_{};
};

}  // namespace xenon::core

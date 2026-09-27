#pragma once

// RunFingerprint (Phase 0 of the AC6 Runtime Readiness / Platform Fidelity
// pass) ties together the identity of every input that could make one Xenon
// run behave differently from another supposedly-identical run: the exact
// guest image, the exact Xenon/game-module builds, the analysis/codegen
// artifacts, and the host/runtime configuration. Every checkpoint, crash
// report, differential trace and capability report produced from Phase 0
// onward carries one, so "run A worked, run B didn't" can always be traced
// to whether A and B were actually running identical generated artifacts -
// this matters once adaptive learning and content-addressed compilation are
// both live, since two runs of the "same" title are not guaranteed to be
// running byte-identical generated code.
//
// Fields are plain strings and never guest addresses or title-specific
// identifiers, so the type is meaningful for any Xbox 360 title, not just
// whichever one is currently under validation.

#include <string>

#include "xenon/core/json.hpp"

namespace xenon::core {

struct RunFingerprint {
  std::string effective_xex_sha1{};    // hex; empty if no title is loaded yet
  std::string tu_identity{};           // "<base_version>+<effective_version>" or "none"
  std::string xenon_build_id{};        // Xenon core build/version identifier
  std::string game_module_build_id{};  // whichever game module is loaded, if any; empty if none
  std::string analysis_schema_version{};
  std::string analysis_hash{};
  std::string generated_code_hash{};
  std::string runtime_config_hash{};
  std::string gpu_backend{};
  std::string host_os{};
  std::string host_cpu_arch{};
  std::string diagnostic_mode{};

  [[nodiscard]] JsonValue to_json() const;

  [[nodiscard]] bool operator==(const RunFingerprint&) const = default;
};

// Best-effort host platform identity, computed once per process from
// compile-time OS/arch macros - no runtime probing, so always available even
// before a session exists.
[[nodiscard]] std::string host_os_identifier();
[[nodiscard]] std::string host_cpu_arch_identifier();

}  // namespace xenon::core

#pragma once

// Recomp Gen 11 — Autonomous Game Intake/Triage.
//
// A real Xbox 360 title is not always exactly one executable. Besides the
// mandatory root `default.xex`, a content source can legitimately contain
// additional bootable/dispatchable XEX modules (alternate boot regions,
// bundled loader stubs, secondary executables placed elsewhere in the
// directory/disc tree). Historically xenon-prepare only ever looked for a
// single root `default.xex` and required a human to know about anything
// else. This header supplies the format-neutral discovery step ("scan every
// executable") that a fully automatic "Import ISO -> Play" pipeline needs,
// independent of xenon-prepare's own CLI/process-worker concerns so it is
// directly unit-testable.
//
// This is deliberately scoped to *discovery and batch preparation*, not to
// changing how/when the runtime loads a secondary module at execution time
// (that remains the kernel module loader's existing responsibility) and not
// to DLC/STFS package content classification (a distinct, title-specific
// concern layered on top of this, not implemented here). See
// docs/recomp/GAME_INTAKE_GEN11.md.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "xenon/core/json.hpp"

namespace xenon::recomp {

// One executable XEX module found inside a content source. `relative_path`
// always uses forward slashes and is relative to the content root (a bare
// filename for a loose .xex file or a single-executable directory/disc
// image) - stable, host- and guest-path-convention-neutral identity that can
// be persisted (cache keys, the compilation graph manifest) and fed back
// into read_game_executable_bytes() unchanged.
struct DiscoveredExecutable {
  std::string relative_path;
  // True for exactly one entry: the mandatory root default.xex every valid
  // Xbox 360 title has. Every other discovered executable is a secondary
  // module.
  bool is_primary{};
};

// Scans `content` - a directory, a loose .xex file, or an Xbox 360 disc
// image (.iso/.xgd/.dvd, resolved via resolve_gdfx_image_path exactly like
// the prior single-module xenon-prepare code path) - for every executable
// XEX module it contains.
//
// The root default.xex is always discovered and always placed first with
// is_primary=true; its absence is a hard error, matching every prior
// single-module xenon-prepare invocation exactly. A loose .xex file, or a
// directory/disc image that happens to contain only a root default.xex,
// always yields exactly one DiscoveredExecutable - so every existing
// single-executable caller sees byte-identical discovery results to before
// this existed. Additional discovered .xex files are sorted by
// relative_path for deterministic ordering and appended after the primary
// entry.
[[nodiscard]] bool discover_game_executables(const std::filesystem::path& content,
                                             std::vector<DiscoveredExecutable>& out,
                                             std::string& error);

// Reads the complete raw bytes of the executable at `relative_path` (as
// produced by discover_game_executables() for this exact `content`) back
// out of it, dispatching to the same directory/.xex/disc-image backend
// discover_game_executables() used - never extracted to a temporary host
// file for the disc-image case, matching the existing single-module
// behavior.
[[nodiscard]] bool read_game_executable_bytes(const std::filesystem::path& content,
                                              const std::string& relative_path,
                                              std::vector<std::byte>& out,
                                              std::string& error);

// Per-module outcome recorded in a GameCompilationGraph. This is diagnostic/
// reporting data, not itself an artifact-cache key.
enum class ModulePreparationStatus : std::uint8_t {
  Pending,
  Fresh,       // an already-valid cached artifact was found; nothing rebuilt
  Prepared,    // analyzed, generated and compiled successfully this run
  Failed,
  Skipped,     // e.g. cancelled before this module was reached
};

[[nodiscard]] const char* module_preparation_status_name(ModulePreparationStatus status) noexcept;

struct ModuleCompilationRecord {
  std::string relative_path;
  bool is_primary{};
  ModulePreparationStatus status{ModulePreparationStatus::Pending};
  std::string title_id;              // lowercase hex, empty until identity is known
  std::string media_id;              // lowercase hex
  std::string effective_image_hash;  // lowercase hex
  std::string cache_key;             // ArtifactCacheKey::digest(), empty until known
  std::string native_extension_path; // meaningful only when status is Fresh/Prepared
  std::string error;                 // meaningful only when status is Failed
  // True when a curated module-hint package exists but declared no data for
  // this module's specific revision. For the primary module that remains a
  // hard failure (unchanged prior behavior); for a secondary module this is
  // recorded here and preparation proceeds without hints for it, since most
  // secondary/rare executables will not have curated per-revision hint data.
  bool hints_unavailable{};
};

// The full per-title intake result: what discover_game_executables() found,
// crossed with what preparing each of those modules actually did. Persisted
// as a deterministic JSON manifest (game-compilation-graph.json) so a
// caller - the launcher, a diagnostic tool, a future re-run - can see the
// state of every module in one place instead of only the one that happened
// to be requested most recently.
struct GameCompilationGraph {
  static constexpr int kSchemaVersion = 1;
  std::vector<ModuleCompilationRecord> modules;

  [[nodiscard]] core::JsonValue to_json() const;
};

}  // namespace xenon::recomp

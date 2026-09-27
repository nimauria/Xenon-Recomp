# Recomp Gen 11 — Autonomous Game Intake

Gen 11 removes one remaining manual step from automatic game preparation:
`xenon-prepare` used to look for exactly one executable, a root
`default.xex`, and required a human to know about anything else a title
shipped. Gen 11 scans the content source for **every** executable XEX module
it actually contains and prepares all of them, recording the outcome of the
whole title - not just whichever module happened to be requested most
recently - in one deterministic manifest.

It builds on Gen 10's content-addressed compilation graph (each discovered
module gets its own independently cached artifact) and leaves Gen 6-9
untouched: every discovered module goes through the exact same
analysis/verification/knowledge pipeline as before, once per module.

## Scope

Gen 11 here is specifically the **intake/preparation** side: discovering
which executables exist and getting all of them precompiled and cached ahead
of time, mirroring RPCS3's documented motivation for precompiling every
module in a game up front so compilation never suddenly happens mid-session
when a title enters another component. It is deliberately **not**:

- a change to how the runtime dynamically loads a secondary module during
  play - that remains the kernel module loader's existing responsibility;
- DLC/STFS package content classification - a distinct, title-specific
  concern that would layer on top of this, not implemented here;
- automatic multi-title-update fan-out - `--title-update` still selects one
  update to apply to the primary module, exactly as before.

## Discovery

`include/xenon/recomp/game_intake.hpp` (`xenon::recomp`) adds
`discover_game_executables()`, format-neutral over the same three content
source shapes `xenon-prepare` already understood:

- a **directory**: recursively scanned for every `.xex` file. The root
  `default.xex` (matched case/separator-insensitively via
  `xenon::filesystem::guest_path_equal`) is mandatory and always reported
  first with `is_primary = true`; its absence is a hard error, unchanged from
  every prior single-module invocation. Every other discovered `.xex` file is
  a secondary module, sorted by relative path for deterministic ordering.
- a **loose `.xex` file**: always yields exactly one primary executable.
- an Xbox 360 **disc image** (`.iso`/`.xgd`/`.dvd`, resolved via the existing
  `resolve_gdfx_image_path` + `GdfxImageSource`): recursively walked the same
  way, entirely through `ReadOnlyContentSource::list()` - no extraction to a
  temporary file, matching the pre-Gen-11 single-module behavior.

`DiscoveredExecutable::relative_path` is always forward-slash-joined
regardless of source, so it is a stable, persistable identity (used directly
as an `ArtifactCacheKey` field and in the compilation graph manifest) whether
it came from a host directory or a disc image. `read_game_executable_bytes()`
reads a given relative path's complete bytes back out of the same content
source, dispatching through the identical directory/file/disc-image logic so
discovery and reading can never disagree about where a module's bytes live.

A directory or disc image containing only the mandatory root `default.xex` -
the overwhelming majority of titles - always yields exactly one
`DiscoveredExecutable`, so every existing single-module `xenon-prepare`
invocation sees byte-identical discovery results to before Gen 11 existed.

## Independent per-module cache identity

`ArtifactCacheKey` gained `xex_relative_path` (default `"default.xex"`,
matching every prior single-module key exactly). Two discovered modules of
the same title - even ones that happen to compile to byte-identical native
code - can never collide on one cache entry, because the relative path each
was found at is part of the key `digest()` hashes. `kArtifactAbiVersion` was
bumped to 5 to invalidate artifacts prepared before this field existed.

Each module also gets its own Gen 10 build **workspace**
(`cache-root/workspaces/<hash of title+media+relative_path+config+recomp-root>`),
so two modules of the same title never share one nested CMake build
directory or fight over the same `xenon_game_module` output.

## `xenon-prepare` behavior

`tools/xenon_prepare.cpp`'s single-module code path is preserved verbatim
for the common case (exactly one discovered executable): identical CLI
contract, identical `status.json`/`--query` JSON shape, identical exit codes.
The actual analyze/generate/compile/validate/commit sequence
(`prepare_one_module()`) is shared unchanged between that path and the new
multi-module loop, so the expensive, previously-battle-tested compilation
logic has exactly one implementation either way.

When more than one executable is discovered:

- every module is independently analyzed, compiled and cached;
- a curated module-hint package with no data for a **secondary** module's
  specific revision is not fatal - most secondary/rare executables will
  never have curated per-revision hint data, and requiring it would defeat
  the point of automatic discovery. It remains exactly as fatal as before
  for the **primary** module;
- `--title-update` still applies only to the primary module;
- runtime-learned adaptive observations are re-filtered per module against
  that module's own effective image hash (each module has its own identity;
  Gen 9 knowledge records are fingerprint-based and shared as-is);
- a **secondary** module's failure is recorded and reported to stderr but
  does not prevent the primary/playable module from being ready - one
  obscure secondary executable cannot hold an entire title hostage;
- a **primary** module's failure remains exactly as fatal as it always was,
  with the same documented exit codes (2/3/4/5/6/7);
- `--query` additionally reports a `"modules"` array (one entry per
  discovered module: `relativePath`, `isPrimary`, `status`, `cacheKey`,
  `nativeExtensionPath`) alongside the existing top-level fields, which
  continue to describe the primary module exactly as before.

## Game Compilation Graph manifest

Every non-query invocation writes `<cache-root>/game-compilation-graph.json`
(`GameCompilationGraph::to_json()`), unconditionally - even for a single
discovered module, so the format is always present rather than only
sometimes existing. Schema version 1; per module:

```json
{
  "schemaVersion": 1,
  "modules": [
    {"relativePath": "default.xex", "isPrimary": true, "status": "Prepared",
     "titleId": "...", "mediaId": "...", "effectiveImageHash": "...",
     "cacheKey": "...", "nativeExtensionPath": "...", "hintsUnavailable": false},
    {"relativePath": "media/update.xex", "isPrimary": false, "status": "Failed",
     "error": "...", "hintsUnavailable": true}
  ]
}
```

`status` is one of `Pending`, `Fresh` (an already-valid cached artifact was
found; nothing rebuilt), `Prepared` (analyzed/generated/compiled this run),
`Failed`, or `Skipped` (cancellation reached before this module).

## Safety properties

- A title with only the mandatory `default.xex` is discovered, prepared and
  reported identically to every `xenon-prepare` invocation before Gen 11
  existed - discovery, the cache key shape, `status.json`, `--query`'s JSON
  and exit codes are all unchanged for that case.
- Two modules of the same title can never share one cache entry or one build
  workspace, even with byte-identical content.
- A secondary module's failure is visible (stderr, the graph manifest) and
  never silently swallowed, but never blocks the primary module either.
- Discovery never fabricates an executable: only real `.xex` files are
  reported, and the mandatory root `default.xex`'s absence is always a hard,
  explicit error.

## Tests

- `tests/recomp/game_intake_tests.cpp`: directory discovery (single module,
  multiple nested modules sorted deterministically, missing primary is a hard
  error), a loose `.xex` file, an unrecognized content source, a synthetic
  Xbox 360 disc image (built the same way
  `tests/filesystem/filesystem_tests.cpp` builds its GDFX fixtures) with a
  secondary XEX module nested next to an unrelated subdirectory, and
  `GameCompilationGraph`'s JSON shape.
- `tests/recomp/xenon_prepare_worker_tests.cpp`: a real end-to-end case
  driving the actual `xenon-prepare` binary against a content directory with
  two XEX modules - both get independently analyzed and compiled through
  real MSVC, the manifest lists both with distinct native extension paths,
  and a second Play rebuilds neither.

## Deliberate limits / next work

Gen 11 does not scan for DLC/STFS package content, does not fan out across
multiple title updates automatically, and does not change how the runtime
loads a secondary module at execution time. A title whose secondary
executables are delivered as downloadable content rather than sitting
alongside `default.xex` on disc/in the install directory is not yet covered.

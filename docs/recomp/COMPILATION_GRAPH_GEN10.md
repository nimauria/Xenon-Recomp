# Recomp Gen 10 — Content-Addressed Compilation Graph

Gen 10 turns Recomp Driver's compilation pipeline into a content-addressed
dependency graph, in the same spirit as a build system like Nix: every stage's
output is a pure function of its own real inputs (decoded guest bytes, the
proven CFG boundary, the exact IR, the codegen backend's identity), keyed by a
digest of those inputs rather than by guest address or a manually bumped
version string. A change that does not affect a region's inputs never causes
that region to be re-analyzed, re-lifted or re-emitted, no matter how many
other functions in the same title changed.

It builds on Gen 6 (whose IR is now the reusable artifact), Gen 7/8/9 (whose
diagnostics/knowledge stay revision-scoped exactly as before) and the existing
`ArtifactCacheStore` (whole-module native-extension cache). It does not
replace any of them — it makes the expensive stages between "analyzed" and
"compiled shared library" incremental.

## The dependency graph

`include/xenon/recomp/compilation_graph.hpp` (`xenon::recomp::graph`) defines
a small, explicit node graph per compiled region:

```text
decoded-region  (decoder)
      |
      v
analysis-cfg    (analysis)
      |
      v
ir              (ir + cpu semantics + IR producer build identity)
      |
      v
source          (codegen + emitted symbol/aliases + producer build identity)
```

Each `Node` records its `stage`, the exact `producer` identity string for
that stage, a small map of `inputs` (already-digested strings — never raw
guest bytes, so the canonical form stays a bounded size) and the keys of any
`dependencies` it built on. `Node::key()` is the SHA-256 (real FIPS 180-4
SHA-256, not a placeholder) of `Node::canonical()`, a deterministic JSON
serialization sorted by field name.

`graph::Versions` names each stage's contract version (`decoder`, `analysis`,
`semantics`, `ir`, `codegen`). Bumping the wrong one is a common incremental-
cache bug class, so Gen 10 does not trust a developer to remember every time
a pass's actual behavior changes: `XENON_GRAPH_IR_PRODUCER` and
`XENON_GRAPH_CODEGEN_PRODUCER` are computed by CMake as a hash of the actual
source files for the PPC/IR/optimizer/function-compiler stage and for
`backend_cpp_aot.cpp` respectively (`CMakeLists.txt`, near `xenon_recomp`),
and are baked in as compile definitions. Editing any of those files
automatically changes the producer identity and invalidates exactly the
artifacts whose correctness depended on it — the version string can never
silently go stale.

## Store

`graph::Store` is a content-addressed cache rooted at a directory (by
default `<output>/.graph`, or `DriverOptions::graph_cache` / the
`--graph-cache` CLI flag / `xenon-prepare`'s own per-store `graph-v1`
subdirectory when driven through automatic preparation):

- `lookup(node)` reads `nodes/<node.key()>/manifest.json` + `payload`, and
  only reports a hit when the manifest's stored `request` still equals the
  node's own canonical form and the payload's digest matches the manifest's
  recorded hash. A directory existing is never itself proof of validity.
- `publish(node, bytes)` writes into a private `staging/<key>-<random>`
  directory, verifies the bytes read back identically, then renames it into
  place. A concurrent publish of the *same* key is resolved by re-reading
  whatever the winner actually published (`WorkerPool`-driven races over the
  exact same node are expected and covered directly by
  `tests/recomp/compilation_graph_tests.cpp`). A corrupt existing entry is
  quarantined rather than overwritten in place, so a reader can never observe
  a half-repaired entry mid-promotion.

This mirrors the existing `ArtifactCacheStore`'s stage/verify/promote
discipline (`docs/development/GAME_PREPARATION.md`) at the much finer grain
of one region/function instead of one whole native module.

## IR serialization

`graph::serialize_ir` / `graph::deserialize_ir` round-trip every field of
`cpu::ir::Function` (blocks, successors/predecessors, every instruction's
op/type/result/immediates/guest provenance/args) through a small versioned
binary format. Deserialization re-verifies the round trip
(`serialize_ir(out) == bytes`) and runs the existing `cpu::ir::Verifier`
before accepting a cached IR node — a bit flip or truncation is refused, not
silently accepted as "probably fine" (`tests/recomp/compilation_graph_tests.cpp`
covers a full round trip plus truncated/out-of-schema rejection).

## Driver integration

`analyze_function_candidate()` (`src/recomp/driver.cpp`) now calls
`graph::compile_region()` instead of invoking `cpu::StaticFunctionCompiler`
directly. The region's guest ranges are hashed into the `decoded-region`
node; the CFG boundary Gen 6 discovery actually proved for that region is the
`analysis-cfg` node's only input (so widening or narrowing the same
function's proven range is a real cache miss, but two structurally identical
functions at different addresses still legitimately share IR — the codegen
`source` node is what carries per-symbol identity; see below). On a cache hit
the previously verified IR is deserialized in place of re-lifting, and
`DiscoveredFunction::ir_cache_hit` records that fact for diagnostics
(`load_and_analyze()`'s progress/diagnostics and Gen 10's own tests both
observe it directly).

`generate_project()`'s per-function codegen loop replaced the old
guest-address-sharded `.cache/<hash>_<address>.cpp` file cache with a
`source` graph node keyed on the function's exact IR digest, its emitted
symbol name and its alternate-entry aliases, plus the codegen producer/CPU
semantics identity. Because the emitted *symbol* is part of the key, two
different functions that happen to compile to byte-identical IR (a bare
`blr`, a trivial `li r3,0; blr` thunk — extremely common across a real
title) still each get their own correctly named cache entry; because the
address itself is deliberately *not* part of the key, the same normalized
region compiled at a different address (a title update relocating code, or
in principle a second title sharing a CRT/engine routine) can still reuse
the cached IR and source text. `compile_region()`'s own test asserts the
converse safety property directly: two regions built from the same
instruction words at two different base addresses receive different
`ir`-stage nodes, because the region's *dependencies* input is not the
literal words but their digest keyed together with the base-relative shape —
address-bearing content never gets mis-treated as position-independent.

Every publish/generation run writes a deterministic `compilation-graph.json`
manifest (`options.output/compilation-graph.json`) listing, per compiled
address, every node in its dependency chain plus its final `source` node —
byte-identical across a fully-cached rerun regardless of worker-count or
scheduling order, which is itself asserted by
`tests/recomp/compilation_graph_tests.cpp` (varying `analysis_jobs`/
`codegen_jobs` between runs must not change the manifest, the emitted shard
text, or `registry.cpp`'s mtime on a fully-cached rerun). A run's `[Graph]`
progress line additionally reports IR/source hit and miss counts and total
reused bytes, so "did the cache actually do anything" is a visible number,
not an assumption.

## Native object cache

Gen 10 does not stop at the generated C++ text. `tools/compilation_cache.py`
is a small, dependency-free `CXX_COMPILER_LAUNCHER` (wired into every
generated project's `xenon_game` target by `generate_project()`) that caches
the actual compiled object file:

- it always re-runs the real preprocessor (`/EP` / `-E`) before hashing, so a
  changed header can never be hidden by a stale dependency list;
- the request hashed for the cache key includes the preprocessed text, the
  exact command-line options (with the source path itself normalized out so
  the same translation unit compiled to a different output path still hits),
  and a **content hash of the compiler and its adjacent helper binaries**
  (`cl.exe`/`clang-cl.exe` and siblings on MSVC, `cc1plus`/`as` on
  GCC/Clang) — a toolchain upgrade is a real cache miss, never a silently
  stale reuse;
- it fails open to a real compile whenever it sees something outside its
  verified contract (PCH, `/Zi` program databases, `/GL`, LTO, response
  files, `-fmodules`, and similar) rather than guessing;
- promotion uses the identical stage-verify-atomic-rename-with-quarantine
  discipline as `graph::Store`, implemented independently in Python
  (`tests/recomp/native_cache_tests.py` covers concurrent promotion,
  corruption detection and a full real-compiler round trip: a cache hit
  reproduces the exact object bytes, a header change or an added `-D` flag
  is a real miss, and a corrupted cached object is never reused).

MSVC builds additionally pass `/Brepro` (and link with `/Brepro
/INCREMENTAL:NO`) so two compiles of identical inputs on the same toolchain
produce byte-identical objects — a prerequisite for the object cache to be
trustworthy rather than merely "probably fine."

## Preparation identity

`ArtifactCacheKey` gained `preparation_identity`: a `graph::Node` digest
(`xenon::recomp::graph::preparation_identity()`, shared by
`tools/xenon_prepare.cpp` and by tests that need to reconstruct the identical
key) over Xenon's own `include/`, `src/`, `cmake/` trees, `CMakeLists.txt`
and `tools/compilation_cache.py`, the exact `cmake` binary, the exact
compiler binary and its directory, and toolchain-relevant environment
variables (`INCLUDE`/`LIB`/`LIBPATH` directories are walked and hashed too,
not just read as opaque strings). A prepared native module now cannot survive
a toolchain or Xenon-source upgrade by accident — the whole-module artifact
cache and the fine-grained compilation graph agree on when the ground has
shifted under them. `kArtifactAbiVersion` was bumped to 4 to invalidate
artifacts prepared before this field existed. This function lives in
`xenon::recomp::graph` (not as a private static in `xenon_prepare.cpp`)
specifically so a second caller — currently `tests/recomp/
xenon_prepare_worker_tests.cpp`'s independent key reconstruction — can never
silently drift from what production actually hashes.

## Preparation workspace

`xenon-prepare` now builds each title/config into a stable, content-addressed
*workspace* directory (`cache-root/workspaces/<hash of title+media+config+
recomp-root>`) instead of a fresh per-attempt staging directory, protected by
an exclusive `.lock` subdirectory (`PreparationWorkspace`). This is what
makes the native object cache and Ninja/CMake's own incremental build
tracking actually pay off across repeated `Play` invocations of the same
title: the nested CMake build directory itself persists, so only objects
whose real inputs changed are ever recompiled. `ArtifactCacheStore::
clean_stale_staging()` is deliberately no longer called automatically at
worker startup — with real workspace reuse, a blanket "delete every staging
directory" sweep could destroy another concurrently running preparation's
in-progress work; it remains available (and directly tested by
`tests/recomp/artifact_cache_tests.cpp`) for explicit maintenance use.

## Safety properties

- A cached `ir`/`source`/native-object entry is re-verified (digest +, for
  IR, full structural re-verification) on every read, never trusted purely
  from a directory's existence.
- Corrupt entries are quarantined, never repaired in place, so a reader can
  never observe a half-written payload.
- Position-dependent content (relocatable addresses, the emitted symbol name)
  is always part of the key that needs it; position-independent content
  (instruction shape, proven CFG) is free to be shared across addresses/
  revisions.
- A knowledge-base-only change (Gen 9) changes the whole-module
  `ArtifactCacheKey` digest (so a `Play` still knows to re-run analysis) but
  cannot, by itself, invalidate an already-correct IR/source node whose own
  actual inputs did not change — asserted directly by
  `tests/recomp/compilation_graph_tests.cpp`.
- The native compiler-launcher fails open to a real compile for anything it
  has not verified a safe contract for; it never guesses.

## Tests

- `tests/recomp/compilation_graph_tests.cpp`: digest vectors, corruption
  detection, concurrent same-key publish races, abandoned-staging rejection,
  full `load_and_analyze()`/`generate_project()` integration across varying
  worker counts, cross-revision IR reuse for an unchanged region, knowledge-
  only changes not invalidating IR/source, a codegen ABI change forcing a
  real miss, full IR round-trip plus truncation rejection, and the
  address-bearing-vs-normalized-reuse distinction for `compile_region()`.
- `tests/recomp/native_cache_tests.py`: atomic parallel promotion, corruption
  detection, and a full real-compiler round trip (cache hit reproduces exact
  object bytes; a header or flag change is a real miss; a corrupted object is
  never reused).
- `tests/recomp/artifact_cache_tests.cpp`: native-module SHA-256 verification
  on both commit and lookup, and `clean_stale_staging()`'s explicit-
  maintenance contract.
- `tests/recomp/xenon_prepare_worker_tests.cpp`: end-to-end first-build/
  no-rebuild/hint-change-rebuild/cancellation behavior through the real
  `xenon-prepare` binary, with the test independently reconstructing the
  exact `ArtifactCacheKey` (including `preparation_identity`) production
  computes.

## Deliberate limits / next work

Gen 10 makes compilation incremental; it does not by itself decide *what* to
compile across an entire game package. Gen 11 (see
`docs/recomp/GAME_INTAKE_GEN11.md`) builds directly on this generation's
content-addressed store: discovering every executable XEX module a title
ships and preparing each one gets its own independently cached artifact
through the exact mechanism described above, with no changes needed to the
store itself. DLC content classification and multi-title-update fan-out
remain outside Gen 11's scope and are not implemented yet.

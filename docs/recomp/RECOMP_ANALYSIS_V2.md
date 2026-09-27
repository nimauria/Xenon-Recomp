# Recomp Analysis V2: performance and discovery pass

## Motivation

A real Ace Combat 6 (Project Gracemeria) analysis run exposed two problems in
the Recomp Driver (`src/recomp/driver.cpp`):

1. **Performance.** `load_and_analyze()`'s function-discovery loop was a
   single sequential `while (!pending.empty())` worklist: pop the next
   address, decode, build a CFG, compile, repeat. On a ~10,527-function
   workload this saturated exactly one host CPU thread and produced almost
   no progress output, regardless of how many cores the host machine had.
2. **Coverage.** Generic (hint-free) discovery found only a handful of
   functions on the same title; the module's revision hints had to supply
   almost everything.

This pass rebuilt the analysis/codegen pipeline into an explicit, phased,
deterministically parallel design (below), and made the CLI/automatic
game-preparation tooling report progress instead of appearing frozen. It
deliberately did **not** attempt the deeper discovery-quality work (better
heuristics, indirect-call dataflow, switch-table strengthening) in the same
pass - see "Explicitly deferred" below.

## Phased pipeline

`load_and_analyze()` now runs in explicit phases:

| Phase | What |
|---|---|
| A/B | Load the XEX, build `ExecutableRangeIndex` (Part 7) - an immutable, sorted, binary-searched description of the image's sections, replacing repeated linear scans. |
| C | Collect seed candidates: entry point, exports, unwind metadata, `ModuleHint`/`AnalysisHintSetV2` data (function hints, indirect call/branch targets, switch tables, native replacements). |
| D/E | Validate seeds against the executable-range index (reject with an explicit `"seed"` unresolved entry, never silently), freeze the initial discovery-wave frontier. |
| F | **Parallel per-function analysis** (below). |
| G | Deterministic merge of each wave's worker-local results into the canonical `AnalysisReport`. |
| H | Repeat F/G in further waves until the frontier is empty (fixed point). |
| I/J | Cross-function resolution (overlap warnings, caller backfill, branch-target validation) and diagnostics computation - unchanged from before this pass, since it only reads the now-final function list. |
| K/L | `generate_project()`: parallel native code generation, then deterministic shard assembly. |

## Parallel per-function analysis (discovery waves)

The old algorithm's worklist popped one address at a time and mutated a
single global `report.functions`/`pending` state as it went - not safely
parallelizable as-is, since analyzing one function can discover new
addresses to analyze (direct calls/branches, indirect targets resolved by
hints, `FunctionChunk` children).

The new design processes the frontier in **discovery waves**:

1. Every raw candidate address is **canonicalized** (`canonicalize_candidate()` -
   redirects a `FunctionChunk` child address to its declared parent, unless
   that exact address is also an independent `FunctionHint`) and validated
   (4-byte PPC alignment - rejected candidates are counted and reported, never
   silently dropped).
2. Candidates already in the `claimed` set (every address ever handed to
   analysis, this run) are dropped; the rest form this wave's frontier,
   address-sorted (`std::set`).
3. `analyze_function_candidate()` runs for every address in the wave,
   distributed across a `WorkerPool` (or inline for `jobs=1` - **the same
   code path**, not a separate serial implementation kept in sync by hand).
   Each call reads only immutable shared state (`AnalysisContext`) and
   returns a purely local `FunctionAnalysisResult` - no worker ever mutates
   the canonical function registry.
4. Results are merged into `report` by the single calling thread, strictly
   in the wave's address-sorted order - independent of which worker finished
   which item first.
5. Newly discovered addresses become the next wave's frontier. Repeat until
   empty.

**Determinism guarantee:** `jobs=1` and `jobs=auto` run through this exact
pipeline and must produce byte-identical `AnalysisReport` content (same
functions, same `unresolved`/`warnings` content, same diagnostics) - verified
by `tests/recomp/parallel_analysis_tests.cpp`. Wave count is a property of
the discovery graph (how many "hops" of call-target discovery are needed),
not of worker count.

## Worker pool

`xenon::recomp::WorkerPool` (`include/xenon/recomp/worker_pool.hpp`) is
Xenon's first generic thread-pool/parallel-for abstraction. Its only
operation is `parallel_for(count, fn)`: run `fn(i)` for every `i` in
`[0, count)`, blocking until all are done.

**Implementation note, for future maintainers:** each `parallel_for()` call
spawns `worker_count() - 1` fresh `std::thread`s (the calling thread
participates as the remaining one) and joins them before returning, rather
than keeping a persistent pool of threads alive across calls and handing
work off via shared dispatch state. An earlier version tried the latter and,
during development on this toolchain, produced a real, reproducible
use-after-free under heavy concurrent stress (traced to a window between a
worker committing to a job pointer and that commitment being accounted for);
fixing that surfaced a second, different-looking failure under even heavier
contention. Rather than keep chasing hand-rolled dispatch-synchronization
bugs, the design was simplified to rely on `std::thread::join()`'s
happens-before guarantee directly. Thread creation cost (microseconds) is
negligible next to a wave's actual analysis/codegen work - this runs once
per discovery wave or codegen batch, not once per function.

Also avoided: `std::jthread`/`std::stop_token`/`std::condition_variable_any`'s
3-argument `wait()` and `std::latch`. These are newer, far less exercised
standard-library surfaces; this pass hit real trouble attributable to them
(a hang, then apparent stack corruption under one toolchain's `/RTC` checks)
during development on a preview MSVC toolchain (Visual Studio "2026" /
`cl 19.51`). The final design uses only `std::thread`, `std::mutex`,
`std::atomic`, and `join()`.

`resolve_worker_count()`: `nullopt`/`0` = auto (`hardware_concurrency() - 1`,
leaving one logical CPU free), `1` = deterministic single-thread mode, `N` =
explicit.

## Executable range index (Part 7)

`ExecutableRangeIndex` (`driver.hpp`/`driver.cpp`) replaces the old
`executable_section()` helper's linear scan over `image.sections` with a
sorted vector + binary search (`containing_section()`,
`is_executable_address()`, `is_mapped_address()`,
`is_aligned_ppc_address()`). Built once per analysis run, shared read-only
across every worker thread. `resolve_switch_targets()` also uses it directly
for its data-section lookup, removing a previously-separate fallback scan.

## Parallel code generation (Part 16)

`generate_project()`'s emission loop (IR -> C++ text via
`cpu::backend::CppAotBackend`, which is stateless) is parallelized the same
way: a fixed, pre-sorted list of codegen items, worker-local text output
indexed by position, then a serial pass that assembles shards in the
original deterministic order. Shard membership (`shard_NNN.cpp`) is decided
by position in the pre-sorted list, never by completion order.

The per-function source cache (`generated/.cache/`) is sharded into 256
subdirectories (first two hex characters of the function's content hash),
pre-created serially before the parallel loop. **Why:** a flat cache
directory receiving thousands of concurrent small-file creates from many
worker threads showed real, measurable filesystem-level contention on this
development machine (NTFS directory metadata locking, likely compounded by
antivirus real-time scanning - common on Windows dev/CI machines) severe
enough to make parallel codegen *slower* than serial on an I/O-dominated
synthetic benchmark. Sharding removed the single shared hot directory.

## CLI and progress reporting (Part 18)

```
recomp-driver game.xex generated [--jobs auto|N] [--quiet] [--json]
```

`--jobs` sets both `DriverOptions::analysis_jobs` and `codegen_jobs`.
Progress milestones print to stderr (never stdout, so they never pollute
`--json`/plain report output) unless `--quiet` is passed. `xenon-prepare`
(the automatic game-preparation worker the launcher drives) forwards the
same progress strings into its existing status-file phase reporting, so a
long analysis no longer looks frozen at a fixed percentage.

## Diagnostics (Part 19)

`AnalysisDiagnostics` gained: `candidate_functions_total`,
`function_candidates_rejected_nonexec`,
`function_candidates_rejected_unaligned`, `analysis_waves`,
`analysis_workers`, `functions_analyzed`, `functions_compiled`,
`invalid_ppc_sites`, `unresolved_indirect_calls`/`branches`,
`resolved_indirect_calls`/`branches`, `analysis_duration_ms`,
`codegen_duration_ms`. All are purely additive to `analysis.json`'s
`diagnostics` object. Each function entry in `analysis.json` also gained a
`"sources"` array (every `DiscoverySource` that contributed to that function
being found - Part 9's "the analyzer should be able to explain WHY a
function exists").

## Synthetic benchmark (Part 20)

`tests/recomp/recomp_analysis_benchmark.cpp` (built when
`XENON_BUILD_BENCHMARKS=ON`, not registered as a ctest - it prints timing,
not pass/fail) builds a large synthetic XEX with thousands of independent,
hint-seeded functions (no copyrighted content - each function body is a run
of `ori r0,r0,0` ending in `blr`) and times `jobs=1` vs `jobs=auto`,
asserting identical function/compiled counts between the two runs.

Measured on this development machine (20 logical CPUs, Release build):

| Functions | Words/fn | Analysis (jobs=1 → auto) | Codegen (jobs=1 → auto) | Overall speedup |
|---|---|---|---|---|
| 3,000 | 20 | 83ms → 24ms (3.5x) | 195ms → 261ms | 0.98x |
| 10,000 | 20 | 364ms → 120ms (3.0x) | 573ms → 423ms (1.35x) | 1.72x |
| 10,527 | 60 | 801ms → 251ms (3.2x) | 511ms → 706ms | 1.37x |

**Honest read:** the analysis phase - the bottleneck this pass exists to
fix - shows a consistent, real 3-3.5x speedup at this machine's worker
count, in every configuration tested. Codegen's wall-clock win is
workload-shape-dependent: at 3,000 trivially-cheap synthetic functions, the
fixed cost of thread creation and small-file I/O exceeds the actual
`emit_translation_unit()` compute, so parallelism doesn't pay off (even
after the cache-directory sharding fix above); at 10,000 functions it does.
Every synthetic function here is close to the cheapest possible valid
PPC body (one instruction repeated); real game functions carry substantially
more decode/lift/optimize/emit work per function, which should shift this
ratio further in parallelism's favor. This was not independently verified
against a real title in this pass (see "Real-title validation" below).

## Real-title validation

A live baseline analysis of Ace Combat 6 (pre-this-pass Xenon) was running
for the entire duration of this pass and was deliberately left undisturbed
(no source, build, or output directory it used was touched). A direct
before/after comparison against that exact title with the multicore
analyzer therefore was **not** performed in this pass and should be done as
a follow-up once that baseline is free to compare against, per Part 24.

## Explicitly deferred

Consistent with the "implement the safe portion fully and explicitly
document what remains" principle: this pass did **not** implement Parts
6/8/10-15/17 of the originating spec (deeper generic-discovery-quality
work): stronger heuristic function-start detection beyond what already
existed, constant-propagation-based indirect-call/`mtctr` resolution,
stronger generic switch-table pattern detection, `invalid-ppc` failure-mode
classification (decoder-gap vs. genuinely-invalid vs.
valid-but-unsupported), an analysis result cache keyed on XEX
hash/hint-set hash for incremental re-analysis, and CMake-level guarantees
about host-compiler parallelism beyond already-independent shard
translation units. Each of these is real, substantive, higher-risk work
(false positives in heuristic function-start detection are dangerous - they
can make data look like code) that deserves its own dedicated pass with its
own real-title validation, not a rushed addition here.

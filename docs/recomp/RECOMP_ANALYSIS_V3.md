# Recomp Analysis V3: discovery-quality pass

Builds on [RECOMP_ANALYSIS_V2.md](RECOMP_ANALYSIS_V2.md) (worker pool, parallel
discovery waves, parallel codegen - unchanged by this pass except where noted).
This pass strengthens **generic** Xbox 360 static discovery: more of a title's
function starts should be findable from the executable itself, so module
hints are needed only where generic evidence genuinely cannot decide.

## Provenance and confidence (Part 1/11)

`DiscoveredFunction::sources` (a `std::vector<DiscoverySource>`, already
present) now carries genuinely richer evidence:

- Every runtime-discovered candidate (a direct call target, a resolved
  indirect target, a jump-table recovery, a FunctionChunk-internal
  reference) is tagged with **why** at the exact call site that discovered
  it, carried through the wave engine's `FunctionAnalysisResult::discovered`
  (now `(address, DiscoverySource)` pairs, not bare addresses) into a
  cross-wave `discovered_evidence` accumulator (mutated only by the single
  calling thread between waves - the same safety contract `seeds`/
  `known_names` already had).
- A candidate reached by more than one independent path (e.g. both a direct
  call from one function and a validated tail call from another) carries
  **all** of them.
- New `DiscoverySource` values: `TlsCallback`, `ResolvedIndirect`,
  `ValidatedTailCall`, `PrologueHeuristic`.
- `confidence_for_sources()` (`driver.hpp`/`driver.cpp`) replaces the old
  single arbitrary "85 or 70 depending on one internal-branch check" with an
  evidence-tiered model (`discovery_source_base_confidence()`): entry point/
  module hint/verified unwind metadata/TLS callback are strongest; a bare
  heuristic pattern with nothing else corroborating it is weakest. Two or
  more independent sources add a small corroboration bonus. The old
  internal-branch-boundary signal is kept as a secondary confidence
  *penalty*, not the primary driver.
- `analysis.json`'s per-function entries already gained a `"sources"` array
  in V2; it now reflects this richer provenance automatically (no format
  change needed).

## Stronger generic XEX discovery (Part 2)

Added: a XEX TLS directory callback address (`XexImage::tls->callback_address`)
is now seeded with very-high-confidence `DiscoverySource::TlsCallback` - a
real, generic, loader-invoked entry point (thread attach/detach) that no
decode-time scan could ever find on its own, and no title needs a hint for.

Audited and found **not currently a source of bad seeds**: XEX relocations
(`XexImage::relocations`) are parsed by XEX Loader V2 but never consumed by
the Recomp Driver for seeding at all - Part 6's "relocation parsing
mistakes" concern does not currently apply to this codebase's architecture,
since relocations simply aren't a discovery input yet. Exports, the entry
point, and unwind metadata were already used (V1/V2); import thunks are
resolved as call targets naturally via the existing import/export machinery
elsewhere in Xenon, not as a direct recomp-analysis seed source.

## Suspicious-seed audit (Part 6)

V2 already added the core fix this part asks for: **every** candidate
address, from **every** source (seed or runtime-discovered, hint-provided or
generic), passes through one shared validation point in the wave engine's
per-wave canonicalization step (`ExecutableRangeIndex::is_aligned_ppc_address()`
+ `is_executable_address()`) before ever reaching decode - a misaligned or
non-mapped/non-executable candidate is rejected there with an explicit,
counted diagnostic (`candidate-unaligned` / `seed` unresolved kinds;
`function_candidates_rejected_unaligned` / `_nonexec` diagnostics), never
silently dropped or silently analyzed. This pass re-verified that invariant
holds for every new seed/discovery source added (TLS callbacks, CTR-dataflow-
resolved targets, jump-table recoveries) and added direct test coverage
(`discovery_quality_tests.cpp`, "Misaligned and non-executable candidates").

## Direct call graph discovery (Part 3)

Already strong from V2's wave engine: every statically proven `bl` target
feeds the next discovery wave exactly once (deduplicated via the `claimed`
set), with no repeated re-decoding of already-known targets. This pass adds
no changes here beyond the provenance tagging described above.

## PPC function-start heuristics (Part 4)

Implemented **conservatively, as supporting evidence only**:
`looks_like_function_prologue()` recognizes the two most common Xbox 360/PPC
compiler prologue openings - `stwu r1,-N(r1)`/`stwux r1,r1,rB` (stack frame
allocation) and `mflr r0` (link register save) - and, **only for a candidate
already being analyzed via some other discovery path**, tags its first
instruction's match as `PrologueHeuristic` supporting evidence.

This deliberately does **not** independently scan executable sections
looking for prologue-shaped bytes to invent new candidates from scratch -
the task's own false-positive warning ("data can look like a prologue too")
applies with full force to that use, and Xenon has no way to verify a
prologue match against real compiled Xbox 360 output at the scale needed to
trust it as a standalone discovery source. As corroborating evidence for an
address already reached some other way, the risk is bounded to nothing worse
than a confidence number; test coverage explicitly proves the heuristic
alone never creates a function.

## Invalid vs. unsupported PPC (Part 5)

`classify_decode_failure()` uses Xenon's own opcode catalog (already the
single source of truth for what the decoder recognizes) to distinguish:

- **`invalid-ppc`**: the word's primary (6-bit) opcode has **zero** cataloged
  entries at all - almost certainly not PPC code (embedded data, padding).
- **`unsupported-ppc`**: the primary opcode **is** cataloged (real
  instructions exist under it) but this specific bit pattern matches none of
  them - a real PPC instruction family Xenon's decoder doesn't yet implement
  this exact encoding of.
- **`unsupported-vmx`**: `unsupported-ppc`, narrowed to primaries whose
  cataloged entries are Vector-group (VMX/VMX128's Xbox-specific encoding
  space is the largest known decoder gap).

New diagnostics: `unsupported_ppc_sites`, `unsupported_vmx_sites` (alongside
the existing `invalid_ppc_sites`). Runs only on the decode-failure path
(rare), never in the per-instruction hot loop.

Categories from the originating task's wishlist deliberately **not**
separately implemented, because PPC's fixed 4-byte instruction width makes
them either inapplicable or already covered by an existing category:
"mid-instruction target" (not a meaningful concept for a fixed-width ISA -
every aligned address is a valid instruction boundary), "malformed
instruction bytes" vs. "invalid PPC" (the same thing here - there is no
separate notion of "malformed but not simply unrecognized"), "bad image/
section mapping" (covered by the existing non-executable/unmapped seed
rejection, already counted separately).

## Bounded indirect-call dataflow (Part 7)

A small, deliberately conservative local constant-propagation tracker runs
inline during each function's own linear decode scan:

- `addi`/`addis` and `ori`/`oris` update a per-register tracked-constant
  table (`gpr_constant[32]`), correctly handling that `ori`/`oris` has its
  source and destination register fields **reversed** relative to
  `addi`/`addis` (verified against `src/cpu/ppc/lifter_integer.cpp`'s own
  field usage, not just PPC ISA manual convention, since a mismatch here
  would statically resolve to a **wrong** address rather than merely fail to
  resolve).
- `mtspr` targeting SPR 9 (CTR) captures the current tracked value of its
  source register into `ctr_constant`.
- **Any other instruction** (including branches) invalidates every tracked
  register and `ctr_constant` unconditionally. This is deliberately far more
  conservative than a real dataflow/CFG analysis would need to be: it only
  ever resolves a target materialized by an **uninterrupted** run of
  whitelisted instructions immediately feeding `mtctr`, exactly the
  `lis/ori/mtctr/bctrl` shape real compilers emit for a function-pointer
  call with no unrelated work scheduled in between. A stale tracked value
  can never leak across an instruction this analysis does not understand.
- At an unresolved `bcctrx` site (CTR-based indirect call or tail branch),
  if `ctr_constant` is set **and** independently passes the same
  alignment/executability validation every other candidate does, it
  resolves the target with `DiscoverySource::ResolvedIndirect` (never
  applied to `bclrx`/LR-based indirect sites, which this dataflow does not
  track).

New diagnostic: `resolved_indirect_via_dataflow`, distinct from
hint-resolved `resolved_indirect_calls`/`resolved_indirect_branches`.

**Found and fixed a real ordering bug during development**: the tracker was
originally updated *before* the current instruction's own branch-resolution
logic ran, which meant `bcctrl` itself (not a whitelisted instruction)
invalidated `ctr_constant` before it was ever read. Fixed by moving the
tracker update to run *after* branch handling, so a site's resolution always
sees state left by *prior* instructions, never itself.

**Explicitly not attempted**: unbounded/CFG-aware symbolic execution,
resolving vtable-style loads (`lwz` from a computed base), or table-of-
function-pointers dispatch. The task explicitly warns against unlimited
symbolic execution and fabricated targets; those would need real
alias/points-to reasoning this pass does not build.

## Switch/jump-table discovery (Part 8)

Strengthened, not rewritten: the existing "scan the 256 bytes after an
unresolved indirect branch for executable-looking pointers" heuristic now
only runs when a `cmp`-family instruction appeared within the last 8
instructions before the branch (`instructions_since_compare`) - real
compiler switch dispatch compares the index against a bound shortly before
branching; recovering "targets" from data with no such corroborating
evidence nearby is exactly the "arbitrary data that merely resembles
pointers" the task warns against. This makes the heuristic **strictly more
conservative** than before (fewer, better-evidenced recoveries), confirmed
directly by two new tests (fires with a nearby compare, does not without
one). Recovered targets are now tagged `DiscoverySource::ResolvedIndirect`
(previously untagged/defaulted).

Module-hint-based `SwitchTableHint`/`KnownIndirectBranch` resolution
(unchanged from V1/V2) remains the authoritative path when a module supplies
it; this generic heuristic only ever runs as a fallback when no hint
resolves a site.

## Function chunks / shared tails (Part 10)

Unchanged - the existing `FunctionChunk` machinery (hint-driven; a chunk is
merged into its parent's own IR, never emitted as a separate overlapping
function) was audited and found adequate; the 11 pre-existing hint-schema
tests, including the FunctionChunk integration test, all still pass
unmodified. This pass adds `ValidatedTailCall` evidence for the *generic*
(non-chunk, non-hint) case: a non-linked branch whose target is provably
another function's own start address (not an internal jump - a real chunk
never appears as a separate `DiscoveredFunction` to match against).

## Tests (Part 17)

`tests/recomp/discovery_quality_tests.cpp` (10 tests): evidence-combination
confidence, multi-source provenance, TLS callback seeding, CTR dataflow
resolution (positive and negative - a memory-loaded CTR value correctly
stays unresolved), invalid-vs-unsupported PPC classification,
prologue-heuristic-as-supporting-evidence-only, switch-table bounds-check
gating (positive and negative), and unaligned/non-executable candidate
rejection. All 11 pre-existing Analysis Hint Schema V2 tests and all V2
threading/determinism tests continue to pass unmodified.

## Performance (Part 14)

Re-ran the V2 synthetic benchmark after every change in this pass; analysis
and codegen wall-clock characteristics are unchanged within normal run-to-
run variance (still ~3x analysis speedup at this machine's worker count).
The new per-instruction work (constant tracker update, compare-recency
counter, decode-failure classification) is O(1) or bounded-small-catalog per
instruction, never O(candidate count²) or repeated-section-rescan.

## Tail-call over-discovery fix (targeted correctness pass)

Real-title validation against Ace Combat 6's Gracemeria mission (~10,527
reference function starts) exposed a serious over-discovery bug in the
scheme described above: `auto_discovered_functions` came back at 42,104 -
roughly 4x the reference count - with ~35,409 of those generated functions
carrying exactly `direct-branch + validated-tail-call` provenance and
~26,261 generated function starts landing inside the address range of
another discovered function.

**Root cause**: `analyze_function_candidate()`'s per-instruction scan pushed
the target of *every* plain (non-linked) direct branch - `b`/`bc` alike,
conditional or unconditional, terminal or not - straight to
`result.discovered` (tagged `DirectBranch`) the moment the branch was
decoded, with no check against the scanning function's own extent (not yet
known at that point in the scan) and no distinction between a genuine
tail call and an ordinary intra-procedural edge (if/else, loop back-edge,
shared-epilogue jump). Every such target became its own independently
claimed, independently compiled `DiscoveredFunction`, duplicating bytes the
originating function's own linear scan already covered (hence the address-
range overlaps) and, once the post-wave cross-reference pass ran, picked up
`ValidatedTailCall` too purely because the originating function's own
`branch_references` happened to name it (hence the `direct-branch +
validated-tail-call` pattern dominating the discovered set).

**Fix** (`src/recomp/driver.cpp`, `analyze_function_candidate()`):

1. A plain direct branch's target is no longer pushed to `result.discovered`
   at decode time. It is buffered (`pending_direct_branch_targets` for the
   primary scan, `pending_chunk_branch_targets` for FunctionChunk-internal
   branches) alongside whether it was the branch that terminated the scan
   (`is_terminal()` - already existed, just consulted earlier and reused).
2. Once the function's complete extent is known - primary range plus every
   FunctionChunk merged into it (`function.ranges`, finalized as before) -
   each buffered target is filtered:
   - **Non-terminal branch**: never promoted. A branch that leaves a live
     fallthrough path is intra-procedural control flow by construction
     (conditional branches, and any non-terminal encoding of an
     "unconditional" opcode); its target is still recorded in
     `branch_references` for diagnostics/cross-referencing, just never
     independently claimed. A terminal direct branch whose forward target was
     excluded only because the scan stopped at the branch is instead scanned
     through that target so the target becomes a local block; ordinary
     inferred tail-call targets remain independently discovered.
   - **Target inside the function's own established extent** (including its
     chunks): never promoted, regardless of terminal-ness - covers short
     "infinite loop" back-edges and any other jump that lands in bytes the
     function's own scan already covers.
   - **The discovering function itself lacks independent evidence**
     (recursive-explosion guard, see below): never promoted.
   - Otherwise: promoted exactly as before (`DirectBranch`).
3. **Recursive-explosion guard**: a candidate whose own evidence, at the
   point it is claimed, is nothing but a plain direct branch (no seed -
   entry/export/module-hint/unwind/TLS-callback - no direct call, no
   resolved-indirect proof, and no independently recognized compiler
   prologue at its own first instruction) does not get full "discovery
   authority": its *own* direct-branch targets are recorded but not
   themselves promoted to new candidates, so a chain of unconfirmed
   heuristic guesses cannot recursively manufacture unbounded numbers of
   functions. The candidate itself is still compiled and reported - never
   silently dropped - and a denied target still surfaces through the
   existing `branch-into-unknown-code` diagnostic if nothing else claims it.
   Any independent evidence (including a matched prologue) keeps full
   authority exactly as before. This check necessarily uses evidence as of
   *claim time*, before the post-wave `ValidatedTailCall` cross-reference
   pass ever runs (that pass is retroactive and global, not available
   mid-analysis).

## CFG closure and code-generation ownership

The exclusive end of a function is not a proof that a reachable direct
successor is another function. While a candidate is being scanned, a direct
executable target equal to the current tentative end is materialized and
decoded into the candidate unless that address already has independent
function evidence (a seed, unwind entry, or explicit `FunctionHint`). The
scan reaches that successor before `guest_end` and `ranges` are finalized.
This closes the CFG before ownership boundaries become hard; it prevents an
emitted branch from pointing at the exclusive end of its own function with no
owner. A target with independent evidence remains a cross-function transfer.

Tail-call promotion is separate from this closure step. Crossing an inferred
boundary is not evidence of a tail call; promotion still requires the target
to be independently established by the normal discovery evidence model.

Immediately before native code generation, Xenon validates every non-local
branch or fallthrough edge in compiled IR. Each target must be a local CFG
block or the entry of a compiled function/chunk. A speculative unresolved
candidate is not fatal merely because it appears in analysis; it becomes fatal
only if code generation would emit executable control flow to it.
4. **Unresolved diagnostic deduplication**: `report.unresolved` is sorted
   and deduplicated by `(kind, address, target, detail)` identity once, after
   every entry is final, before diagnostics counters are computed. The same
   physical site can legitimately be recorded more than once while being
   produced (e.g. the same branch target referenced by several different
   branch instructions within one function's `branch_references`), and that
   repeated emission of the same finding must not inflate top-level counters.
5. **Resolved-indirect provenance/diagnostics reconciliation**: added
   `resolved_indirect_via_jump_table` (mirrors `resolved_indirect_via_dataflow`
   but for Part 8's switch/jump-table recovery, which previously incremented
   no counter at all despite tagging `DiscoverySource::ResolvedIndirect` just
   like the dataflow path) and `functions_with_resolved_indirect_provenance`
   (the distinct-function count directly comparable against the sum of the
   two `_via_*` event counters). `resolved_indirect_calls`/
   `resolved_indirect_branches` remain their own, deliberately separate
   concept - module-hint-declared resolved sites, never conflated with
   Xenon's own generic proof - now documented explicitly as such in
   `driver.hpp`.

**What this does not do**: it does not change `FunctionChunk` handling
(`canonicalize_candidate()`'s chunk-owner redirect, already correct, is
untouched), does not change how direct calls (`bl`) or hint/dataflow/jump-
table-resolved indirect targets are discovered (always trusted, as before),
and does not add any AC6-specific address or rule - every check here is a
property of the branch instruction and the scanning function's own state.

**Tests**: `tests/recomp/discovery_quality_tests.cpp` gained six tests
(conditional-branch-stays-internal, terminal-back-edge-to-mid-function-
address-stays-internal, shared-epilogue-preserved-and-corroborated, weak-
candidate-preserved-but-denied-authority-and-deterministic-under-jobs=1-vs-
8, unresolved-diagnostic-deduplication, resolved-indirect-dataflow- and
jump-table-reconciliation);
`tests/recomp/analysis_v2_consumption_tests.cpp`'s existing FunctionChunk
integration test gained a regression assertion that a chunk's branch back
into the parent's own primary range does not spawn a duplicate function
(the pre-existing fixture already exercised the bug; the assertion just
didn't check for it).

## Explicitly deferred

Consistent with "implement the safe portion fully and explicitly document
what remains" (not faked, not silently dropped):

- **Part 9 (function pointer tables / vtables)**: not implemented. Requires
  real points-to/structure analysis to safely distinguish a genuine callable
  table from coincidental data (the task's own repeated warning against
  promoting data to code applies most strongly here); deserves a dedicated
  pass with real-title validation, not a rushed heuristic.
- **Part 12 (function fingerprint foundation)**: this was deferred during the
  V3 pass. It is now implemented by **Gen 9 - Universal Knowledge Base**; see
  `docs/recomp/UNIVERSAL_KNOWLEDGE_GEN9.md` for the versioned normalized
  instruction/entry/CFG/constant/call-neighbourhood fingerprint design.
- **Part 13 (analysis cache)**: not implemented. Xenon already has two
  adjacent, working caches at different granularities - the whole-module
  `ArtifactCacheStore` (compiled shared library, keyed by executable+hint
  identity) and `generate_project()`'s per-function generated-C++-source
  cache (keyed by content hash) - both from before this pass. A dedicated
  discovery-metadata cache (function starts/provenance/diagnostics, skipping
  re-*discovery* on an unchanged input) is real, additive work distinct from
  either; not attempted here given the size of everything else in this pass.
- **Part 16 (cross-title learning prep)**: this was deferred during the V3
  pass and is now implemented by Gen 9's JSONL knowledge records,
  confidence-scored matching, cross-revision anchor nomination and source
  provenance.
- **Part 18 (real-title validation)**: blocked. The AC6 baseline analysis
  was running for this entire pass and was deliberately left untouched.
  Should be run as a direct follow-up: compare `analysis.json`'s
  `auto_discovered_functions`, `candidate_functions_total`,
  `resolved_indirect_via_dataflow`, `unsupported_ppc_sites`/
  `unsupported_vmx_sites`, and the new per-function `sources` provenance
  against Project Gracemeria's ~10,527 known function starts, with and
  without hints supplied, exactly as the task's comparison-report
  requirements describe.

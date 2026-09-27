# Gen 5 — Adaptive analysis hardening

This pass hardens the Region + Entry architecture before another full-title rebuild. It intentionally keeps title-specific addresses out of production logic.

## Design ideas reused and generalized

- **Xenia:** resolve executable guest addresses independently from perfect semantic function recovery, and avoid aggressive forward-branch tail-call heuristics. Xenon keeps function identity separate from dispatchable guest entries and now records enough edge semantics to keep conditional branches from becoming tail calls during reconciliation.
- **ReXGlue / recomp projects:** retain graph authority, block ownership, gap recovery and pre-codegen validation, but make the recovery generic rather than shipping large title-specific boundary tables.
- **Dead Rising recomp:** feed runtime-discovered targets back into later static passes, prune false loop-header/function discoveries, and repair dropped control-flow edges. Xenon applies those ideas as revision-scoped observations and Region + Entry ownership rather than editing a per-game function list.
- **N64Recomp:** indirect calls are runtime address lookups rather than proof that every target had to be a statically perfect function. Xenon's alternate-entry registry follows the same address-oriented principle while remaining AOT.
- **angr CFGFast:** indirect-jump resolution is a bounded pipeline with explicit target limits and a distinction between aggressive tail-call detection and ordinary CFG recovery. Xenon's feedback parser and runtime recorder are now bounded; tail-call promotion uses exact edge provenance.
- **rev.ng:** user/model facts and generated artifacts are separate, and model changes invalidate only dependent cached artifacts. Xenon similarly keeps learned observations as evidence, adds an analysis-engine revision to the configuration hash, and versions generated reports separately from the module hint schema.
- **LLVM BOLT:** execution profiles are treated as evidence that can become stale; BOLT fingerprints blocks before attempting stale-profile matching. Xenon currently takes the safer first step — exact effective-image scoping — and does not reuse runtime observations across revisions unless a future block-fingerprint matcher can prove equivalence.
- **XenosRecomp:** preserve arbitrary control-flow semantics first, then flatten/optimize only when safe. Region + Entry follows the same correctness-first rule for CPU CFGs.

## Changes in this pass

1. Generated analysis contract now has `report_schema_version = 3` and `analysis_engine_revision = 5`. The analysis-engine revision is part of the configuration hash; native artifact ABI is bumped to 3.
2. `BranchReference` now records `linked`, `conditional` and `fallthrough` in addition to site, target, terminal and indirect. Direct/indirect calls are included in provenance, while function-boundary recovery explicitly ignores linked edges.
3. `ValidatedTailCall` promotion requires a non-linked, non-indirect, terminal, non-conditional edge. Tentative extent crossing alone remains insufficient.
4. Runtime adaptive observations are accepted only for the exact effective-image SHA-1 when a hash is present. Mismatched revisions and invalid/non-executable targets are counted separately.
5. Observation ingestion accepts UTF-8 BOM, validates 40-character SHA-1 values, limits line size to 64 KiB, and caps a trace at one million lines. Runtime recording is capped at 65,536 distinct facts per session.
6. The shared JSON parser accepts an optional UTF-8 BOM, fixing PowerShell 5.1 JSON globally rather than in one caller.
7. Region/entry integrity validation runs before C++ emission: unique entries, real canonical owners, materialized alternate-entry blocks, and dispatchability of every non-linked edge that leaves its owner.
8. Root generated project files are written into `.generation-stage` and only promoted after validation/codegen succeeds. `generation-status.json` is written `complete:false` before work starts and includes the failed phase; the previous successful `analysis.json` remains untouched on failure.
9. Reports expose semantic-function, compiled-region, guest-entry, alternate-entry, rejection and integrity metrics so full-title before/after analysis is measurable.
10. Regression coverage now includes BOM JSON, exact-revision adaptive learning, and transactional failed regeneration in addition to the existing boundary/shared-suffix/switch-tail/orphan tests.

## Deliberately deferred

Cross-title-update reuse of learned observations is **not** enabled yet. BOLT-style block fingerprints are a promising future extension, but exact image identity is safer until Xenon can prove a stale observation still points at byte/CFG-equivalent code.

# Title Update Fidelity Report — AC6 Runtime Readiness pass, Part 12

Part 12 asked to audit the full title-update path (base XEX -> TU discovery
-> TU identity -> header delta -> image delta -> effective image ->
analysis -> codegen -> cache key -> runtime metadata), ensure analysis and
native artifacts are keyed to the effective post-TU image with no base/TU
cross-contamination, and add a report: "Base SHA1, TU identity, Effective
SHA1, Effective version".

## Audit result: the path itself was already correct

`xbox::apply_title_update()`/`compute_effective_identity()`
(`src/xbox/xex/xex_loader.cpp`) and `XenonSession::load_game()`
(`src/core/session.cpp`) already: parse the base image first, apply the
update through XEX Loader V2's canonical patcher when one is selected, map
*that* effective image (never the base) for execution, and reject a
malformed/incompatible update as an explicit, hard `load_game()` failure -
never a silent fallback to the base XEX. `effective_identity()` (the
existing `xbox::XexEffectiveIdentity`) is what native-extension module
compatibility gating and `xenon::recomp::generate_project()`'s emitted
`Xenon_SupportedExecutableRevisions()` declaration both key off - i.e.
analysis/codegen keying to the effective image, not the base, already held
before this pass. `tests/core/title_update_integration_tests.cpp` already
covered this exhaustively (base-only launch, valid TU applied, malformed TU
rejected with no partial state, title/media ID mismatch rejected, native-
extension revision compatibility) - all real, passing tests.

## What this pass added: the report itself did not exist

`XexEffectiveIdentity` had `effective_image_hash` but no separate hash for
the *base* image - a caller could reconstruct it by calling
`compute_effective_image_hash()` on the base image again, but nothing
packaged "Base SHA1 vs Effective SHA1" as one report. Added
`XexEffectiveIdentity::base_image_hash` (computed in
`compute_effective_identity()`, equal to `effective_image_hash` exactly
when no update was applied) and a new `capability_report()` section,
`"titleUpdate"` (omitted until a title is loaded, matching `"gpu"`/
`"shader"`'s convention): `baseSha1`, `effectiveSha1`, `titleUpdateApplied`,
`tuIdentity` (`"<base_version>+<effective_version>"` or `"none"`,
matching `RunFingerprint`'s existing convention), `effectiveVersion`.

This is deliberately a separate section from `RunFingerprint`'s own
`effective_xex_sha1`/`tu_identity` fields - `RunFingerprint` intentionally
only describes what actually ran (for tracing a checkpoint/crash report
back to an exact combination of inputs); `"titleUpdate"` is the two-hash
comparison Part 12 specifically asks for.

## Tests

- `tests/core/title_update_integration_tests.cpp`: `test_base_only_launch`
  asserts `baseSha1 == effectiveSha1` and `tuIdentity == "none"`;
  `test_valid_title_update_applied` asserts `baseSha1 != effectiveSha1`,
  both hashes match independently-computed reference hashes, and
  `tuIdentity`/`effectiveVersion` show the real `1.0.0.0`/`2.0.0.0`
  base/update versions from the test fixture.

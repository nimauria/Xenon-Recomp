# AC6 Capability Report Verdict — AC6 Runtime Readiness pass, Part 17

Part 17 asked for a unified aggregator tying CPU/imports/GPU/runtime
diagnostics into one integrity verdict, and the reviewer's addition asked
for that verdict to be a three-state `PASS` / `PASS_WITH_FALLBACK` / `FAIL`
rather than a binary pass/fail.

## What existed before this pass

`XenonSession::capability_report()` already published independent sections
(`runFingerprint`, `fallback`, `gpu`, `shader`, `titleUpdate`, `boot`), each
built from real, live counters/state a subsystem maintains for its own
purposes (Parts 7, 9, 12, 14, 15 of this pass). Nothing combined them: a
caller had to know to look at six different sections and interpret each
one's fields itself to answer "did this run actually work?".

## What this pass added

A new `"verdict"` section, computed at the end of `capability_report()`
purely by reading the sections/state already assembled above it - it
introduces no new telemetry of its own, so it can never read cleaner than
the data it is built from:

```
"verdict": {
  "state": "PASS" | "PASS_WITH_FALLBACK" | "FAIL",
  "failReasons": [ "..." ],
  "fallbackReasons": [ "..." ]
}
```

**FAIL** - reserved for signals meaning part of the run could not execute
at all:
- `state() == SessionState::Failed` (a session-level failure was actually
  recorded, via `set_error()` - e.g. an unrecognized graphics backend, a
  main-thread trap, or any other path that already calls `set_error()`).
  The real `last_error()` message is carried into `failReasons`, not just
  a bare "failed" flag.
- A title was loaded (`loaded_xex_` exists) but its native compiled-code
  extension never bound (`!native_extension_bound_`) - meaning no guest
  code could execute at all.

**PASS_WITH_FALLBACK** - the run completed but took a degraded path:
- One or more unresolved imports (present, but the entry point still ran -
  distinguishing "some symbol was never called" from an actual crash,
  matching `resolve_xex_imports()`'s own pre-existing "unresolved is not
  fatal" design).
- Any guest code executed through the Gen 7 dynamic fallback instead of
  AOT-compiled code, or any unsupported PPC instruction was encountered
  (Part 14's `fallback` counters).
- Any unsupported GPU operation (Part 7's `unsupportedOperationsTotal`) or
  shader translation failure (Part 9's `shader.translationFailures`).

**PASS** - none of the above.

Every reason string names the concrete signal it came from (e.g. "3
unresolved import(s)", "12 guest code block(s) executed via the Gen 7
dynamic fallback instead of AOT"), so a caller reading only the verdict
section still gets a real, specific answer, not just a traffic light.

## Tests added

`tests/core/session_tests.cpp`:
- A freshly initialized session with nothing loaded/executed verdicts
  `PASS` with both reason arrays empty.
- A session with a directly-injected unresolved import (via a new
  `SessionExecutionTestAccess` friend, matching the existing pattern in
  `tests/core/thread_creation_tests.cpp`) verdicts `PASS_WITH_FALLBACK`,
  with a reason string that actually names "unresolved import".
- The existing "unrecognized graphics backend" failure test now also
  asserts the verdict is `FAIL` with a non-empty `failReasons`.

## Tests run

- `xenon_session_tests`: all pass, including the three new verdict
  assertions.
- Full solution rebuild + full test suite: see the pass's finish report
  for the exact run.

## Follow-up: the "imports" piece of "CPU/imports/GPU/runtime"

Part 17's own wording named imports as one of the four things to tie
together, but the first pass of this work only tied together
CPU/GPU/runtime signals - the whole-XEX import capability audit (Part
3/4's `classify_import()`/`compute_import_capability_verdict()`, already
real and tested via `tools/recomp_tools.cpp`'s standalone import-scanner
and `tests/core/import_capability_report_tests.cpp`) still required a
separate offline tool run against the XEX file; it had no live
`capability_report()` presence.

Closed in a follow-up: `capability_report()` now publishes an `"imports"`
section (omitted until a title is loaded) with `implemented`/`safeStub`/
`partial`/`missing` counts, its own `verdict` (`PASS`/`PASS_WITH_FALLBACK`/
`FAIL`, from the same already-tested `compute_import_capability_verdict()`
the offline tool uses), and a `partialNotes` array naming every import
with a documented behavior gap. The overall session `"verdict"` above was
extended to add `PASS_WITH_FALLBACK` reasons when `safeStub`/`partial` are
non-zero (previously only "unresolved/Missing" imports were represented
there, via `unresolved_imports_`).

Deliberately *not* folded in: the imports-section's own `Fail`-on-any-
Missing rule is not promoted into the overall session verdict's
`failReasons` - that would contradict `resolve_xex_imports()`'s existing,
deliberate "an unresolved-but-never-called import must not block launch"
design. The two verdicts answer different questions on purpose: the
imports-section verdict is "is this XEX's import table fully,
gap-free covered" (a static, XEX-wide question - matching the offline
tool's own purpose), while the overall session verdict is "did this
actual run work" (a live, this-session question).

This follow-up's test, in `tests/core/guest_import_negative_tests.cpp`:
`test_capability_report_imports_section_classifies_real_mix()` builds a
real XEX import table with one genuinely Missing, one genuinely SafeStub
(a synthetic `ExportRequirement::Stubbed` export), one genuinely Partial
(a synthetic export with `partial=true` and a real note), and one
genuinely Implemented (`xboxkrnl`'s real `KeQueryPerformanceFrequency`)
import, then asserts `capability_report()`'s `"imports"` section counts
and verdict are exactly right, `partialNotes` carries the real note
through, and the overall session verdict's `fallbackReasons` picks up both
the SafeStub and Partial signals. `xenon_guest_import_negative_tests`
passes in full, including this new test.

## Remaining gaps

None for Part 17's scope as now completed (CPU/imports/GPU/runtime all
tied together). The guest timebase/SPR bug noticed during this audit was
a separate, real Part 6 gap - fixed separately and documented in
`docs/cpu/GUEST_TIMEBASE_SPR_AUDIT.md`, not left as a footnote here.

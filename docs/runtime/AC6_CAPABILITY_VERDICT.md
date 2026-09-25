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

## Remaining gaps

None for Part 17's own scope. Two adjacent, pre-existing gaps were noticed
while reading `session.cpp` during this audit but are out of scope for
Part 17 and are called out separately rather than silently left alone:
`XenonSession::write_spr()` is a real no-op ("For now, do nothing") and
`read_time_base()` returns a simple incrementing counter rather than a
host-time-derived value ("In a real implementation, this would be based on
host time") - both predate this pass and belong to Part 6 ("Guest
timebase/vblank/timers"), not Part 17.

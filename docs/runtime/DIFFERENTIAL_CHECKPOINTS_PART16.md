# Differential Execution Checkpoint System — AC6 Runtime Readiness pass, Part 16

Part 16 is explicitly marked optional in the original plan. This pass's
judgment: defer it, documented here, rather than build a shallow version
that would not meet CLAUDE.md's no-paper-completion bar.

## Why deferral, not a stub implementation

A genuine differential-execution-checkpoint system needs a second,
independent oracle to diff live execution state against at each checkpoint.
This project already has exactly that kind of system - the Gen 8 semantic
verifier (`tests/cpu/verification/semantic_verifier_tests.cpp`,
`xenon_cpu_semantic_verification`, documented in
`docs/cpu/SEMANTIC_VERIFICATION_GEN8.md`) differentially checks real
generated AOT code against an independent PowerPC reference interpreter,
with deterministic seeded replay and bounded failure minimization - but it
is deliberately scoped to synthetic, randomized instruction streams, which
is exactly where an independent reference model is tractable to build and
trust.

A live AC6 boot/gameplay run has no equivalent independent oracle: there is
no second, trusted implementation of "what Xbox 360 AC6 does at boot
checkpoint N" to hash and compare against. Building a checkpoint-hashing
system with nothing genuine to diff against would produce hashes that are
merely descriptive telemetry (a state fingerprint at each
`BootCheckpoint`), not an actual differential correctness check - which is
what Part 16 asked for. Implementing that shallow version and calling it
"Part 16, done" would be exactly the paper-completion CLAUDE.md prohibits:
an interface that exists but does not perform the verification the plan
actually wants.

## What would need to exist first

A real version of this needs one of:
- A second, independently-implemented Xbox 360 execution path (e.g. an
  interpreter-mode fallback broad enough to run real guest code, not just
  Gen 8's synthetic streams) to diff the AOT path against at each
  `BootCheckpoint`, or
- A captured reference trace from real Xbox 360 hardware or another mature
  Xbox 360 emulator/recomp project's verified-correct run, to diff against
  (subject to the Research rule's licensing constraints - not something to
  copy wholesale even if available).

Neither exists in this repository today. This is a genuine, concrete
prerequisite gap, not a scheduling deferral - per CLAUDE.md, this is marked
incomplete rather than claimed done.

## Status

`STATUS: INCOMPLETE` for Part 16, by design (optional, explicitly deferred,
concrete blocking prerequisite identified above). No stub, interface, or
partial implementation was added, since a partial version here would be
exactly the "interface without implementation" / "placeholder" pattern
CLAUDE.md prohibits.

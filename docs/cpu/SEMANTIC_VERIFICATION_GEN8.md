# CPU Gen 8 — Semantic Verification

Gen 8 treats Xenon as a compiler that must be checked, not trusted.  The goal is
not to add another execution engine.  It adds a deterministic verification
surface that compares independently transcribed PowerPC semantics with the
actual C++ AOT code Xenon generates.

## Why this exists

A static recompiler can discover every function correctly and still be wrong if
one translated instruction has subtly incorrect carry, record-bit, endian,
FPSCR, vector-lane, memory, or control-flow behavior.  Those failures are
especially difficult to diagnose when they only appear in an arbitrary title.
Gen 8 turns those semantics into continuously testable compiler invariants.

The reference side is intentionally independent of both the AOT helpers and the
Gen 7 `DynamicFallbackExecutor`.  Reusing either as the oracle would make shared
bugs invisible.

## Verification pipeline

```text
fixed master seed
      |
      v
per-trial deterministic seed
      |
      +---- randomized CpuState
      |       GPR / FPR bits / VMX128 / CR / XER / CTR / LR / FPSCR / VSCR
      |
      +---- randomized guest memory image
      |
      +------------------------------+
      |                              |
      v                              v
independent PPC reference       generated native AOT
      |                              |
      +--------------+---------------+
                     v
              compare results
              - register state
              - vector/FPR state
              - CR/XER/FPSCR/etc.
              - memory bytes
              - exception class
              - flow reason/detail
              - next PC / CIA / NIA
```

The native side in `xenon_cpu_semantic_verification` is not a mock callback.  A
build-time generator decodes and lifts the corpus through Xenon's production
frontend and emits C++ through `CppAotBackend`; the host compiler then compiles
those generated functions and the verifier calls them directly.

## Bugs caught while bringing Gen 8 online

The verifier immediately exposed two state-accounting defects in the native
backend:

- an AOT `blr` returned the correct `ExecutionResult::next_address` but left
  `CpuState::nia` at the sequential PC instead of the return target;
- after optimization removed/folded the final guest instruction's surviving IR
  provenance, fallthrough code could materialize `CpuState::cia` from the
  penultimate guest instruction.

`CppAotBackend` now updates NIA on native branch/return exits and derives final
fallthrough CIA from the declared guest block range, not merely the last IR node
that survived optimization.  `sem_blr` and the short-block corpus keep both
regressions under differential test.

## Current independent reference coverage

Gen 8 starts with instruction families that give high semantic leverage while
remaining independently auditable:

- immediate integer arithmetic and logical operations;
- integer register arithmetic/logical and record forms;
- sign extension and count-leading-zero;
- signed/unsigned, word/doubleword compares;
- big-endian scalar loads/stores (`lbz/lhz/lwz/ld`, `stb/sth/stw/std`);
- bit-exact FPR move/sign operations (`fmr`, `fneg`, `fabs` with Rc=0);
- VMX bitwise `vand`, `vor`, and `vxor`;
- direct branch flow plus unlinked LR/CTR indirect control flow (`b`, `blr`, `bcctr`).

Reference coverage is fail-closed.  An instruction that has not been
independently modeled cannot be silently verified by delegating to AOT or Gen 7.
This is deliberate: a green test must mean two independent implementations
agreed.

## Short-block fuzzing

Per-instruction testing does not catch all compiler bugs.  The corpus therefore also emits short AOT blocks that exercise:

- value forwarding between instructions;
- record/CR updates after an earlier computed value;
- load -> transform -> store memory side effects;
- FPR and VMX state changes in the same translated block.

Single-instruction cases run a bounded deterministic randomized corpus and the
hand-authored short blocks run a larger, still CI-friendly count. In addition,
the build-time code generator synthesizes 24 deterministic 3–6 instruction
blocks from independently covered integer, memory, FPR and VMX operations. The
same generated source exports each block's exact guest words and native entry,
so the runtime test decodes those words again and differentially executes the
host-compiled result. Seeds are fixed by default so failures do not disappear
between machines or reruns.


## AOT architectural-PC hardening

The first differential passes also hardened the generated C++ control-flow
boundary itself. External direct/indirect branches and returns now materialize
the taken target into `CpuState::nia` before returning an `ExecutionResult`, and
block fallthrough records final CIA from the guest block boundary even if the
optimizer removed the last guest-tagged IR node. This keeps debugger,
exception, adaptive-observation and verifier state consistent with the flow
result rather than merely returning the right host-side target.

## Gen 7 fallback cross-check

Gen 8 also checks the instruction-family overlap between the independent
reference model and the Gen 7 bounded fallback executor. The test runs scalar
integer/compare/load/store probes from executable Memory V2 pages, terminates
them through a real `blr`, and compares the complete architectural state plus
data-memory bytes against the reference path.

That cross-check immediately found a real Gen 7 state bug: a fallback `blr`
returned the correct `ExecutionResult::next_address` but left `CpuState::nia` at
`pc + 4`. Gen 8 fixes the return path so NIA is now the taken LR target before
returning to the caller. This is the intended purpose of the generation: find
semantic discrepancies that ordinary "did the game keep running?" tests can
miss.

## Reproduction and minimization

Every failed trial contains its exact derived seed.  `SemanticVerifier::replay`
can execute that trial directly without replaying the preceding RNG stream.

When enabled, the verifier performs bounded delta-style input minimization.  It
tries to remove irrelevant GPR/FPR/VMX/special-register values and then coarse
memory regions while retaining the mismatch.  This is intentionally capped so a
bad CI run cannot spend unbounded time reducing a failure.

A mismatch can emit `xenon.semantic-repro.v1` JSON containing:

- case and trial seed;
- guest instruction addresses and words;
- minimized GPR/FPR/special-register state;
- minimized guest memory image.

That artifact is a test/debug input, not learned runtime evidence and not part
of the Gen 7 adaptive database.

## Safety and determinism

- The reference executor never interprets an unsupported instruction.
- Randomness uses Xenon's own tiny deterministic generator, avoiding standard
  library implementation differences.
- Memory images are cloned byte-for-byte before each side executes.
- Exceptions are captured by class so a fault-vs-success disagreement is a
  semantic mismatch.
- Tests compare the complete `CpuState`, not only the nominal destination
  register, catching accidental clobbers.
- Repro generation occurs only on failure.

## Expansion path

The framework is intentionally broader than the initial corpus.  New
instruction families should be added by first independently transcribing their
reference semantics, then adding generated AOT cases.  High-value next coverage
is carry/overflow arithmetic, rotate/mask variants, reservation/conditional
stores, floating arithmetic plus FPSCR exception behavior, vector arithmetic
and VMX128 extended-register forms, and conditional LR/CTR control flow.

Gen 8 is therefore the semantic-validation foundation rather than a claim that
every Xenon opcode has already received an independent second implementation.
The important architectural change is that semantic correctness now has a
repeatable, fuzzable, replayable proof surface which future opcode work can
extend without changing the runtime.

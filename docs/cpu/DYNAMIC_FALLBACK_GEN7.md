# Gen 7 — Hybrid Execution Safety Net

Gen 7 adds a deliberately small dynamic PPC execution path for executable guest
code that the static/AOT pipeline did not discover in advance. It is a safety
net, not Xenon's normal execution model and not a replacement for static
recompilation.

The dispatch order is:

```text
known target
  -> compiled/AOT native entry

compiled lookup miss
  -> validate guest target is executable PPC memory
  -> bounded Gen 7 fallback executor
  -> record verified runtime observation

non-executable call target
  -> existing RuntimeServices/import boundary
```

## Design goals

* Keep AOT/native execution first and overwhelmingly dominant.
* Continue through legitimate executable edges that static analysis missed.
* Never reinterpret imports, MMIO, unmapped memory or NX data as PPC code.
* Reuse Memory V2 executable-page generations for self-modifying-code safety.
* Bound all fallback work so a corrupt target cannot become an unbounded
  interpreter loop.
* Produce reusable evidence for later analysis/knowledge generations.

## Runtime binding

`ExecutionContext` now has a separate `DynamicFallbackCallback` in addition to
its compiled-entry lookup. Generated code and `XenonSession` use the same order:
compiled lookup first, then dynamic fallback, then the existing runtime/import
boundary where a call target is not executable guest code.

Keeping the callback separate from `ExecutableCodeCache` is intentional. The
native translation cache still owns only native translations and its existing
page-stamp validation rules. Gen 7 can later be replaced by a micro-JIT without
changing generated-module ABI or the compiled registry.

## Executable target validation

The fallback claims a target only when all of these are true:

* the guest PC is 4-byte aligned;
* `MemoryPort::executable_page_stamp()` reports an executable mapping;
* instructions can be fetched with the normal execute-aware Memory V2 path.

An unmapped or NX target returns `handled=false`. That is important for Xbox
imports and other runtime-owned addresses: they continue to the pre-existing
`RuntimeServices` path instead of being misclassified as guest PPC.

## Self-modifying code and invalidation

No new write callback or global RAM lock was added. Gen 7 uses Memory V2's
sticky executable-page identity/generation stamps.

For every page touched during one fallback dispatch, the executor remembers the
first executable stamp and checks it again before each instruction. It also
checks after the instruction fetch. If a guest store, remap, protection change,
or another host thread changes the executable page generation, execution stops
with `SourceChanged` before more code from a mixed source generation is used.

This gives the fallback the same basic invalidation source of truth as native
`ExecutableCodeCache` entries.

## Bounded interpreter scope

The first implementation is an interpreter because it is the smallest path that
can establish correctness and collect runtime evidence. It intentionally has a
hard instruction budget and recursion-depth limit.

The executor currently covers the common scalar/control subset needed to cross
many missed indirect edges, including:

* integer immediate/register arithmetic used for address construction;
* logical, rotate, shift, compare and selected SPR/CR operations;
* common scalar D/DS/X-form loads and stores, update forms and byte-reversed
  accesses;
* direct and conditional branches;
* LR/CTR indirect branches and calls;
* compiled-entry handoff at any recovered target;
* system-call and architected trap runtime boundaries;
* common ordering/cache instructions used by generated code paths.

Unsupported valid/invalid instruction words on executable pages stop with a
structured trap. Exceeding nested-call depth also produces a handled structured
trap rather than falling through to the import/runtime call path. Gen 7 does not silently skip an opcode and does not pretend a
partially executed block succeeded.

Architected trap instructions (`tw`, `twi`, `td`, `tdi`) evaluate their TO
condition before invoking the runtime trap service.

The fallback is deliberately not expected to become a full game-running
interpreter. Gen 8 semantic verification and future runtime evidence should
reduce how often it is needed; a micro-JIT can later sit behind the same callback
if profiling shows it is worthwhile.

## Calls and control transfer

For a call encountered inside fallback execution:

1. look for an AOT/native target;
2. if the target is executable guest memory, recurse through the bounded
   fallback;
3. otherwise use `RuntimeServices::call` for the existing import/runtime path.

For non-linking branches, an available native target is handed off immediately.
A `bclr` returning to the LR value present at fallback-call entry produces the
same `FlowReason::Return` contract expected by generated callers.

## Runtime observations

Successful/handled fallback blocks emit `DynamicFallbackObservation` records. Nested dynamically executed callees are
recorded as their own entries rather than being hidden inside only the outer
trace. Records contain:

* entry and source site;
* exit address;
* call/branch lookup kind;
* stop reason;
* executed instruction count;
* an address-independent instruction-word fingerprint.

`XenonSession` appends these to the existing adaptive observation JSONL as
`kind: "executed-entry"`, with extra Gen 7 fields such as `fallback`,
`fingerprint`, `fallbackReason`, `transferKind`, and `instructions`.

The existing adaptive-analysis reader already accepts `executed-entry` and
ignores unknown fields, so Gen 7 evidence is backward compatible with the
current learning loop while preserving richer data for Gen 9.

Observations are deduplicated per `(entry, fingerprint)` and capped per session
to avoid an unbounded telemetry file.

## Safety invariants

Gen 7 must preserve these invariants:

1. AOT lookup always runs before fallback.
2. NX/unmapped targets are never interpreted.
3. Executable-page generation changes stop the active fallback dispatch.
4. Unsupported instructions never become implicit NOPs.
5. Instruction count and nested-call depth are bounded.
6. Calls still preserve the existing compiled/runtime return contract.
7. Fallback evidence is advisory input to later AOT analysis, not permission to
   bypass executable-memory validation.

## Validation

`tests/cpu/dynamic_fallback_tests.cpp` covers:

* an unknown executable call (`li; blr`) continuing successfully;
* NX/unmapped target rejection;
* unsupported executable instruction trapping;
* conditional PPC trap semantics;
* self-modifying code invalidating the active source page;
* the hard instruction budget on a self-loop.

Generated AOT indirect-call/branch paths are also compiled as part of the CPU
codegen validation so the new `ExecutionContext` callback remains usable from
emitted C++.

## Relationship to the next generation

Gen 7 supplies the reference execution path and runtime observations needed by
Gen 8. Gen 8 should use this path, together with independent PPC semantics, for
differential instruction/basic-block testing and translation validation rather
than expanding Gen 7 into an omniscient interpreter.

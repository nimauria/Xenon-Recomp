# Recomp Analysis Gen 6 — PPC Value & Indirect-Flow Analysis

Gen 6 extends Xenon's existing Recomp Analysis V3 discovery/provenance system. It does **not** replace the current CFG discovery, pointer-table scan, switch-table recovery, adaptive observations, or module hints. Its job is to make indirect PowerPC control flow substantially less fragile while keeping expensive analysis selective.

## Goals

Gen 6 targets five related problems:

1. preserve useful PPC register facts across ordinary instructions instead of clearing the entire analysis state;
2. resolve compiler-materialized indirect calls/branches cheaply when possible;
3. fold static function-pointer/vtable entries only when the source bytes are genuinely immutable in the loaded XEX image;
4. invoke bounded backward slicing only at unresolved indirect sites;
5. infer `MayReturn` / `NoReturn` behavior to a monotone fixed point through terminal tail-call chains.

The normal hierarchy is therefore:

```text
module/switch/known-site metadata (when supplied)
                |
                v
cheap block-local PPC abstract values
                |
          unresolved only
                v
bounded backward slice
                |
          unresolved only
                v
existing conservative jump-table recovery / unresolved diagnostic
```

## Abstract value lattice

`include/xenon/recomp/ppc_value_analysis.hpp` defines the bounded value domain used by the analysis:

- `Unknown`
- `Constant`
- `Address`
- `AddressPlusOffset`
- `FiniteSet`
- `Range`
- `StackRelative`
- `LoadedFromReadOnlyTable`

This is deliberately **not** unrestricted symbolic execution. The representation is kept small enough to run on every decoded instruction, while `FiniteSet` and `Range` provide room for later CFG joins and switch-index reasoning without changing the public analysis vocabulary again.

`Unknown` is the top element: merging an unknown incoming path with a precise value never manufactures certainty.

## Fast PPC propagation

`PpcValueTracker` currently understands the address/value operations that matter most for Xbox 360 indirect control flow, including:

- `addi` / `addis` (`lis`);
- `ori` / `oris` / `xori` / `xoris`;
- register OR/AND/XOR;
- integer add/subtract;
- `mtspr` / `mfspr` for CTR/LR;
- D/DS/X-form static loads;
- update-form effective addresses;
- stack-relative displacement tracking.

Compares, ordinary branches, floating-point instructions, and vector instructions no longer erase unrelated GPR facts.

### Call boundaries

Linked branches are ABI clobber boundaries. Xenon invalidates volatile GPR facts plus CTR after a call, while preserving the stack pointer and nonvolatile `r14-r31`. The backward slicer applies the same rule, so it cannot walk through a call and resurrect a stale volatile value.

This is intentionally conservative: losing a fact is acceptable; inventing an indirect target is not.

## Static pointer / vtable loads

A memory load can become `LoadedFromReadOnlyTable` only when all of the following hold:

- its effective address is statically known;
- the containing XEX section is readable;
- the containing section is **not writable**;
- the complete load lies within the materialized section bytes.

The loaded value is still accepted as a control-flow target only after normal executable-section and PPC alignment validation.

Writable tables are runtime state and are never folded from their on-disk bytes.

This covers common compiler/linker function-pointer tables and statically materialized vtable entries while avoiding false certainty for runtime-mutated dispatch structures.

## Tiered indirect resolver

`resolve_indirect_flow()` operates on `bcctrx` and `bclrx` sources.

1. **FastValue** — use the current block-local CTR/LR abstract value.
2. **ReadOnlyTable** — same fast path or slice, but the value originated in a proven non-writable table load; the source table address is preserved for diagnostics.
3. **BackwardSlice** — only when the fast value failed, reconstruct the last writer chain from a bounded instruction history.

The backward slice is capped by both instruction count and recursion depth. It follows only the small supported address/value/load family rather than trying to symbolically execute arbitrary PPC.

The resolver is used in:

- primary function scans;
- declared `FunctionChunk` scans;
- inferred local CFG ranges.

FunctionChunk abstract state is reset after terminal transfers so a contiguous metadata range cannot propagate facts through unreachable bytes.

## Return / no-return fixed point

`DiscoveredFunction` now records:

- `return_behavior` — `Unknown`, `MayReturn`, or `NoReturn`;
- whether that behavior came from explicit module metadata;
- whether an explicit normal `bclrx`/`blr` return was decoded.

An explicit return establishes `MayReturn` unless authoritative `FunctionFlags::NoReturn` metadata says otherwise. Conflicting metadata is kept authoritative but emits a warning.

After discovery and ownership reconciliation, Xenon runs a monotone fixed point over terminal, unconditional, non-linked external edges:

- a tail edge to a `MayReturn` function makes the caller `MayReturn`;
- if every resolved terminal external target is `NoReturn`, the caller becomes `NoReturn`;
- unresolved/unknown exits leave the function `Unknown`.

Ordinary linked calls are deliberately **not** enough to classify the caller as non-returning without path-sensitive proof.

## Diagnostics and cache invalidation

The analysis engine revision is bumped to **6**, invalidating prepared artifacts whose correctness depended on the older indirect-flow behavior.

New diagnostics include:

- `resolved_indirect_via_backward_slice`;
- `resolved_indirect_via_readonly_table`;
- `return_fixed_point_iterations`;
- `inferred_no_return_functions`;
- `inferred_may_return_functions`.

`resolved_indirect_via_dataflow` remains as the compatibility aggregate for generic PPC value-analysis resolutions.

Human-readable and JSON analysis reports expose the new return classification and counters.

## Safety properties

Gen 6 keeps the following hard rules:

- every candidate indirect target must be PPC-aligned and executable in the effective image;
- writable-memory loads never become static table facts;
- unknown instructions invalidate registers they may write rather than preserving guessed values;
- calls are explicit clobber barriers for volatile state;
- expensive slicing is bounded and only used after the fast path fails;
- unresolved sites remain visible diagnostics instead of being silently guessed.

## Tests

`tests/recomp/ppc_value_analysis_tests.cpp` covers:

- finite-set lattice joins;
- constants surviving unrelated comparisons;
- read-only function-pointer loads;
- rejection of writable-table folding;
- volatile register / CTR invalidation across calls;
- preservation of nonvolatile registers across calls;
- selective bounded backward slicing.

`tests/recomp/discovery_quality_tests.cpp` additionally covers:

- explicit normal-return classification;
- `NoReturn` propagation through terminal tail-call chains;
- all pre-existing Analysis V3 discovery-quality cases to guard against regressions.

## Deliberate limits / next work

Gen 6 does not try to make static analysis omniscient. Runtime-built dispatch tables, self-modifying code, dynamically generated PPC, and genuinely data-dependent indirect targets remain unresolved by design. Those cases belong to **Gen 7 — Hybrid Execution Safety Net**, where an unresolved executable edge can be handled at runtime, observed, validated, and fed back into a later AOT pass.

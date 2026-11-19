# Project Xenon whole-game recompilation pipeline

The supported entry point is:

```text
recomp-driver game.xex generated
```

The driver parses the XEX and PE section metadata, seeds discovery from the
entry point, exports, direct calls/branches, and module boundaries, then
compiles each conservatively discovered function through the CPU V2 decoder,
IR verifier, optimizer, and C++ AOT backend. The output is deterministic and
sharded:

```text
generated/
  functions/shard_000.cpp
  registry.cpp
  registry.hpp
  imports.cpp
  metadata.cpp
  hooks.cpp
  CMakeLists.txt
  .cache/
```

The function database retains guest ranges, names, discovery sources, calls,
callers, branch references, confidence, source hashes, and compilation status.
The driver reports invalid PPC, indirect control flow, targets outside
executable sections, and compilation failures rather than silently emitting
native code for them. A non-zero exit status indicates unresolved analysis
items.

`ModuleHint` is the extension point for Project Gracemeria and other modules.
Hints may add symbols and boundaries (and can be extended with data/ignored
regions and hooks); they never provide PPC semantics. Hints are included in
the configuration hash used for incremental identity. Generated function
sources are cached at the function level, keyed by BOTH the guest-word/
configuration content hash AND the function's own canonical guest start
address - never content hash alone. Content hash alone is not a safe cache
key: two different, unrelated functions can have byte-identical machine code
(a bare `blr`, a `li r3,0; blr` return-0 thunk, and other trivial stub bodies
recur constantly across a real title's tens of thousands of functions), and
keying purely on content previously let one such function's cached,
name-bearing output be silently reused for another - producing duplicate C++
symbol definitions (MSVC C2084) for one function while leaving the other's
own symbols never emitted. The `.cache/` directory reuses unchanged generated
function bodies (same address, same content) across incremental runs instead
of rerunning native source generation; it never reuses one function's output
for a different address.

## Instruction hooks

A module can patch one guest instruction without replacing its function. A
hint-set hook with `"kind": "instruction"` names a C++ routine (`"symbol"`)
that runs before the instruction at `"address"`, or after it with
`"afterInstruction": true`:

```json
{"address": 2182954124, "kind": "instruction", "symbol": "ac6StorageOrderHookOp2"}
```

The routine is `void symbol(xenon::cpu::ExecutionContext&)`. It reads and
writes guest registers through `context.state` and guest memory through
`context.memory`. A hook with `"onTrue"`/`"onFalse"` set to `"jump"` (with
`"jumpAddressOnTrue"`/`"jumpAddressOnFalse"`) or `"return"` returns `bool`, and
that result jumps to the target or returns to the guest link register.

The routine's source comes from `DriverOptions::hook_sources` and from the
hint provider (`ModuleHintProviderV2::hook_sources()`; an installed module
package keeps them in `revisions/<hash>/hooks/*.cpp`). The generated
`CMakeLists.txt` compiles them into `xenon_game`, and the function shard
declares and calls the routine.

The lifter emits a `HostHook` IR operation at the hook point. The optimizer
treats it as a full state barrier: guest registers held in host temporaries
are written to `CpuState` before the call and re-read after it. Jump targets
become block leaders and CFG edges. Hooks belong to the IR and generated-source
cache keys, so adding or moving one recompiles only the functions it touches.

Mistakes fail loudly rather than leaving a hook that never runs:
- a hook outside every compiled function fails analysis;
- an after-hook on a branch, which has no following point, fails the compile;
- a hint set with instruction hooks but no hook sources fails generation;
- a symbol that is not a C++ identifier, or one used with both signatures, fails schema validation;
- a routine the sources do not define fails the module link.

## Porting hints to another revision

A module's hints are scoped to one executable revision. `hint-port` carries
them to another revision of the same title, such as another region's disc
or a later build:

```text
hint-port <source-content> <target-content> source-hints.json out-hints.json [--map map.json] [--report report.txt]
```

Both contents can be a disc image, an extracted folder or a loose XEX.

The executables' code is aligned instruction by instruction, ignoring the
fields that change when code moves: branch displacements and the immediates
of address-forming and load/store instructions. Windows of eight
instructions that occur exactly once in each revision anchor the alignment.
Runs of anchors at one offset become segments, longest first, so moved code
still maps; segments then extend over equal neighbours.

Masked comparison can pair look-alike functions that a revision reordered,
such as two thunks that branch to different targets. Every aligned branch
is therefore checked: its destination must map to where the paired branch
goes. When it does not, the function on the side with the higher share of
conflicting branches loses its alignment. On a tie, the shorter function
loses. This repeats until the map is consistent.

A function entry outside the aligned code is placed through its references:
- every aligned call to it must land on the same target address; or
- its body must occur exactly once in the target with every outgoing branch
  landing where the alignment maps it. When several bodies fit, the one
  between the neighbouring aligned code wins.

Exact constants decide first. A body that differs only in a struct offset,
as after a class layout change, still matches on shape and branches.

Hints whose addresses do not translate are left out and listed: data
addresses (switch tables), code the revision changed, and hook sites in
rewritten code. `--map` writes the alignment as JSON so a module can carry
its own metadata files through it. Exit status 3 means some hints were left
behind.

Before code generation runs, the driver validates that every function
reaching codegen has a unique guest address and a unique generated symbol
family (`<name>`, `<name>_v2`, `<name>_dispatch_v2`). A collision fails
codegen immediately with a diagnostic naming both addresses and the first/
second owner, rather than emitting conflicting or silently-merged C++ - see
`AnalysisDiagnostics::codegen_duplicate_addresses_merged` and
`codegen_duplicate_symbols_rejected` in `analysis.json`.

Computed branches are reported as unresolved indirect control flow. The driver
also scans aligned words immediately following an indirect branch for pointers
into executable sections and records those as possible jump-table targets, with
warnings so module hints can confirm or reject them.

Diagnostic tools share the same analysis:

```text
ppc-disasm <game.xex>
ir-dump <game.xex>
import-scanner <game.xex>
module-inspector <game.xex>
```

All tools provide `--help`. `ir-dump` shows discovered CPU V2 blocks,
`import-scanner` classifies every imported symbol against the real
production export registry as IMPLEMENTED/SAFE_STUB/PARTIAL/MISSING (a
per-library and overall PASS/FAIL summary; `--json` for machine-readable
output - see `docs/kernel/KERNEL_V1_SUMMARY.md`), and
`module-inspector` prints XEX identity and section counts. `recomp-driver`
is the only tool that writes generated build input. The generated
`CMakeLists.txt` builds the shards, registry, imports, and metadata as a
normal `xenon_game` target; configure it with
`-DXENON_RECOMP_ROOT=<path-to-Xenon-Recomp>`.
`registry.cpp` also emits a CPU V2 lookup callback and
`bind_compiled_registry(ExecutionContext&)` for generated code integration.

Module hints can be supplied without changing the driver:

```text
name=ProjectGracemeria
function_boundaries=0x82001000,0x82002000
data_regions=0x82010000
ignored_regions=0x82020000
known_symbols=GameMain@0x82001000
special_hooks=Present
patches=fix-load-order
```

Pass the file with `recomp-driver game.xex generated --hints module.hints`.

## Performance and parallelism

`recomp-driver` (and the automatic game-preparation worker, `xenon-prepare`)
run analysis and code generation across multiple worker threads by default
(`--jobs auto`, or `--jobs 1` for deterministic single-thread execution). See
[RECOMP_ANALYSIS_V2.md](RECOMP_ANALYSIS_V2.md) for the phased pipeline design,
the discovery-wave model, and measured before/after characteristics.

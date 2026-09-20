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
sources are content-addressable at the function level through the guest-word
and configuration hash. The `.cache/` directory reuses unchanged generated
function bodies instead of rerunning native source generation.

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
`import-scanner` prints imported module symbols and thunks, and
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

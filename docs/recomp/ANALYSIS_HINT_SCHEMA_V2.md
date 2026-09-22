# Analysis Hint Schema V2 / ModuleHintProvider / AC6 Migration

Covers Parts 1-3 and 6 of the Project Gracemeria final platform readiness
pass. See `CLAUDE.md` for the standing implementation contract this work
follows.

## Why this exists

The pre-existing `xenon::recomp::ModuleHint` (`include/xenon/recomp/driver.hpp`)
is a flat bag of addresses (`function_boundaries`, `data_regions`,
`ignored_regions`, `known_symbols`, `special_hooks`, `patches`) - enough for
a simple/synthetic title, not enough for a complex commercial one like Ace
Combat 6, which needs function chunks, explicit switch-table targets, known
indirect call/branch sites, native CRT/heap replacements, setjmp/longjmp
runtime-helper identities, and revision-scoped data/ignored/invalid-code
regions. `ModuleHint` is unchanged and still supported; the new schema is
additive.

## Schema (`include/xenon/recomp/analysis_schema.hpp`)

`xenon::recomp::analysis::AnalysisHintSetV2` is a versioned
(`kAnalysisSchemaVersion = 2`), revision-scoped bundle of:

| Concept | Type | Notes |
|---|---|---|
| Function boundary/name/size | `FunctionHint` | address required; end/size/name/flags optional |
| Discontinuous chunk | `FunctionChunk` | `[start,end)` + `parent_function` |
| Manual jump table | `SwitchTableHint` | explicit targets, or `table_address`+`entry_count`+`entry_format` for the driver to decode |
| Known indirect call | `KnownIndirectCall` | callsite + targets (empty = acknowledged, not resolved) |
| Known indirect branch | `KnownIndirectBranch` | same idea, non-linking |
| CRT/heap replacement | `NativeReplacement` | guest address -> `NativeReplacementKind` (Xenon owns the implementation - see below) |
| setjmp/longjmp/register-spill helper | `RuntimeHelper` | generic; no address is ever hard-coded into Xenon core. Register-range kinds (`{Save,Restore}{GprLr,Fpr,Vmx,Vmx128}`) also carry `register_start` - see `docs/runtime/RUNTIME_HELPERS.md` |
| Literal instruction-word exclusion | `InstructionPatternHint` | `value`/`mask`/`skip_bytes`, optional `[scope_start,scope_end)` - content match, not an address range; see `docs/runtime/RUNTIME_HELPERS.md` |
| Data/ignored/invalid/code-override range | `RegionHint` | `[start,end)` |
| Symbol name | `SymbolHint` | |
| Byte patch | `PatchDeclaration` | applied by runtime/module loading, not analysis-time |
| Hook | `HookDeclaration` | free-form; Gracemeria-defined semantics |

`validate_hint_set()` checks: schema version, malformed ranges, duplicate/
self-referential/cyclic function parents, contradictory chunk/region overlap,
malformed switch-table shapes, at-most-one `RuntimeHelper` per kind (all 10
kinds, not just setjmp/longjmp) with `register_start` present/in-range for
the 8 register-range kinds and absent otherwise, no address collision across
the *expanded* register-range set, and `InstructionPatternHint` structural
validity (`skip_bytes` a positive multiple of 4, nonzero mask, well-formed
scope). See `docs/runtime/RUNTIME_HELPERS.md` for the register-helper/
instruction-pattern design in full.
`hint_set_matches_identity()` requires an **exact** match against
`xbox::XexEffectiveIdentity` (title_id, media_id, effective_image_hash) - a
hint set is never applied to the wrong executable revision (e.g. a base XEX's
hints must never apply to a title-update-patched revision, or vice versa).

JSON (de)serialization lives in `analysis_schema_json.hpp`/`.cpp`, built on
the existing dependency-free `xenon::core::JsonValue`.

## Recomp Driver consumption (`src/recomp/driver.cpp`)

`load_and_analyze()` actually **uses** the hint set, not merely stores it:

- Seeds function discovery from every `FunctionHint`/`FunctionChunk.start`/
  known indirect call & branch target/decoded switch target/`NativeReplacement`
  address.
- A `FunctionHint`'s own `end`/`size` bounds its scan; every other hint
  address (function/chunk/native-replacement) still acts as an implicit
  boundary for scans that reach it, exactly like the legacy
  `ModuleHint::function_boundaries`.
- `RegionHint(Data/Ignored/InvalidInstruction)` ranges are excluded from
  seeding and stop a function's scan the same way `ModuleHint`'s
  `data_regions`/`ignored_regions` already did.
- An indirect call/branch site (XL-format instruction) matching a
  `KnownIndirectCall`/`KnownIndirectBranch`/`SwitchTableHint` is resolved
  (targets seeded, treated like a direct branch) instead of falling into the
  generic nearby-word heuristic; an **empty** target list is reported as
  `indirect-call-acknowledged`/`indirect-branch-acknowledged`, distinct from
  a genuinely unresolved site nothing ever looked at. An ordinary `blr`
  (`bclrx`, unconditional, non-linked) is excluded from this whole path
  entirely - it is a function return CPU V2's own codegen already handles via
  `state.lr`, not an indirect branch needing target resolution.
- `SwitchTableHint.table_address`/`entry_count`/`entry_format` (when no
  `explicit_targets` are given) are decoded directly from the image's bytes
  (`AbsoluteWord32`/`RelativeWord32`/`RelativeInt16`).
- A `NativeReplacement` address is never decoded/compiled: `generate_project()`
  emits a `lookup_compiled()` case calling straight into
  `xenon::recomp::native_replacements::*` (see below) instead of a generated
  shard. A recognized-but-unimplemented kind (currently the four `Heap*`
  kinds - see "Known limitations") falls back to normal analysis and is
  reported in `AnalysisReport::warnings`, never silently substituted or
  dropped.
- setjmp/longjmp addresses are emitted as real generated symbols
  (`kHasSetJmpAddress`/`kSetJmpAddress`/... in `registry.hpp`/`metadata.cpp`)
  - carried through to the codegen output; see "Known limitations" for what
  this does *not* yet do.
- Register-range `RuntimeHelper` families (`{Save,Restore}{GprLr,Fpr,Vmx,
  Vmx128}`) are expanded (`analysis::expand_runtime_helpers()`) into every
  callable register-count variant address and dispatched natively, exactly
  like setjmp/longjmp - see `docs/runtime/RUNTIME_HELPERS.md`.
- `InstructionPatternHint` entries are checked against every candidate
  instruction word during the function/chunk scan loops, before decoding is
  attempted; a match ends the current scan cleanly (never a decode error,
  never seeded as a function) - see `docs/runtime/RUNTIME_HELPERS.md`.
- `AnalysisReport::diagnostics` (`AnalysisDiagnostics`) reports
  auto-discovered vs. hinted function counts, manual chunks, resolved switch
  tables, known indirect calls/branches, unresolved indirect sites, applied/
  unsupported native replacements, data/ignored region counts, register-save/
  restore helper counts, instruction-pattern rules loaded/matched, and
  analysis errors - surfaced in both `format_report()` and
  `format_report_json()`.

### Native replacements (`include/xenon/recomp/native_replacements.hpp`)

Real CPU-V2-ABI (`NativeCompiledEntry` - `ExecutionResult(ExecutionContext&)`,
`blr`-shaped return via `state.lr`) implementations for: `memcpy`, `memmove`,
`memset`, `memcpy_s`-shaped, `memmove_s`-shaped, `memcmp`, `strlen`,
`strncmp`, `strncpy`, `strchr`, `strstr`, `strrchr`, `strcpy_s`-shaped.

**Known limitation**: `HeapAllocate`/`HeapFree`/`HeapSize`/`HeapReAllocate`
(`RtlAllocateHeap`/`RtlFreeHeap`/`RtlSizeHeap`/`RtlReAllocateHeap`-shaped) are
recognized schema/name identities but have **no implementation** in this
pass: a correct one needs a real guest-heap allocator, and `MemoryPort` (the
CPU-facing interface these functions run against) has no allocation
primitive to build one on. `native_replacements::entry_for()` returns
`nullptr` for these; the driver falls back to normal analysis and reports it.
Building a guest heap is future work, not done here.

**Update (generated-code deduplication / shard ownership fix)**: this
limitation is resolved. `FunctionChunk` bytes ARE stitched into their
declared parent's single compiled IR/function (`compile_ranges()` receives
the parent's primary range plus every owned chunk's words as one compiler
input) - a chunk is not an independently-dispatchable entry, and
`AnalysisReport::functions` never contains a separate record for a chunk's
own `start` address. See `tests/recomp/analysis_v2_consumption_tests.cpp`'s
"FunctionChunk is integrated into parent IR and direct native control flow"
test. The one exception: a chunk address that ALSO has its own independent,
parentless `FunctionHint` is excluded from its declared parent's merged body
and compiles as that independent function instead - the two are mutually
exclusive so the same guest bytes are never compiled twice under two
canonical identities (see `has_independent_function_hint()` in
`src/recomp/driver.cpp` and that test file's "A FunctionChunk excluded by a
conflicting independent FunctionHint..." case). `DiscoveredFunction::
chunk_parent` remains unset by this pass (nothing currently populates it);
chunk association is recoverable from the parent's own `ranges` instead.

**Known limitation**: setjmp/longjmp metadata reaches generated code as real
symbols (proving the analysis/codegen plumbing works end to end - see
`tests/recomp/analysis_schema_tests.cpp`), but nothing in CPU V2 yet
*traps*/interprets a call to those addresses as save/restore-context
semantics. That interpretation is a CPU V2 change, out of scope here.

## ModuleHintProvider production integration (Part 2)

`ModuleHintProviderV2` (`driver.hpp`) is the interface the Recomp Driver
actually calls (`DriverOptions::hint_provider_v2`). `FileModuleHintProvider`
(`module_hint_provider.hpp`/`.cpp`) is the real, production implementation,
reading a module package from disk:

```
<module_root>/
  manifest.json                                  {"moduleName": "...", "revisions": [...]}
  revisions/<effective_image_hash_hex>/analysis.json   - one AnalysisHintSetV2 (JSON)
```

`<effective_image_hash_hex>` is `xbox::format_effective_image_hash()`'s
lowercase hex form - selecting the right revision is an **exact** directory-
name match against the executable actually being analyzed, never a
"closest-looking" fallback. A module with no data for the running revision,
a missing manifest, or an analysis.json whose own embedded identity
disagrees with its revision directory are all clear, reported failures.

### CLI wiring (Part 2.3/2.4)

```
recomp-driver default.xex generated --module /path/to/module-package
```

`tools/recomp_tools.cpp` constructs a real `FileModuleHintProvider` from
`--module` and passes it as `DriverOptions::hint_provider_v2` - this is the
production construction site the contract requires (a provider class that
nothing ever constructs would be incomplete).

## AC6 config migration tool lives in Project Gracemeria, not here

Xenon-Recomp deliberately has **no** AC6-specific parser, namespace,
address, configuration format, or migration code, and never reads ReXGlue
TOML anywhere in its runtime or Recomp Driver. AC6/ReXGlue knowledge is
title-specific and belongs to the game module, per the standing
architecture rule (`CLAUDE.md`): "Game modules do not implement... Xenon
owns Xbox 360 semantics... Game modules supply only game-specific
information."

The `ac6recomp_config.toml` -> `AnalysisHintSetV2` migration tool is
implemented in the separate **Project Gracemeria** repository
(`gracemeria::migration`, depending on this schema's public headers only -
the dependency direction is Gracemeria -> Xenon, never the reverse). See
that repository's own docs for the tool and its category-mapping table.
Xenon's role ends at exposing a stable, generic schema
(`AnalysisHintSetV2`/`ModuleHintProviderV2`) for any game module - AC6 or
otherwise - to target.

## Import scanner / analysis report tooling (Part 6)

`import-scanner` groups its output by library (`xboxkrnl`, `xam`, an
`xboxkrnl (audio)` sub-group by resolved-name prefix since Audio V1 exports
are registered under the literal `"xboxkrnl"` string rather than a distinct
import module, and any other declared module), printing for each import its
resolved Xenon symbol and implementation status (`Required`/`Stubbed`/
`Optional`/`DiagnosticOnly`/`NOT_REGISTERED`) against the real, production
`core::ExportRegistry` - never a second, hand-maintained list.

`recomp-driver`'s analysis report (`format_report`/`format_report_json`) now
includes the full `AnalysisDiagnostics` block described above, giving a
commercial-title-scale view of what static analysis actually resolved versus
what still needs attention.

## Tests

- `tests/recomp/analysis_schema_tests.cpp` - schema validation, revision
  scoping, JSON round trip (pure unit tests, no XEX/driver pipeline),
  including `expand_runtime_helpers()`'s address formula and
  `InstructionPatternHint`'s structural validation.
- `tests/recomp/analysis_v2_consumption_tests.cpp` - proves the Recomp
  Driver actually *uses* chunks/switch tables/known indirect calls/native
  replacements/regions/instruction-pattern hints during real analysis
  (synthetic XEX fixtures).
- `tests/recomp/module_hint_provider_tests.cpp` - a synthetic two-revision
  module package on disk; correct/incorrect revision selection; proof the
  driver actually consumes hints resolved through `hint_provider_v2`.
- `tests/cpu/x86_64/runtime_helpers_register_range.cpp` - proves the
  register-range `RuntimeHelperKind` native implementations carry out the
  real guest ABI save/restore chain end to end (see `docs/runtime/RUNTIME_HELPERS.md`).

AC6-specific migration tests live in Project Gracemeria's own repository,
against this schema's public API.

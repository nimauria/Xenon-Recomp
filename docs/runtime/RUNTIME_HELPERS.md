# Runtime helpers: register-spill/restore families and instruction-pattern hints

Covers two generic Analysis Hint Schema V2 / Recomp Driver capabilities added
to close the two gaps Project Gaia's Sonic Unleashed (UnleashedRecomp/SWA.toml)
bring-up surfaced against Xenon core - see `GENERIC_XENON_BLOCKERS.md` for the
original evidence. Both gaps were genuinely generic (any XenonRecomp-toolchain
title can hit them), not Sonic-specific, and neither is implemented with any
title address or title-specific logic anywhere in this repository.

## 1. Register-range `RuntimeHelperKind` families

### What these are

Xbox 360 titles built with the XenonRecomp-toolchain convention (also seen
independently in generic PowerPC/EABI "out-of-line prologue/epilogue"
compiler support) do not inline a nonvolatile-register spill/restore
sequence into every function. Instead, the compiler emits **one shared
"fall-through ladder"** per register class, with a distinct entry point for
every register count a function might need:

```
_savegprlr_14:  std r14, OFFSET(r1)
_savegprlr_15:  std r15, OFFSET+8(r1)     <- entered directly when only r15..r31 need saving
_savegprlr_16:  std r16, OFFSET+16(r1)
...
_savegprlr_31:  std r31, OFFSET+136(r1)
                std r0,  LR_OFFSET(r1)    <- caller's own mflr'd LR
                blr
```

A function needing `r20..r31` saved simply does `bl _savegprlr_20` (i.e. the
address 6 instructions past `_savegprlr_14`) rather than emitting its own
6-instruction sequence. hedge-dev/XenonRecomp's own `RecompilerConfig`
(research reference, reimplemented independently - never copied) declares
exactly 8 such family-base addresses, distinct from `setjmp`/`longjmp`:

```cpp
uint32_t restGpr14Address, saveGpr14Address;     // GPR + LR, registers 14..31
uint32_t restFpr14Address, saveFpr14Address;     // FPR,      registers 14..31
uint32_t restVmx14Address, saveVmx14Address;     // VMX,      registers 14..31
uint32_t restVmx64Address, saveVmx64Address;     // VMX128,   registers 64..127 (extended)
```

Its own `Analyse()` derives every other callable variant address from the
single declared base via `base + (register - family_start) * stride`, where
`stride` is 4 bytes (one register+immediate-displacement instruction) for
GPR/FPR, or 8 bytes (an index-register setup instruction plus one indexed
vector store/load - AltiVec/VMX has no immediate-displacement addressing
mode) for VMX/VMX128.

### Schema representation

`xenon::recomp::analysis::RuntimeHelperKind` (`analysis_schema.hpp`) gained 8
values: `{Save,Restore}{GprLr,Fpr,Vmx,Vmx128}`, alongside the pre-existing
`SetJmp`/`LongJmp`. `RuntimeHelper` gained an optional `register_start` field,
used only by these 8 kinds - a module declares **one** entry per family (the
lowest-numbered variant, matching XenonRecomp's own single-address config
field), and `analysis::expand_runtime_helpers()` derives every other callable
variant address using the exact same formula:

```cpp
std::vector<RuntimeHelper> expand_runtime_helpers(const std::vector<RuntimeHelper>& helpers);
```

`validate_hint_set()` enforces: at most one `RuntimeHelper` per kind (matching
XenonRecomp's config, which has exactly one address field per family);
`register_start` present and in range (`[14,32)` for GPR/FPR/VMX, `[64,128)`
for VMX128) for the 8 register-range kinds, absent for `SetJmp`/`LongJmp`; and
no address collision across the *expanded* set (two families could otherwise
silently compute overlapping concrete addresses).

### Execution

These helpers dispatch through the **same existing mechanism** `SetJmp`/
`LongJmp` already used - not a new one. Recomp Driver's discovery loop
(`load_and_analyze()`) skips decoding/compiling the real guest bytes at any
expanded runtime-helper address (`expanded_runtime_helpers`, computed once
via `expand_runtime_helpers()`); `generate_project()` emits one
`lookup_compiled()` `case` per expanded address, dispatching straight to a
native C++ implementation.

This was a deliberate architectural choice, not the only possible one: the
real bytes at these addresses genuinely are ordinary, compilable PPC
instructions, so "just recompile them normally" was considered. It was
rejected because these entry points *physically overlap* (the `register_start
= 20` entry point's bytes are a byte-for-byte suffix of the `register_start =
14` entry point's bytes) - Xenon's function-discovery/chunk/artifact-cache
model assumes non-overlapping function ownership, and teaching it to support
a "shared overlapping ladder" pattern generically would be far more invasive
than reusing the dispatcher mechanism that already exists and is already
proven correct for `SetJmp`/`LongJmp`.

Native implementations (`include/xenon/recomp/runtime_helpers.hpp`, header-only
templates parameterized on `RegisterStart` so the driver's generated code can
instantiate exactly the variants a title actually uses, e.g.
`&runtime_helpers::save_gpr_lr_v2<20>`):

| Kind | Base register | Offset formula | Confirmed against real disassembly? |
|---|---|---|---|
| `SaveGprLr`/`RestoreGprLr` | `r1` | `r1 - 0x98 + (reg-14)*8`; LR at `r1-0x08` | **Yes** - community reverse-engineering of real Xbox 360 binaries (`std r14,-0x98(r1)`, ascending 8 bytes/register) |
| `SaveVmx`/`RestoreVmx` | `r12` (not `r1` - see below) | `r12 - 0x120 + (reg-14)*16` | **Yes** (`li r11,-0x120` + `lvx v14,r11,r12`) |
| `SaveVmx128`/`RestoreVmx128` | `r12` | `r12 - 0x400 + (reg-64)*16` | **Yes** (`li r11,-0x400` + `lvx128 v64,r11,r12`) |
| `SaveFpr`/`RestoreFpr` | `r1` | `r1 - 0x420 + (reg-14)*8` | **No** - a self-consistent placeholder in its own reserved band, clear of the other three confirmed ranges so combined use never aliases; not independently confirmed against a real disassembly this pass. **Verify against a real title's disassembly before relying on this for actual FPR-spilling guest code.** |

VMX/VMX128 use `r12`, not `r1`, because AltiVec/VMX128 load/store
instructions have no immediate-displacement addressing mode - the real
compiled helper synthesizes the offset into an index register (`r11`) and
reads a base out of `r12` (a frame-pointer alias the *calling* guest code, not
Xenon, is responsible for setting up correctly - Xenon only reads it, exactly
as the real `lvx vN, r11, r12` instruction would).

`SaveGprLr` is entered via `bl` (returns into the caller's remaining
prologue, using its live `state.lr`); `RestoreGprLr` is entered via a tail
`b` substituting for the calling function's own epilogue, so it reloads LR
**from the stack slot the paired save wrote**, not whatever is currently live
in the LR register (which nested calls may have clobbered since), and
returns through that. `SaveFpr`/`SaveVmx`/`SaveVmx128` and their restores
never touch LR at all - only the GPR family bundles it, matching the real
`*gprlr*` (not `*gpr*`) naming.

### Tests

`tests/cpu/x86_64/runtime_helpers_register_range.cpp` proves the full guest
ABI chain end to end (direct calls against a real `ExecutionContext`/guest
`AddressSpace`, mirroring `setjmp_longjmp.cpp`'s style - no compiled guest
code is needed since these dispatch natively): initialize guest registers,
run the save helper, verify guest stack memory holds the exact expected
big-endian values at the confirmed offsets, clobber the registers, run the
restore helper, verify the original values (and, for the GPR family, the
original LR) come back - for the lowest, a middle, and the highest supported
register-start variant of every family, plus a combined-use case proving
GPR/FPR/VMX slots for the same frame never alias each other.
`tests/recomp/analysis_schema_tests.cpp` covers `expand_runtime_helpers()`'s
address-derivation formula and `validate_hint_set()`'s new checks.

## 2. `InstructionPatternHint`: literal instruction-word content matching

### What this is

hedge-dev/XenonRecomp's `RecompilerConfig::invalidInstructions` is
`std::unordered_map<uint32_t, uint32_t>` keyed by a **literal 32-bit
big-endian instruction word**, not an address - its one consumption site
(`recompiler.cpp`) reads:

```cpp
auto invalidInstr = config.invalidInstructions.find(ByteSwap(*(uint32_t*)data));
if (invalidInstr != config.invalidInstructions.end()) {
  base += invalidInstr->second;  // skip `second` bytes and keep scanning
  data += invalidInstr->second;
  continue;
}
```

i.e. "wherever this exact 4-byte pattern occurs, treat it and the next N
bytes as non-code" - a content-match rule evaluated at every scan position,
not a declaration about one address. Real `SWA.toml` entries confirm this:
`data = 0x00000000 # Padding` and `data = 0x00485645 # End of .text` are not
plausible guest addresses at all.

### Schema representation

`analysis::InstructionPatternHint` (`analysis_schema.hpp`): `value`, `mask`
(default all-ones - exact match), `skip_bytes` (default 4), optional
`[scope_start, scope_end)` to restrict matching to one address range (when
absent, matching applies globally, matching XenonRecomp's own semantics).
`validate_hint_set()` requires `skip_bytes` to be a positive multiple of 4, a
nonzero mask, and `scope_end > scope_start` when both are given.
`AnalysisHintSetV2::instruction_patterns` is a new, empty-by-default vector -
existing hint sets and JSON files remain valid without any migration.

### Analyzer consumption

Recomp Driver's two per-function/per-chunk linear scan loops
(`load_and_analyze()`) check every candidate instruction word against the
loaded `instruction_patterns` **before** attempting to decode it (mirroring
XenonRecomp's own "check first" ordering). A match (masked equality, and
within scope if the rule declares one) ends the current function/chunk's
scan cleanly at that address - not as a decode error - and the matched bytes
are never seeded as a function start. This is real analysis-time consumption
(Part 2.4/2.5), not metadata that only round-trips through JSON:
`AnalysisDiagnostics` gained `instruction_patterns_loaded`/
`instruction_pattern_matches`, surfaced in both `format_report()` (as
"instruction pattern rules: loaded: N matches: N") and
`format_report_json()`.

This was deliberately **not** modeled as a runtime illegal-instruction trap -
the real semantic is "this is embedded data/a sentinel, not code", evaluated
during static analysis, never a guest-visible execution-time fault.

### Tests

`tests/recomp/analysis_v2_consumption_tests.cpp` builds a synthetic XEX
containing normal code, an in-scope occurrence of the pattern (proven to end
a function's scan cleanly, with no fake function created at the matched
address, while surrounding valid code remains fully analyzable), and an
out-of-scope occurrence of the identical literal word (proven to still fail
to decode normally, showing scope is actually enforced rather than applied
globally) - plus a separate test proving mask semantics (`word & mask ==
value & mask`, not exact equality). `tests/recomp/analysis_schema_tests.cpp`
covers `validate_hint_set()`'s new structural checks and the JSON round trip,
including a legacy fixture with neither `registerStart` nor
`instructionPatterns` present, to prove backward compatibility.

## Project Gaia migration

`gaia::migration::migrate_swa_config()` (Project Gaia, not Xenon) now maps
all 8 register-helper addresses to a real `RuntimeHelper` (kind +
`register_start`) apiece, and all 5 real `invalid_instructions` entries to a
real `InstructionPatternHint` apiece - both categories previously reported as
unsupported/downgraded are now `0` in the migration report. See Project
Gaia's own `GENERIC_XENON_BLOCKERS.md` entry and `docs/UNLEASHED_RECOMP_AUDIT.md`
for the before/after counts against the real public `SWA.toml`.

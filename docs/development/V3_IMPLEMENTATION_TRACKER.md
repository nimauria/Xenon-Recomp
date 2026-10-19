# Xenon V3 implementation tracker

This is the single maintained status record for the Xenon V3 programme
(Xbox 360 native recompilation, platform fidelity and dual-format game
delivery; specification amended 14 October 2026, see "Plan amendment"
below). It records what the tree actually does, measured on a named
revision, and what each phase must deliver. Executable tests and the
configured CMake graph remain the source of truth; where this file disagrees
with them, this file is wrong.

**Naming.** "V3" here means this programme. `docs/recomp/RECOMP_ANALYSIS_V3.md`
is an earlier, unrelated analysis pass; `XEX_LOADER_V2`, `KERNEL_V1`, `GPU_V1`
and similar names are subsystem document generations, not programme phases.

**States.** `NOT STARTED`, `IN PROGRESS`, `BLOCKED`, `DONE`. A phase is `DONE`
only when its acceptance criteria pass with recorded evidence.

**Inventory classes.** `implemented+tested` (behaviour tests pass),
`implemented+unverified` (code traced, no qualifying behaviour test),
`partial` (real behaviour with a known gap), `stub` (placeholder),
`missing`, `unknown` (not assessed). Synthetic tests passing does not mean a
retail title works.

## Phase status

| ID | Phase | State | Notes |
| --- | --- | --- | --- |
| V3-00 | Audit and baseline | DONE | This document; baseline below |
| V3-01 | CI and build correctness | DONE | Linux and Windows green on `6c23b0b` (runs 38124710875, 38124710898, 38124710980). Residual: `ilammy/msvc-dev-cmd` still runs on Node 20; no Node 24 release exists upstream |
| V3-02 | Diagnostics contracts | DONE | Acceptance met locally; the Windows probe-log fix (`641b17e`) passed on `6c23b0b` |
| V3-03 | Guest scheduler and waits | DONE | Acceptance met on both platforms at `6c23b0b`; deferrals listed under V3-03 below. AC6 not run |
| V3-04 | Kernel execution and interrupts | DONE | Linux, Windows, Xenon CI and coverage refresh green on `ccdadae`; evidence-gated items deferred with reasons (V3-04 progress). AC6 not run |
| V3-05 | Guest services and ABI | IN PROGRESS | Registry ordinal check; ABI defects fixed in voice, enumerators, `XamUserGetXUID`, `XeCryptSha`, `ExCreateThread`, `XCONTENT_DATA`; profile settings, content packages, notifications, headless dialogs and XeCrypt hashes implemented (see V3-05 progress) |
| V3-06 | Discovery database | NOT STARTED | |
| V3-07 | Deep analysis and decompiler views | NOT STARTED | |
| V3-08 | Adaptive knowledge | NOT STARTED | |
| V3-09 | AOT compilation and strict mode | NOT STARTED | Strict-AOT blockers recorded under "Plan amendment" |
| V3-10 | CPU semantics and verifier | NOT STARTED | |
| V3-11 | Memory fidelity and efficiency | NOT STARTED | |
| V3-12 | Xenos command frontend | NOT STARTED | |
| V3-13 | Vulkan/D3D12 parity | NOT STARTED | |
| V3-14 | Audio and timing | NOT STARTED | |
| V3-15 | Replay and compatibility lab | NOT STARTED | |
| V3-16 | Standalone SDK and dual hosts | NOT STARTED | One game core under the launcher module or a standalone bootstrap |
| V3-17 | CLI and dual-format packaging | NOT STARTED | `launcher` / `standalone` / `both` from one preparation |
| V3-18 | Cross-title qualification | NOT STARTED | |

## Plan amendment (14 October 2026)

The owner amended the specification. Changes against the version this
tracker was started from:

- **Strict AOT.** Release and gameplay builds must not interpret PowerPC at
  runtime; a compiled-lookup miss must fail with a structured diagnostic.
  Gen 7 interpretation stays, as an explicit developer and reference option.
- **Two products from one preparation graph:** a launcher-loadable module
  and a standalone native executable/package, selected as `launcher`,
  `standalone` or `both`, sharing discovery, IR, generated code and
  compatible objects. Optional deterministic source export. A standalone
  package need not be a single file.
- **Phase changes.** V3-02 gains structured `aot_missing_target`,
  `dynamic_code_detected`, `unsupported_guest_instruction` and
  `unresolved_indirect_flow` events; V3-09 becomes "AOT compilation and
  strict mode"; V3-12 adds statically preparable shaders and resources;
  V3-16 becomes dual hosts; V3-17 becomes CLI and dual-format packaging;
  V3-18 qualifies both paths. The acceptance framework adds strict-AOT
  and packaging-parity checks.
- **V3-00 additions:** audit Qt independence of source/module export, the
  launcher module ABI, standalone-host state, Gen 7 callsites and
  strict-AOT blockers, and record the decisions. Done below; nothing was
  implemented.

### V3-00 audit delta: strict AOT and dual output

Measured on `fbf0678`.

| Question | Finding |
| --- | --- |
| Where does Gen 7 run? | `DynamicFallbackExecutor` is created in `XenonSession::init_cpu()` when `SessionConfig::enable_dynamic_fallback` is set, which is the **default** (`true`), and bound to each `ExecutionContext` in `guest_dispatch.cpp`. Generated C++ calls `context.try_dynamic_fallback()` after every compiled-lookup miss on an indirect call or branch (emitted by `backend_cpp_aot.cpp` and `instruction_emission.cpp`), and thread-entry dispatch does the same (`guest_dispatch.cpp`). So every build today interprets a missed target silently. |
| What happens with the fallback off? | A call miss goes to `XenonSession::call()`: the `ExecutableCodeCache` (native translations only; the session installs no runtime compiler), then import thunks, then a trap; a branch miss returns `FlowReason::Branch` to the dispatcher; a thread entry miss crashes the thread with a message. No structured event names the missing target. |
| Other runtime code generation? | `ExecutableCodeCache` accepts an optional runtime compile callback; none is installed. Strict mode must forbid installing one as well as Gen 7. |
| Launcher module ABI | The session loads a native module with `dlopen`/`LoadLibrary` and calls `Xenon_BindCompiledRegistry(ExecutionContext&)` (`native_extension.cpp`): a same-toolchain C++ interface, no version negotiation (weakness 7). |
| Standalone host | `xenon_runtime_host` links only `xenon_core` (no Qt; `ldd` shows none) but is generic: it is driven by the launcher's `launch-config.json`, `status.json` and `stop.signal` contract and loads a separately built module. There is no per-game executable or bootstrap and no package manifest. |
| Preparation | `xenon-prepare` is Qt-free and builds the CMake target `xenon_game_module` from generated C++ in its cache root. Flags: `--content`, `--cache-root`, `--module` (a hint package directory), `--module-id`, `--title-update`, `--config`, `--status-file`, `--stop-signal`, `--observations`, `--knowledge`, `--recomp-root`, `--force`, `--query`. No `--package`, platform `--target`, `--output` or `--emit-source`. |
| Naming conflicts with the proposed CLI | The proposal takes the image positionally and an `--output` directory; today they are `--content` and `--cache-root`. `--module` already means hints, not the package mode. V3-17's compatibility audit must keep the existing flags working. |
| Strict-AOT blockers | Indirect targets discovery does not resolve (each currently reaches Gen 7); runtime-generated or self-modified code (`ExecutableCodeCache` invalidation exists, but nothing compiles a replacement); the default-on fallback; no structured miss event; thread entries reached only through function pointers. AOT coverage (confirmed functions, resolved indirect edges) is not measured anywhere yet. |

Decisions recorded (not implemented):

- Strict AOT will be a session and package policy: release/gameplay
  packages run with the Gen 7 executor absent and no runtime compile
  callback, and a miss produces an `aot_missing_target` event and an
  explicit stop. Development builds may select Gen 7 explicitly and every
  interpreted block stays visible in the capability report. Owned by
  V3-09, with the event kinds added to the V3-02 event model then.
- Strict AOT reuses the existing learning loop (owner suggestion, 14
  October 2026). Compiled-lookup misses are already recorded as adaptive
  observations (`record_compiled_lookup_miss`, capped at 65,536 facts per
  session) and `xenon-prepare --observations` feeds them into the next
  preparation, filtered by image hash and part of the cache key, so a
  target interpreted once is compiled on the next Play. V3-09 keeps this:
  development runs interpret and record; strict runs stop on a miss and
  still record it, so the following preparation compiles it. Limits: only
  executed code is learned; a learning run is only as correct as Gen 7;
  observed targets stay candidates that discovery must validate as code;
  runtime-generated or self-modified code cannot be learned this way.
- Launcher and standalone packages share one preparation graph (Gen 10
  store); only link and package stages differ. The standalone bootstrap
  and the launcher adapter are thin hosts over the same `xenon_core`
  session. Owned by V3-16 and V3-17; the existing
  `Xenon_BindCompiledRegistry` path keeps working through an adapter.

## V3-00 baseline (10 October 2026)

### Repository state

At the start of the audit: local HEAD `e796fe5`, ten commits ahead of
`origin/development-restructure` (`a01d2e8`), plus uncommitted work: the
`xenon-prepare` feedback-loader refactor, findings 3 to 6 in
`RESTRUCTURE_FINDINGS.md`, and nine multi-object wait reproducers in
`tests/kernel/wait_semantics_tests.cpp`.

During the audit the owner committed and pushed that work as `03b26e5`, so
`origin` now carries all eleven commits. Every workflow on `03b26e5` failed
(runs 38045421347, 38045421352, 38045421442, 38045421382) for three
independent reasons, none of them in runtime code:

1. `tools/xenon_prepare/feedback.cpp` defined its functions outside
   `namespace xenon::prepare_tool`, so `xenon-prepare` did not compile on
   either platform.
2. The README rewrite (`5b1ddb4`) links the coverage badge to
   `docs/coverage/README.md`; `tools/coverage/test_generate.py` still required
   `dashboard.svg`, failing Xenon CI and the coverage refresh.
3. `wait_semantics_tests.cpp` was committed without CMake registration, which
   the source-ownership audit rejects on both platforms. Registered, it fails
   on the real `wait.cpp` defects.

Fixed locally (not pushed, by owner decision): `6e1b603` (namespace),
`eb97e6b` (coverage test), and the V3-03 wait fix that registers the
reproducers (see V3-03 below).

### Host and toolchain

Ubuntu 24.04.5 Codespace, 2 vCPU (Xeon 8370C), 7 GiB RAM, about 4 GiB free
disk. GCC 13.3, Clang 18.1.3, CMake 3.28.3, Ninja 1.11.1, Python 3.14.2.
Vulkan through Mesa llvmpipe (API 1.4.318). No audio output device. No
Windows, D3D12, physical GPU or retail title input. Build preset
`linux-x64-debug` (Debug, tests and benchmarks on, Qt launcher on, DXC on).

### CI before the audit (pushed head `a01d2e8`)

| Workflow | Run | Result |
| --- | --- | --- |
| Xenon Linux (debug, 149 tests) | 37979112354 | pass |
| Xenon CI: `linux-sanitizers` (ASan+UBSan subset) | 37979112077 | pass (2 m 53 s) |
| Xenon CI: `platform-results` | 37979112077 | fail: needs same-commit Windows and Linux success |
| Xenon Windows MSVC (148 tests, launcher off) | 37979112157 | fail: 147/148 |
| Xenon coverage refresh | 37979112266 | pass |

The only Windows failure is `xenon_backend_capability_tests`. On the
Microsoft Basic Render Driver the D3D12 presentation step reports DirectX
debug-layer notification `0x87D` once after the first present and once after
the resized present, and the test fails on any debug-layer notification. The
functional D3D12 checks before it pass. The cause has not been diagnosed. All
workflows also warn that `actions/checkout@v4` runs on deprecated Node 20.

### Local build and tests

Baseline, working tree at `e796fe5` plus the then-uncommitted work, Debug,
`ctest --parallel 2`: **148/148 passed** in 17 min 52 s. The 149th test,
`xenon_prepare_worker_tests`, could not build (reason 1 above) and was
excluded. Slowest tests, each dominated by a nested build of a generated
project against a snapshot of the whole runtime:

| Test | Seconds |
| --- | ---: |
| `xenon_registry_numeric_format_tests` | 533 |
| `xenon_guest_export_abi_tests` | 449 |
| `xenon_audio_guest_callback_tests` | 449 |
| `xenon_recomp_driver_tests` | 266 |
| `xenon_memory_tests` | 185 |
| `xenon_memory_hardening_tests` | 54 |

Everything else finished in under 30 s. Warnings in a full Debug build are the
known pre-existing set (unused parameters in `runtime_services.cpp` and
`mount_content()`, ignored `terminate()` results in `thread.cpp`).

### Source and test inventory

Counts from the working tree (`.cpp`/`.hpp`, lines including comments):

| Area | Files | Lines | Test files | Test lines |
| --- | ---: | ---: | ---: | ---: |
| `src/cpu` | 33 | 7,909 | 42 | 5,094 |
| `src/memory` | 19 | 5,033 | 12 | 4,370 |
| `src/kernel` | 26 | 4,206 | 9 (+1 unregistered) | 2,420 |
| `src/xbox` | 39 | 11,797 | 24 | 9,347 |
| `src/xam` | 30 | 4,592 | 5 | 1,150 |
| `src/core` | 37 | 6,309 | 15 | 5,220 |
| `src/graphics` | 45 | 17,064 | 16 | 6,422 |
| `src/audio` | 9 | 2,696 | 4 | 688 |
| `src/input` | 18 | 4,315 | 9 | 1,919 |
| `src/filesystem` | 15 | 3,832 | 1 | 1,175 |
| `src/network` | 9 | 1,675 | 1 | 581 |
| `src/recomp` | 50 | 9,118 | 20 | 7,571 |
| `include/xenon` | 218 | 24,520 | | |
| `runtime_host/src` | 11 | 1,716 | 4 | 738 |
| `launcher` (Qt, optional) | 196 | 37,252 | own tests | |

Build accountability (`tools/development/build_accountability.py check`):
365 of 380 production sources compile in this configuration; the other 15
are platform or dependency exclusions (D3D12 ×11, Windows sockets, SDL3,
XInput, Windows and fallback host VM). 165 standalone test programs, 149
CTest entries.

Live export registry (`import-scanner --registry`, the production
`ExportRegistry`, not a source grep), without audio because this host has
no audio device:

| Library | Functions | Partial | Stubbed | Variables |
| --- | ---: | ---: | ---: | ---: |
| xboxkrnl | 202 | 14 | 2 | 10 |
| xam | 66 | 18 | 10 | 0 |

The coverage dashboard lists 254 kernel operations. 209 of them match the
registry by name. The other 45 are XAudio/XMA exports, which are registered
only when the session's audio subsystem starts, so they are absent from a
dump on a host without audio. The dashboard omits `DbgPrint`, `KeBugCheck` and
`KeBugCheckEx`, which the registry has. Eleven file-I/O exports are registered under library name
`xboxkrnl` and the rest under `xboxkrnl.exe`; lookup normalizes the suffix,
so this is inconsistent labelling, not a resolution failure.

Coverage dashboard (reviewed lower bounds, assessed `bc02c44`): kernel
20/254 verified, PPC 1/455, shader 1/105, PM4 7/50. 819 of 864 listed
operations are unassessed, which means unknown, not missing.

## Subsystem inventory

| Subsystem | Class | Evidence and gaps |
| --- | --- | --- |
| XEX parse, decrypt, decompress, title updates | implemented+tested | `xenon_xex_*` tests on synthetic images; retail coverage beyond AC6 unknown |
| PPC decode, lift and AOT lowering | implemented+tested for catalogue coverage; semantics mostly implemented+unverified | 455/455 catalogue patterns decode, lift and lower (`CPU_V2_DESIGN.md`). Independent Gen 8 reference covers integer immediate/register/record forms, compares, extensions, scalar loads and stores, `fmr`/`fneg`/`fabs`, `vand`/`vor`/`vxor` and simple branches. FP/FPSCR, carry and overflow edge cases, rotates, reservations and most of VMX/VMX128 have no independent reference |
| CPU IR, optimizer and state promotion | implemented+tested | `xenon_cpu_v2_*` tests |
| Gen 7 dynamic fallback | implemented+tested (bounded) | Unsupported instructions trap rather than NOP; the interpreter is deliberately limited |
| Gen 8 semantic verification | partial | Fail-closed but narrow, see above; no differential checkpoint system by design |
| Memory V2 | implemented+tested | 21/21 brief phases; 81 MB metadata for 512 MiB guest RAM. Open: findings 5 (commit handshake depends on x86 store/load ordering) and 6 (`InterlockedPopEntrySList` link read) in `RESTRUCTURE_FINDINGS.md` |
| Kernel objects (events, semaphores, mutants, timers) | implemented+tested individually | Single-object waits are covered by `xenon_kernel_synchronization_tests`; mutant abandonment on owner exit and guest `SignalState` consistency since V3-03 |
| Kernel multi-object waits | implemented+tested (since the V3-03 fix) | At the baseline, `wait.cpp` WaitAll consumed objects one at a time (a timeout left earlier objects consumed), a zero-timeout WaitAll always timed out, and WaitAny polled every 1 ms. Fixed; see V3-03 |
| APC, DPC, interrupts | missing (APC/DPC), partial (interrupts) | Only `KiApcNormalRoutineNop` is registered; no `KeInitializeApc`, `KeInsertQueueApc`, `KeInitializeDpc` or `KeInsertQueueDpc`. GPU interrupts are delivered synchronously on the GPU pump thread |
| Guest SEH and exceptions | partial | `RtlRaiseException`, `RtlUnwind`, `RtlCaptureContext`, `__C_specific_handler` declare partial; `setjmp`/`longjmp` has dedicated tests |
| xboxkrnl services | mixed | 202 functions, 14 partial, 2 stubbed; 20 verified by reviewed behaviour tests |
| XAM services | mixed | 66 functions, 18 partial, 10 stubbed; offline-first, no Xbox Live |
| Filesystem, content, STFS/GDFX | implemented+tested on fixtures | `XenonSession::mount_content()` returns success and does nothing (finding 2) |
| Xenos PM4 frontend | partial | 7/50 PM4 entries verified. `ContextUpdate`, `EventWriteCacheFlush`, `EventWriteZPassDone`, `MeInit`, `SetState`, `WaitForIdle`, `WaitIndirectBufferPfdComplete` and `WaitUntilRead` are emitted as IR with no execution path; `MemWriteCounter` is raw passthrough. Unknown packets are preserved, not guessed |
| Shader frontend and translation | partial | 1/105 verified; `MarkVsFetchDone` and `Nop` have no lowering (likely benign, unassessed) |
| Vulkan backend | implemented+tested on llvmpipe | Shares `BackendCore` with D3D12; four behavioural divergences listed in `RESTRUCTURE_FINDINGS.md` |
| D3D12 backend | implemented+tested on the Microsoft Basic Render Driver, presentation test failing | Windows CI debug-layer `0x87D`; neither backend compiles without DXC (finding 1); stencil-reference capability check (finding 4) |
| Audio and XMA | implemented+tested on synthetic fixtures | Guest exports exist only when a host audio device starts; eight-credit callback model (`kMaxQueuedRenderFrames`) |
| Input | implemented+tested | SDL2, keyboard/mouse, XAM facade tests; SDL3 and XInput not built here |
| Network | partial | Host sockets Windows-only; several XNet exports partial |
| Recomp Gen 6 value and indirect flow | implemented+tested | Function-pointer tables and vtables deferred |
| Gen 9 knowledge base | implemented+tested | JSONL records with fingerprints, a confidence score and a `curated` flag; no lifecycle states or revocation |
| Gen 10 compilation graph | implemented+tested | Caches IR and generated source per region in `graph::Store`; **discovery always re-runs** |
| Gen 11 game intake | implemented+tested | DLC and multi-title-update fan-out out of scope |
| `xenon-prepare` | implemented+tested at HEAD; broken in working tree | Builds the full runtime once per module (finding 3) |
| Runtime host | implemented+tested | Qt-free console process driven by `launch-config.json`; SDL2 window |
| Native module ABI | partial | `Xenon_BindCompiledRegistry(ExecutionContext&)` is a same-toolchain C++ contract; no versioned C ABI; no `install(EXPORT)` or CMake package. `include/xenon/modules/game_module_api.hpp` is used only by an unbuilt example header |
| Diagnostics | partial | `capability_report()` verdict (`PASS`/`PASS_WITH_FALLBACK`/`FAIL`) has no forward-progress signal, so a stalled run can report `PASS`. Probe logs are unconditional and unbounded |
| Launcher separation | holds | No Qt include or link outside `launcher/` (CMake references are packaging only) |

## Architectural weaknesses found

Each item has a source reference and an owning phase. "Confirmed" means
observed in source or by execution during V3-00.

1. **Multi-object waits were non-atomic and polled** (confirmed by source
   reading and by the owner's reproducers). Fixed in V3-03.
2. **Guest-visible audio depends on a host audio device** (confirmed).
   `init_audio()` failure fails session initialization; headless mode
   disables audio, so XAudio/XMA exports disappear and a title's imports
   become missing. Guest semantics should run against a null host sink.
   V3-14, V3-16.
3. **Pump callbacks are synchronous and unbounded** (confirmed in
   `src/core/session/pumps/gpu_pump.cpp`; no watchdog exists in
   `src/core` or `src/audio`). The vsync and
   graphics interrupts run on the GPU pump thread and the audio render
   callback on the audio thread, with no watchdog. A blocked guest callback
   stops ring draining and presentation (`AC6_RUNTIME_INVESTIGATION.md`).
   V3-04, V3-14.
4. **Diagnostics are unbounded files in the working directory**
   (confirmed). 36 raw `fopen` probe sites in 16 source files, plus
   `append_probe_log()` probes, all unconditional. Test runs left a 35 MB
   `ke_wait_diag.log` in the repository root and 9 MB in the build
   directory. Title-specific AC6 probes live in
   `src/core/session/diagnostics/title_probes.cpp` with guest addresses.
   V3-02.
5. **Discovery is not cached.** Gen 10 keys IR and source nodes, but
   `load_and_analyze()` re-runs whole-module discovery every time.
   `RECOMP_ANALYSIS_V3.md` Part 13 deferred this. V3-06 should add a
   discovery stage to the existing `graph::Store`, not a second store.
6. **Generated projects rebuild the runtime per module** (finding 3).
   `xenon_prepare_worker_tests` uses 540 to 783 s of a 900 s Windows budget.
   V3-09, V3-17.
7. **The native module boundary is a C++ reference ABI** with no version
   negotiation or CMake package export. V3-16.
8. **False-success paths remain.** `mount_content()` returns success and does
   nothing; 12 exports are registered as stubbed. V3-05.
9. **Memory ordering assumes x86-64** for the reservation commit handshake
   (finding 5). Correct on the only supported host; not portable to ARM64.
   V3-11.
10. **Kernel objects read guest `SignalState` once** (confirmed in source;
    fixed in V3-03, `b907c97`).
    `resolve_dispatcher_object()` (`src/xbox/xex_dispatcher_header.cpp`)
    reads `SignalState` only when it creates and stashes the host object;
    later guest writes to that field are not observed. Whether titles write
    it directly is unverified (the AC6 handshake-event probe asks exactly
    that). V3-03.
11. **The capability verdict cannot see a stall.** It reports `FAIL` only
    for a recorded session error or an unbound native module. V3-02, V3-15.
12. **Benchmarks are never executed by CI**, so they rot unnoticed
    (confirmed: the recomp benchmark failed its own assertion at every size).
    V3-09 should run a short benchmark smoke as a CTest.

## Documentation that disagrees with the tree

| Document | Claim | Reality |
| --- | --- | --- |
| `docs/memory/MEMORY_V2.md` | Complete 21/21; "no remaining planned Memory V2 phase" | The restructure later found and fixed a reservation-monitor lock-order deadlock; findings 5 and 6 remain open |
| `docs/cpu/CPU_V2_DESIGN.md` | "Production Xbox 360 recompilation core ready" | Catalogue coverage is complete; independently verified semantics are a small subset (dashboard 1/455) |
| `docs/kernel/KERNEL_V1.md` | Done criteria include waiting on objects | Multi-object waits were defective at the baseline (weakness 1, now fixed); APCs and DPCs are still absent |
| `docs/xam/XAM_V1.md` | `XamShowDeviceSelectorUI` and `XamContentCreateEnumerator` stubbed; Phase 7 AC6 validation TODO | Both registered as required, not partial |
| `docs/coverage/` inventory | 254 kernel operations | Live registry differs as described above; counts need reconciling in V3-05 |
| `docs/runtime/AC6_CAPABILITY_VERDICT.md` | "No remaining gaps" for the verdict | Verdict has no progress criterion (weakness 11) |
| `docs/recomp/RECOMP_ANALYSIS_V3.md` | Name | Not this programme |

Historical V1/V2 documents remain useful design records. Phases that touch
them should correct the claims above rather than add new status files.

## Reconciling the V3 plan with the tree

- **V3-01 is narrow.** On Windows, one test fails for one reason. Locally,
  the working tree does not compile `xenon-prepare`. Also in scope: the
  Node 20 deprecation, and checking that `registry_numeric_format` and
  `prepare_worker` have Windows timing margin now that nested builds use
  the outer generator.
- **V3-02 and V3-03 ordering.** The plan makes V3-03 depend on V3-02. The
  core wait fix was pulled forward (see V3-03 progress) because it blocked
  CI and needed no new diagnostics. The rest of V3-03 still benefits from
  V3-02's bounded wait tracing, so V3-02 comes next, starting with the two
  largest probes (`ke_wait`, `set_event_all`).
- **V3-06 builds on Gen 10**, adding a `discovery` node keyed on the
  effective image digest, hint-set digest, analyzer producer hash, decoder
  semantics and knowledge fingerprint.
- **V3-08 extends Gen 9.** The record states the plan asks for
  (`observed` to `revoked`) do not exist yet; Gen 9 has curated and
  confidence-scored records.
- **V3-10 extends Gen 8's reference executor**, not a new verifier.
- **V3-14 includes weakness 2**: separate guest audio semantics from host
  output with a null sink.
- **V3-16 starts from an existing Qt-free runtime host.** The gaps are the
  ABI, packaging and an orphaned module API, not launcher coupling.
- **AC6 reference corpus.** The ~10,527 reference function starts come from
  Project Gracemeria (`RECOMP_ANALYSIS_V3.md`). Their revision must be
  reconfirmed before V3-07 compares against them.

## Benchmark corpus

What exists:

- `xenon_recomp_analysis_benchmark [functions] [words]`: synthetic XEX of
  hint-seeded NOP/`blr` functions; reports analysis and codegen wall time
  for one worker and for all workers. Always cold: it deletes its cache root.
  It does not exercise real CFG shapes.
- `xenon_memory_v2_benchmarks`: the Memory V2 Release benchmark (CSV/JSON).
- CTest durations from `ctest-results-*` JUnit artifacts in CI.

What V3 needs and does not yet have:

| Measure | Status |
| --- | --- |
| Cold analysis | Synthetic only; needs a corpus with real CFG shapes (calls, switches, indirect flow, shared tails) |
| Warm analysis | Missing; blocked on V3-06 |
| C++ codegen | Synthetic, in the benchmark above |
| Native build, cold and warm prepared module | Missing; `xenon_prepare_worker_tests` duration is the only proxy |
| Peak RSS | Missing from harnesses; measured externally below |
| False-positive functions, unresolved flow | Reported in `analysis.json`; needs a synthetic corpus with ground truth (V3-06/V3-07) |
| Runtime progression | Missing; needs V3-02 events and V3-15 reports |
| Lawful AC6 run | Not available in this environment |

Baseline measurement after fixing the benchmark fixture (below), Debug build,
`xenon_recomp_analysis_benchmark 3000 20`, three process runs each timing
`jobs=1` and `jobs=auto` (one worker on this host), six samples:

| Measure | Median | Range |
| --- | ---: | ---: |
| Analysis | 5.39 s | 5.11 to 5.67 s |
| C++ codegen | 3.16 s | 3.08 to 3.37 s |
| Peak RSS (whole process) | 29 MiB | 29 MiB |

Debug numbers set a reference point for this host only; Release
measurements belong to V3-06 and V3-09.

**The benchmark was broken.** Its synthetic XEX put `.text` at raw file
offset `0x600` while declaring RVA `0x10000`. The loader copies sections by
RVA (`src/xbox/xex/image/pe_directories.cpp`), which is right for XEX
basefiles, so the first 64,000 bytes of hinted functions read as zero words
and were rejected. Every size failed the benchmark's own assertion (3,000
requested, 2,200 compiled). The fixture now places `.text` at its RVA.
Because the benchmark is not a CTest, nothing caught this, and the Recomp
Analysis V2 figures in `RECOMP_ANALYSIS_V2.md` could not be re-measured until
this fix.

## V3-01 progress

CI on `0e59c08` (runs 38050800775, 38050800823): Linux passed every test,
including `xenon_kernel_wait_semantics_tests` and `xenon_prepare_worker_tests`.
Windows passed 147 of 149, including the wait tests under MSVC. Two failures:

1. `xenon_registry_numeric_format_tests` timed out at 900 s;
   `xenon_prepare_worker_tests` passed at 882 s. Both build a generated project
   against the whole runtime (finding 3). Generated modules linked
   `Xenon::Recomp`, so every nested build compiled the recompiler (44 of 172
   compiles) although generated code needs only the runtime helpers and
   native replacements. Those are now `xenon_recomp_runtime` (`caa56ca`). A
   nested build compiles 130 sources. Locally, the full suite went from 27 min
   37 s to 22 min 22 s and passed 150/150 for the first time; per test,
   `guest_export_abi` 449 to 205 s, `audio_guest_callback` 449 to 262 s,
   `recomp_driver` 266 to 174 s, and `prepare_worker` passes at 875 s instead
   of timing out. No timeout was changed. Windows effect not yet measured.
2. `xenon_backend_capability_tests` (D3D12 presentation): two debug-layer
   exceptions with code `0x87D`. Per the ReactOS mirror of `winerror.h`,
   2173 (`0x87D`) is `FACILITY_DIRECT3D11_DEBUG` (`0x87A` is
   `FACILITY_DXGI`); not yet checked against the Windows SDK header. The
   exception parameters hold no readable text. A reported case of the same
   exception during `ResizeBuffers` came with an
   `OBJECT_DELETED_WHILE_STILL_IN_USE` corruption message, but Xenon's resize
   already waits for the queue to go idle first, so that cause is not
   assumed. The presentation backend is created without the debug layer
   requested, yet runs under it, because an earlier step in the same process
   enables it process-wide. The test now prints the debug layer's stored
   messages after each presentation step (`927d6c9`); the next Windows run
   names the actual message.

Run 38073115216 (`d5cd6e8`): Windows passed 148 of 149. The nested-build
change brought `registry_numeric_format` to 840 s (from a 900 s timeout),
`prepare_worker` to 597 s (from 882 s), `guest_export_abi` to 129 s and
`audio_guest_callback` to 147 s. Linux finished in 18 min 53 s (from 23 min
15 s).

The stored messages named the D3D12 defect: `CORRUPTION #921`, an
`ID3D12Resource` final-released while referenced by GPU work in flight on
the queue, after resize, and at teardown a command queue final-released
while in use. `CommandQueue::wait_idle()` waited only for the fence value of
Xenon's own last submission, but `IDXGISwapChain::Present()` queues work on
the same queue after it, so resize and teardown released the back buffers
and the queue under an in-flight present. `wait_idle()` now signals a new
value and waits for it (`d62bdcd`). The Vulkan backend already follows its
wait with `vkQueueWaitIdle()`. The run also showed ten `ClearRenderTargetView`
/ `ClearDepthStencilView` warnings (#820, #821: clear values differ from the
optimized clear value given at resource creation); these are performance
warnings, raise no notification, and are left for V3-13.

Also on this branch: CI actions moved to their first Node 24 majors
(`f41960d`; `ilammy/msvc-dev-cmd` has no Node 24 release yet), the known
Debug warnings cleared (`ba4b94c`), and `mount_content()` now fails instead
of reporting success (`e471e19`).

Run 38075338656 (`6ee8852`): `xenon_backend_capability_tests` passed on
Windows; the D3D12 fix holds. One other test failed there for the first
time: `xenon_thread_creation_tests` asserts that a looping guest thread
stops within 500 ms of `terminate()`, and it took longer (it stopped within
the 2 s join bound). It passed on the two previous Windows runs and passes
30 of 30 locally, idle and with the CPU saturated. On that build the thread's
exit path still opened probe files unconditionally, which can be slow on a
loaded Windows runner; that is a hypothesis, not a cause. The 500 ms bound is
unchanged; the next Windows runs decide.

## V3-02 progress

Run 38082406059 (`99298e1`, Windows): 151/152; `xenon_probe_log_tests`
failed. `append_probe_log()` opened files in text mode, so MSVC's CRT wrote
a carriage return before every newline and the file no longer held the
bytes the 4 MiB cap counted. Files are now opened in binary mode
(`641b17e`). Linux cannot show the difference; the next Windows run
confirms it. The same run passed `xenon_thread_creation_tests` (43 s) and
`registry_numeric_format` at 823 s, `prepare_worker` at 381 s.

Status as of `01563c1`. Off by default; see "Diagnostics" in
`docs/runtime/AC6_RUNTIME_INVESTIGATION.md` for the switches.

- **Bounded event ring** (`676117d`): fixed-size, lock-free, atomic records
  with a per-slot sequence check; 300 plain and 20 ThreadSanitizer runs
  clean; publishing a slot before its payload fails the tests.
- **Wait, signal and lock events**: every Ke*/Nt* wait and signal
  (`69d1846`, `1b84dc9`), spin locks and critical sections (`433e0d2`), with
  the kernel object id so Ke* and Nt* use of one object line up.
- **Stall report** (`6dbb155`): `blocked_waits()` names each thread still
  waiting, the object, how long, and the last signaller, flagging a signal
  that came after the wait began. A staged stall through the real export
  registry is reported correctly.
- **Probe logs** (`a336752`): every `*_diag.log` probe goes through one
  function that is off unless enabled, replaces each file per run and caps
  it at 4 MiB. The launch configuration can enable events and probes
  (`d36972c`).
- **Title addresses out of generic code**: the guest store path compared
  every 32-bit write against two AC6 addresses; it is now an opt-in
  `XENON_WRITE_TRAP` (`86c9da9`). The spin-lock path named an AC6 lock
  (`433e0d2`). Only `title_probes.cpp`, the isolated AC6 probe file, still
  names title addresses.
- **Audio credits** (`01563c1`): per-client counts of callbacks, frames and
  rejections by reason, and a one-line diagnosis in the stop report. This
  answers the AC6 note's first question (does the client exhaust its eight
  credits, and why) on the next real run.
- **Defect found on the way**: generated game modules linked Xenon's static
  libraries without `-fPIC`; that worked only while no linked object
  referenced preemptible global data. The write-trap table did, and every
  module then failed to link on Linux. Generated projects now build Xenon
  as position-independent code (`e0ee079`).

- **GPU fence parks** (`e6c173e`): `GpuWaitParked` once per fence and
  `GpuWaitResumed` when it is satisfied; the stop report prints an
  unresolved park beside the open guest waits.
- **Guest caller and categories** (`1e11832`): events carry the caller's
  LR, so a report names the waiting guest code; open waits with no timeout
  are tagged "deadlock candidate" from the shared failure-category list.

Acceptance (plan, V3-02): deterministic tests show correlation (one object
id across Ke*/Nt* waits and signals, the waiter's caller) and bounded storage
(the ring keeps the newest 65,536 events; probe files stop at 4 MiB per
run); a staged stall through the export registry is reported as the right
thread, object, caller and last signaller; per-test suite timings did not
regress, and `record()` costs about 6 ns off and 40 ns on (GCC 13 -O2).

Deferred, with reasons:

- Guest-time correlation: events carry host monotonic time only. The guest
  time base is per thread and not reachable from every recording site;
  V3-04 (thread identity and scheduling) is the natural owner.
- Run fingerprints in exported event files: events are not written to disk
  yet; the stop report prints them. A JSONL export belongs with V3-15's
  compatibility reports.
- GPU ring pointer progress and interrupt delivery are counters already
  (`gpu_interrupts_delivered_`, PM4 statistics); per-event recording would
  flood the ring at 60 Hz and is not added.
- The remaining failure categories are defined but assigned by later phases
  (V3-05 exports, V3-13 backends, V3-15 reports).

## V3-03 progress

Pulled ahead of V3-02 on 10 October 2026 because the unregistered
reproducers blocked CI and the owner chose to fix the defect rather than
register a failing test or remove it.

Change (`src/kernel/synchronization/`):

- `wait.cpp`: a multi-object wait locks each distinct object's own mutex in
  address order and then checks and consumes in one critical section. WaitAll
  takes every object or none; WaitAny takes the lowest-indexed satisfiable
  object and reports its index. A zero timeout checks once.
- Waiters sleep on one kernel-wide wait generation instead of polling.
  Event set, semaphore release, mutant release and abandon, and timer set and
  fire advance it whenever a multi-object wait is registered; thread
  termination always does. Its mutex is a leaf lock (`wait_internal.hpp`).
- Event, semaphore, mutant and timer expose `can_satisfy_locked()` and
  `satisfy_locked()` to `detail::WaitAccess` only, and their own single-object
  waits use the same rules, so the two paths cannot disagree.
- `tests/kernel/wait_semantics_tests.cpp` is registered as
  `xenon_kernel_wait_semantics_tests`, with four more tests: overlapping
  WaitAll sets in opposite orders, recursive mutant ownership inside WaitAll,
  waking on thread termination, and a wake-latency bound for every signalling
  operation.

Verification:

| Check | Result |
| --- | --- |
| Owner reproducers before the fix (built out of tree) | Abort on the first: a timed-out WaitAll reset an auto-reset event |
| `xenon_kernel_wait_semantics_tests` (13 tests) | Pass; 300 repetitions of the original nine and 100 of all 13 with no failure |
| ThreadSanitizer (GCC 13, test plus kernel synchronization and thread sources) | 20 runs, no reports |
| Mutation: remove the notification from thread termination, event, semaphore, mutant or timer in turn | Each makes the suite abort |
| Source-ownership audit | 0 errors (was 1) |
| Full CTest, Debug, `--parallel 2 --timeout 900` | 149/150; `xenon_prepare_worker_tests` timed out at 900 s while compiling a nested generated project |
| `xenon_prepare_worker_tests` alone | Pass in 1,047 s on this 2-vCPU host, over the 900 s CI budget. Not attributable to the wait change (it is compile-bound; finding 3) but not yet compared against a baseline run |
| Sync-related test durations before and after | No regression (for example `xenon_xboxkrnl_ke_sync_export_tests` 2.30 s to 2.17 s, `xenon_thread_creation_tests` 8.38 s to 7.14 s) |
| Windows MSVC build and tests | Not run; needs CI |

Second batch, 11 October 2026 (local, not pushed):

- **Mutant abandonment** (`8941755`). Nothing called
  `KernelMutant::abandon()`, so a thread that exited owning a mutant left it
  owned forever and every later waiter hung; `WaitResult::Abandoned` was
  never produced. A mutant now records the host thread that acquired it
  (guest thread ids are per `ThreadManager` and repeat across sessions in one
  process). When a `KernelThread`'s host thread finishes, after guest code
  has stopped and before the thread is signalled, it abandons every live
  mutant it still owns. Doing it in `terminate()` instead would free the
  mutant while the thread may still be inside the critical section. The
  next wait takes ownership and returns `STATUS_ABANDONED_WAIT_0` plus the
  satisfying index for a WaitAny, or index 0 for a WaitAll.
- **Guest `SignalState` kept consistent both ways** (`b907c97`). Events and
  semaphores resolved from a guest header store every state change into
  `SignalState` under their own mutex, through the process's address space.
  Each later `Ke*` resolution adopts a value the guest wrote directly and
  records `GuestStateAdopted` (or `GuestStateRejected` for a semaphore count
  outside 0..limit). As on hardware, a direct write wakes no thread already
  blocked. This closes weakness 10 and answers AC6 hypothesis 4 from the
  event ring on the next run.
- **`ObReferenceObjectByHandle` for events and semaphores** (`b94565a`)
  returned the sentinel `0xDEADF00D`, so a title passing the pointer to
  `KeSetEvent` or a `Ke*` wait failed to resolve it. The first reference now
  places a `DISPATCHER_HEADER` (and a semaphore's limit) in the kernel pool,
  stashed to resolve to the handle's own object; later references return the
  same address. Mutants and timers keep the sentinel.
- **Dispatched timers under multi-object waits** (`585036f`): tests for
  WaitAny waking at the due time, a synchronization timer releasing one of
  two waiters, and WaitAll completing when the timer fires.
- **Per-object multi-wait lists** (`d5a0f28`). Every signal used to wake
  every multi-object wait in progress. Each object now keeps a list of the
  multi-object waits on it, as the NT dispatcher does, and a change wakes
  only that list.

- **Multi-object wait type was inverted.** Both exports took 0 as WaitAny
  and nonzero as WaitAll. NT's `WAIT_TYPE` is `{ WaitAll, WaitAny, ... }`
  (0 = WaitAll), and xenia selects WaitAny for any nonzero value, so every
  guest multi-object wait ran as the other type: a WaitAny on a work event
  and a quit event became a WaitAll that needs both. Fixed; other values are
  now rejected, and `NtWaitForMultipleObjectsEx` rejects a WaitAll naming one
  object twice with `STATUS_INVALID_PARAMETER_MIX`. This is a candidate cause
  of AC6's permanent waits; unconfirmed until AC6 is run.

Semantics provenance (research policy, section 5):

| Question | Source | Certainty | Implemented |
| --- | --- | --- | --- |
| Owner exit abandons a mutant; next waiter gets ownership and `STATUS_ABANDONED` | Win32 `WaitForSingleObject`/`WaitForMultipleObjects` documentation; NT dispatcher (WRK `KiWaitSatisfyMutant`, recalled, not re-read) | High for NT; Xbox 360 by NT derivation, unverified on hardware | Yes |
| WaitAny abandoned status is `0x80 + index`; WaitAll is `0x80` | WRK `KeWaitForMultipleObjects` (recalled) | Medium | Yes |
| `SignalState` is the object's state; a direct write takes effect at the next wait or signal and wakes no blocked waiter | NT `DISPATCHER_HEADER` semantics: waits test `SignalState`, waking is done by `KiWaitTest` on signal paths | High for NT | Yes |
| `ObReferenceObjectByHandle` returns the object body | NT object manager contract | High | Events and semaphores |
| `WAIT_TYPE`: WaitAll = 0, WaitAny = 1 | NT headers (ntdoc `_WAIT_TYPE`); xenia `XObject::WaitMultiple` (nonzero = WaitAny) | High | Yes |
| Nt WaitAll with a duplicate object fails `STATUS_INVALID_PARAMETER_MIX` | ReactOS `ntoskrnl/ob/obwait.c`; Win32 `WaitForMultipleObjects` documentation | High for NT; Xbox unverified | Nt only; Ke still consumes a duplicate once |

Verification (Debug unless noted, this host):

| Check | Result |
| --- | --- |
| `xenon_kernel_mutant_abandonment_tests` (9 tests, new) | Pass; 200 repetitions with no failure |
| `xenon_kernel_wait_semantics_tests` (14 tests) | Pass; 50 repetitions |
| `xenon_kernel_timer_dispatch_tests` (8 tests) | Pass; 50 repetitions |
| `xenon_xboxkrnl_ke_sync_export_tests`, `xenon_xboxkrnl_sync_export_tests`, `xenon_xbox_ob_exports_tests` | Pass; Ke tests 50 repetitions |
| ThreadSanitizer, GCC 13, the three kernel suites | 20 runs each, no reports |
| Mutation: remove the exit-time abandonment; the abandoned flag; the abandoned index in the status; the `SignalState` store; the reconcile; the Ob header; each object's multi-wait wake (event, semaphore, mutant release and abandon, timer set and fire, thread exit and `terminate()`) | Each fails a test (removing the store hangs the cross-thread test, which CTest's timeout would report) |
| Source-ownership audit | 0 errors |
| Full CTest at `d5a0f28`, Debug, `--parallel 2 --timeout 900` | 153/154 in 16 min 44 s; `xenon_prepare_worker_tests` timed out at 900 s while running beside `registry_numeric_format` (317 s), then passed alone in 720 s. Compile-bound (finding 3), as on earlier runs on this host; timeout unchanged |
| Wake cost, `-O2`, 20,000 set/reset pairs on an event nobody waits on, three runs | Before `d5a0f28`: 0.7 us per signal with 4 idle WaitAny waiters, 27 to 42 us with 16, about 176 us (273 us CPU) with 64. After: about 0.04 us with 0 to 64 |

Still open in V3-03:

- `KeWaitForMultipleObjects` with a duplicate object in a WaitAll consumes
  it once; NT leaves that case undefined for kernel callers.
- Alertable waits and APC delivery during waits need APCs, which do not
  exist yet (V3-04).
- A thread that runs guest code on a host thread not started by
  `KernelThread::thread_main()` does not abandon its mutants on exit; every
  guest thread created through `ExCreateThread` does.
- `terminate()` signals a waiter on the thread before the terminated
  thread has stopped running guest code (unchanged; mutants are abandoned
  later, at the real exit).
- `TimerManager` runs timer callbacks while holding its own mutex, so a
  callback that re-schedules a timer would deadlock. Production passes no
  callback today; recorded, not changed.
- AC6 has not been run; whether any of these changes moves its stall is
  unknown.

CI on `6c23b0b` (all workflows green): Windows 153/153, including
`xenon_probe_log_tests` (0.6 s), the three kernel wait suites and the Ke,
Nt and Ob export tests under MSVC; `prepare_worker` 414 s and
`registry_numeric_format` 749 s within the 900 s budget.

## V3-04 progress

Started 11 October 2026. The plan asks for APC/DPC semantics only with
evidence that titles need them; the repository holds no AC6 import list or
trace naming an APC or DPC export, so none was added yet. The evidence that
does exist (weakness 3, AC6 hypothesis 2) is that pump callbacks run
synchronously, so the first step makes a blocked callback visible rather
than changing interrupt ordering.

- **Pump callback watch** (`d6708af`). Each pump keeps an always-on
  `CallbackWatch`: the guest callback it is inside and since when, the
  number of callbacks, how many took longer than a 60 Hz frame, and the
  longest. The stop report prints both pumps. With diagnostic events on,
  `CallbackBegin`/`CallbackEnd` (with duration) are recorded and
  `open_callbacks()` lists a callback that never returned beside that
  thread's open waits. On hardware an interrupt callback runs at raised
  IRQL and cannot block, so a long or open callback points at a Xenon
  semantics gap. Tested through a real session with a vsync callback that
  blocks (`xenon_gpu_pump_tests`) and deterministically
  (`xenon_diagnostic_events_tests`).
- **`RtlRaiseException`** (`54d36c2`) was a silent no-op. The MSVC
  thread-naming exception (`0x406D1388`, `THREADNAME_INFO` in
  `ExceptionInformation`) now names the thread, as xenia does; returning is
  correct there because the idiom wraps the raise in `__try`/`__except`.
  Any other exception is logged once per code and recorded as
  `ExceptionNotDispatched`; Xenon still has no guest SEH unwinder, so the
  call returns. `KernelThread::name()` now returns a copy under the
  thread's lock.

- **Terminated threads' waits end** (`e58d458`). `terminate()` could not
  interrupt a kernel wait, so a guest thread parked in an unbounded wait
  could never be stopped. A `KernelThread`'s waits now also register on its
  own thread, and return `Terminating` (`STATUS_THREAD_IS_TERMINATING`)
  without taking anything; `KeDelayExecutionThread` is interruptible too.
  `RtlEnterCriticalSection` takes no ownership in that case and leaves the
  waiter's `LockCount` registration, as hardware does. Reproducer
  `xenon_kernel_wait_termination_tests` timed out on `join()` before the
  change. Waiting for another thread to finish still uses `join()` and is
  not interruptible.
- **Stop and shutdown** (`3c20d56`). `stop()` now terminates the guest's
  threads after printing the stall report, so a guest blocked in a wait
  exits cooperatively instead of reaching the runtime host's hard `_Exit`.
  `shutdown()` terminates threads before joining the main thread and each
  pump, and terminates and joins every guest thread before unloading the
  native extension (the KNOWN ISSUE in `shutdown.cpp` is gone). Tested with
  a vsync callback and a worker both blocked in infinite waits
  (`xenon_gpu_pump_tests`); without the pump terminate, shutdown hangs.
  Analysis and plan for both by the earlier `xenon-recomp-ad` session.

Verification for the last two: 50 repetitions and 15 ThreadSanitizer runs
of the termination suite, no failures or reports; mutations (no
own-thread registration, no critical-section guard, no pump terminate)
each fail a test; session, kernel and export suites 42/42 and runtime-host
suites 6/6. Full CTest at `3c20d56`, Debug, `--parallel 2 --timeout 900`:
155/155 in 23 min 34 s (`prepare_worker` 889 s, compile-bound as before).

- **Callback and wait ordering** (`gpu_pump_tests`): a guest render thread
  waiting on an event the vsync callback sets sees ten frames within 5 s
  with no wait timing out.

Acceptance items (plan, V3-04) and where they are tested: concurrent
callback/wait ordering and blocked callbacks (`xenon_gpu_pump_tests`);
interruption of waits and delays (`xenon_kernel_wait_termination_tests`);
suspended creation (`xenon_kernel_thread_lifecycle_tests`,
`xenon_thread_creation_tests`); TLS identity per thread
(`xenon_thread_creation_tests`); shutdown with blocked threads
(`xenon_gpu_pump_tests`); guest faults (`xenon_thread_creation_tests`
crash case); exception boundaries (`RtlRaiseException` in
`xenon_xbox_threading_exports_tests`, setjmp/longjmp in
`xenon_cpu_setjmp_longjmp`). AC6 categorisation improves through the
callback watch and wait-type fix; unconfirmed until AC6 runs.

Decided not to change, with reasons (plan: semantics only with evidence):

- **IRQL.** Neither xenia nor Xenon keeps IRQL in guest memory; xenia
  tracks it host-side per thread and uses it mainly to deliver pending APCs
  when `KfLowerIrql` drops the level. Xenon has no APCs, and guest code only
  passes the returned previous IRQL back to the matching release, so a
  tracked value would change nothing a title can observe today.
- **Guest SEH unwinder.** Needs the PPC `.pdata`/unwind contract verified
  and evidence that a title raises a real exception; `ExceptionNotDispatched`
  events now provide that evidence from any run.
- **DPC delivery** (revised 11 October, see "User APCs" below). xenia and
  rexglue-sdk both stub `KeInsertQueueDpc` (`assert_always("DPC does not
  dispatch yet; going to hang!")`, nothing drains the list), and both AC6
  recomps (`sal063/AC6_recomp`, `rapidsamphire/AC6Recomp`, on rexglue-sdk
  with only input and codegen fixes on top) run in-game on that stub, so AC6
  does not depend on a DPC running. Xenon does not register the DPC exports;
  a title importing them is reported as an unresolved import.
- **Decoupling a blocked callback from its pump.** Needs the ordering model
  the plan asks for; the callback watch now measures whether AC6 needs it.
- **Join-then-detach** in `ThreadManager::shutdown()` and `~KernelThread()`
  is still reached by a thread spinning in compiled code with no block
  boundary; waits no longer reach it.

### User APCs (V3-04, 11 October 2026)

The owner asked whether Xenon needs APCs and DPCs; the evidence above was
re-checked against xenia, rexglue-sdk and the AC6 recomps. User APCs are
needed: titles queue them through I/O completion routines, timer routines,
XAM overlapped completions and `NtQueueApcThread`, and both references
deliver them when the thread next waits or sleeps alertably, returning
`STATUS_USER_APC`. Xenon had no queue, ignored the alertable flag, logged
and dropped `NtSetTimerEx` routines, and dropped every I/O completion APC
(the I/O bridge's APC hook was never installed).

- `7b5fc47`: per-thread user APC queue; alertable waits and delays return
  `WaitResult::UserApc` when an APC is pending and no object is
  satisfiable (an object is taken first, as on NT).
- `48821cb`: `ExportCallContext::guest_call`, implemented by the session as
  a nested dispatch on the calling thread below its stack
  (`call_guest_routine()`); alertable Ke*/Nt* waits and
  `KeDelayExecutionThread` run the queued APCs and return
  `STATUS_USER_APC`; producers `NtQueueApcThread`, `NtSetTimerEx` routines
  (to the setting thread) and I/O completion routines (to the issuing
  thread).
- `f87e662`: guest KAPC objects (`KeInitializeApc`, `KeInsertQueueApc`,
  `KeRemoveQueueApc`, 40-byte layout as in xenia and rexglue-sdk); the
  kernel routine runs first and may rewrite the normal routine, context and
  arguments. Kernel-mode KAPCs are accepted but not delivered, as in both
  references.

Tests: `xenon_kernel_user_apc_tests` (queue, alertable and non-alertable
waits and delays, ordering), `xenon_xboxkrnl_user_apc_export_tests`
(NtQueueApcThread, timer routine, KAPC kernel-then-normal routine,
removal, kernel-mode, bad targets), and the nested call through a real
session in `xenon_thread_creation_tests`. Mutations removing delivery or
the timer routine fail them; ThreadSanitizer clean over 15 runs.

- `3a99684`: XOVERLAPPED completion. Every XAM export ignored its
  overlapped and returned synchronously, and `XamGetOverlappedResult` was
  unregistered, so a title waiting on the overlapped's event or result
  waited forever. Calls given an overlapped now complete it at once (Result,
  ExtendedError, Length; event signalled; completion routine queued as a
  user APC) and return `ERROR_IO_PENDING`, as xenia and rexglue-sdk do with
  `CompleteOverlappedImmediate`; applied to the twelve registered exports
  that take one, reading a ninth argument and beyond from the stack.
  `XamGetOverlappedResult` (0x1FB) added. Tests: `xenon_xam_overlapped_tests`.
- `a7c8dcf`: thread alerts (`KeAlertThread`, `KeAlertResumeThread`,
  `KeTestAlertThread`; rexglue-sdk stubs them). An alertable wait or delay
  returns `STATUS_ALERTED` and clears the alert, before a pending APC as in
  NT. One flag per thread, since Xenon has no distinct processor mode.

Still open: kernel-mode APC delivery and DPCs (neither reference delivers
them; AC6 runs in-game without them); XAM exports that take an overlapped
but are not registered yet (V3-05's inventory).

Environment note: after the Codespace restarted, PulseAudio was no longer
installed, so sessions with audio enabled failed `initialize()` ("dsp: No
such audio device"); local runs use `SDL_AUDIODRIVER=dummy`. This is
weakness 2 (guest audio depends on a host device), owned by V3-14. An
interrupted build also left 15 objects with no recorded header
dependencies, which ninja then never rebuilt (a stale
`title_update_integration_tests.o` overflowed its stack-allocated
`XenonSession`); deleting them restored correct rebuilds.

## V3-05 progress

Started 11 October 2026. No title image or import list is available here,
so blockers are found by auditing the live registry against independent
references, not from an AC6 run.

**Ordinal check.** `xenon_export_ordinal_reference_tests` dumps the
production registry (`import-scanner --registry`, with audio) and checks
every registered (library, ordinal, name, kind) against pinned research
tables: xenia's xboxkrnl table (already used by the coverage dashboard) and
now its XAM table (`tools/coverage/reference/xenia_xam_997d055.json`).
Every registered ordinal was also checked against the independent xkelib
export lists (`xamext21256.0.def`, `kernelext21256.0.def`); the only
differences are spelling (`_swprintf`) and xkelib listing ordinal 296 as a
second `RtlFreeAnsiString` (the same routine). The check found one defect:

- `ed2678f`: `XamVoiceHeadsetPresent`, `XamVoiceSubmitPacket` and
  `XamVoiceClose` were rotated one place (0x30D-0x30F); a title's
  `XamVoiceClose` reached the `SubmitPacket` handler. 331/331 registered
  exports now match.

**ABI audit.** Each XAM handler's argument registers were compared with
xenia's typed export signatures (399 signatures from the xam and xboxkrnl
sources at the pinned commit), and every difference was checked by hand
against rexglue-sdk and xenia-canary:

- `bfa22a9`: `XamUserCreateAchievementEnumerator` and
  `XamUserCreateStatsEnumerator` take eight arguments with the buffer size
  and handle pointers in r9/r10; Xenon wrote a fixed fake handle through r7
  (the achievement offset) and left both outputs unwritten. Both now return
  real, empty enumerator handles (no XDBF achievement table is read; stats
  are online-only) and the required buffer size.
- `8f1354a`: `XamUserGetXUID` is (user_index, type_mask, xuid_ptr) and
  returns an HRESULT. Xenon took the XUID pointer from r4 (type_mask), so a
  title's call wrote eight bytes to guest address 1..7: a page-protection
  fault on the path every title takes at startup. The session test had
  encoded the same wrong ABI and now uses the documented one.
- `3be87c2`: profile settings. `XamUserReadProfileSettings` failed any
  request for settings with InvalidParameter (Xenon's own notes record an
  AC6 call asking for three) and answered no size query;
  `XamUserWriteProfileSettings` and `XamUserReadProfileSettingsEx` were
  missing. `xam::ProfileSettings` now holds the defaults the references give
  a new profile and per-title title-specific settings, saved under the save
  root. Unresolved between references: rexglue-sdk fails a read of a setting
  with no value and xenia-canary omits it; Xenon returns it with source 0
  (`XSOURCE_NO_VALUE`).

- `92f23fb`: `XeCryptSha` wrote all 20 digest bytes regardless of
  `output_size`; xenia, xenia-canary and rexglue-sdk write
  min(20, output_size) (Xenon's note claimed rexglue-sdk ignored it).
- `87e26bd`: `ExCreateThread` ignored r6, the `XapiThreadStartup` routine
  XAPI's `CreateThread` passes. xenia and rexglue-sdk start the thread
  there as `XapiThreadStartup(start_address, start_context)`; Xenon jumped
  to `start_address`, skipping XAPI's per-thread setup and exit.
- `d52f2a8`: `XCONTENT_DATA` is 0x134 bytes (two bytes of padding, both
  references); Xenon used 306, misaligning every enumerated item after the
  first.
- `73456af`: content packages. `XamContentCreateEx` mounted nothing, so a
  title's `save:\...` I/O after creating its save had no device, ignored
  the creation disposition and kept content only in memory. Packages are
  now host directories (xenia's layout, under `<save root>/Content`, or a
  scratch directory removed at shutdown), mounted as `<root>:`; the five
  dispositions, `XamContentCreate`, `XamContentClose`, `XamContentDelete`
  and `XamContentFlush` follow xenia.

- `2979fcd`: `XamContentDelete` reported success when the host could not
  remove the package (a file still open on Windows, a read-only
  directory); it now returns `ERROR_ACCESS_DENIED` and keeps it listed.
  Found when the package test aborted on Windows.
- `f95fb43`: XNotify listeners. Every listener shared one queue, masks and
  `match_id` were ignored, the ids were invented, and no startup
  notifications were sent, so a title waiting at boot for
  `XN_SYS_SIGNINCHANGED` would wait forever. A listener is now a waitable
  kernel event with its own queue, area mask and version filter; the first
  system listener gets the startup set xenia and rexglue-sdk send;
  profile-setting writes broadcast `XN_SYS_PROFILESETTINGCHANGED`.

Missing exports. Of the exports xenia marks implemented, 94 were not
registered in Xenon (excluding networking, which Xenon registers on
Windows only, about 70). Added so far:

- `e6f722a`: `XamShowMessageBoxUI` and `XamShowKeyboardUI`, answered as
  xenia answers them headless (focused button, default text, `XN_SYS_UI`
  on/off, overlapped completed); `XamIsUIActive`; `XamUserGetGamerTag`.
- `4f5f69d`: incremental SHA-1 and SHA-256, HMAC-SHA1, RC4,
  `XeCryptBnQw_SwapDwQwLeBe` and `RtlComputeCrc32`, checked against
  published vectors (FIPS 180-2, RFC 2202, RC4, CRC-32 check value).
  `XeCryptRc4Key` uses the key's length; the references read
  `key[x % 16]`. Coverage dashboard: 273 of 922 kernel exports registered.

Still missing from that list: AES, DES3 and the `XeKeys*` key functions;
I/O completion ports; `Ob*ByName` and symbolic-link objects;
`XexGetModuleSection`, `XexLoadImage`; `KeSetCurrentStackPointers`;
`ObLookupThreadByThreadId`; the XAM locale-string and resource-locator
helpers; `XamContentGetCreator` and `XamContentGetThumbnail`.

Unverified, left as they are: `XamQueryTimeZoneInformation` (no reference
implements it, so its ABI and structure are unknown), and the
`XamUserCheckPrivilege` grant-all offline model.

Tests: `xenon_export_ordinal_reference_tests`, `xenon_xam_export_ordinal_tests`
(voice ordinals and behaviour), `xenon_guest_enumerator_tests`,
`xenon_session_tests`, `xenon_xam_profile_settings_tests`,
`xenon_misc_export_tests`, `xenon_thread_creation_tests`,
`xenon_xam_content_package_tests`, `xenon_xam_notification_tests`,
`xenon_xam_ui_tests`. Each fix was
checked by reverting it: the test fails (the old `XamUserGetXUID` raises the
guest page-protection fault).

**AC6 import audit (19 October 2026, owner's Windows host).** First run
against a real title: AC6 `default.xex` (sha256 `6eefba42...bbbc`, the
owner's lawful local copy; nothing committed), Windows 11, MSVC 14.51,
Release. `import-scanner` first reported FAIL with 32 missing imports. All
were reporting defects, not missing exports:

- The ten xboxkrnl imports reported missing are the variable exports
  `kernel_variables.cpp` registers (`KeTimeStampBundle`, `XboxKrnlVersion`,
  `XexExecutableModuleHandle`, ...). The scanner and the session's
  `capability_report()` looked variable records up in the function
  namespace; the load path (`resolve_xex_imports`) did not, so execution
  was unaffected.
- `XamInputGetCapabilities`/`GetState`/`SetState`/`GetKeystrokeEx` were
  reported missing because the scanner's session had input disabled; a
  Normal Play session registers them.
- Every function import was counted twice (its FunctionAddress record and
  its thunk), and the scanner exited 4 whenever analysis left unresolved
  control flow, against its documented "nonzero only on FAIL" contract.

Fix: `classify_xex_import()` (`import_classification.cpp`) is now the one
rule for the load path, the capability report and the scanner; the scanner
builds its session like `--registry` (every compiled subsystem, fallbacks
reported). Corrected result: **239 imports, 0 missing, 202 implemented, 37
partial** (`PASS_WITH_FALLBACK`). The partial set is networking/Live,
`XMsg*` app registry, SEH/unwind (`RtlUnwind`, `RtlCaptureContext`,
`__C_specific_handler`, non-thread-naming `RtlRaiseException`),
`XamLoaderLaunchTitle`, STFS device and console-key exports, each with its
registry note. Tests: `xenon_import_capability_report_tests` (variable,
address-record and thunk cases) and `xenon_guest_import_negative_tests`
(a variable record in the live capability report); both fail against the
previous code. AC6's blockers are therefore not unregistered imports; the
next V3-05 evidence must come from running it.

Windows build fix found on the same host: FFmpeg's MSVC install puts
`avcodec.lib`/`avutil.lib` in `bin/`; the bootstrap's readiness check
searched `bin/` but `cmake/Dependencies.cmake` did not, so a fresh managed
Windows configure failed after reporting FFmpeg ready.

Second Windows defect: `xenon-prepare` on AC6 failed with "graph cache
write failed" at a 260-character `graph-v1/staging/<key>-<pid>-<n>/manifest.json`.
No Xenon executable declared `longPathAware`, so Windows applied
`MAX_PATH` even with the `LongPathsEnabled` policy on; any cache root longer
than about 110 characters broke preparation. Every MSVC executable now
embeds `cmake/windows/long-path-aware.manifest`. Regression:
`xenon_compilation_graph_tests` publishes and reads back a graph entry whose
staging manifest path exceeds 260 characters (aborts without the manifest;
skipped with a stated reason where the OS policy is off).

Third defect, in analysis (V3-07 territory, fixed here because it blocked
preparing AC6): code generation failed entry-integrity with "edge
0x821a6e10 -> 0x821a6d8c leaves owner 0x821a6df0". `0x821a6bc0` is a
method reached only through a vtable; it dispatches a 40-case switch
through an inline jump table. Discovery correctly gave it every case
block, but each case target also stayed a separate overlapping function,
because weak candidates were absorbed only into owners with semantic
boundary evidence and a vtable-only method has none. A case branching into
the middle of a sibling case then had no dispatchable entry. Absorption now
also accepts a root owner (a compiled function whose start is no other
function's block), which can never itself be absorbed, so no owner chain
can form. AC6's compiled function count fell from about 13,840 to 11,949.
Regression: `xenon_discovery_quality_tests` (a gap-recovered dispatcher
whose case A branches into case B); it fails under the old owner rule.

Fourth: MSVC's compiler and PDB writer stop at `MAX_PATH` even in a
long-path-aware process (C1041 thousands of objects into the module build).
`xenon-prepare` now rejects, before any work, a Windows cache root that
leaves fewer than `kNativeBuildPathHeadroom` (160) characters for the
deepest workspace path (measured 146). Regression in
`xenon_prepare_worker_tests`.

**AC6 first runs (same host).** AC6 prepares, loads, binds its module and
runs for about 9 s, then faults. Defects found from that run, all generic:

- *Imports reached by a tail branch ran twice.* `XenonSession::call`
  reports a completed import as `Fallthrough` at `cia`, which is right
  after a `bl` but not after `b thunk` from an argument-shuffling wrapper:
  the dispatcher resumed at the thunk and ran the import again with the
  first call's result as its first argument (seen as `NetDll_socket`/
  `NetDll_bind` pairs). `cpu::call_import_from_branch` makes a branch to an
  import complete as a `Return` to LR, in the guest dispatcher and the
  dynamic fallback. Regressions in `xenon_session_execution_tests` and
  `xenon_dynamic_fallback_tests`.
- *VDP sockets failed.* Guest protocol 254 (VDP) is now created as UDP on
  the host, as Xenia does. Regression in `xenon_xam_net_socket_tests`.
- *Fault reports printed junk callers.* They read LR at `frame+8` (the SysV
  slot); Xbox 360 prologues store it at `-8` from the caller's stack pointer.
  `walk_guest_stack()` follows the back chain correctly, and the report now
  includes r11, r12 and r14-r31. Regression: `xenon_guest_stack_walk_tests`.
- *The 9 s fault itself: `memset(p, 0, n)` filled memory with `0x2A`.* The
  native vector splat path emitted IR `imm0`, which holds the lifter's
  mnemonic tag, as the splat element/immediate. AC6's VMX memset
  (`0x821F3BF0`) builds its fill vector with `vspltisb`/`vspltb`, so
  zero-initialised structures were filled with `'*'` and a later
  dereference of a field faulted at `0x2A2A2A2A`. The immediate now comes
  from bits 16-20 of the guest word. Regression: `xenon_cpu_vector_splat`
  generates and executes all eight splat forms for all 32 field values
  (fails before the fix).
- *Pump state was missing from crash logs.* The GPU/audio pump evidence
  (callback in progress, vsync deliveries, audio credits) was only logged by
  a clean stop. It is now logged on any guest thread or pump callback crash.
  Regression in `xenon_session_execution_tests`.

Open from these runs: the earlier "GPU pump and audio only work once"
symptom has not been re-checked since the memset and double-import fixes;
both could plausibly cause it (zeroed ring/voice state filled with `0x2A`,
credit/fence handshakes run twice), but that is not established. "Unhandled
GPU command variant" warnings (indices 1, 2, 6, 7, 8) remain, and
`xenon-prepare` still hard-codes `--parallel 8` for the module build.

Kernel handlers use several argument idioms (`dispatch<N>`, helper
functions), so the automated comparison covered XAM fully and xboxkrnl only
where handlers read `gpr[N]` directly; the xboxkrnl hits were reviewed and
are address arguments xenia types as `dword_t`, not defects. Handlers that
delegate to helpers were then read by hand, which found the `XeCryptSha`
and `ExCreateThread` defects above.

## Dependency graph

```text
V3-00 -> V3-01 -+-> V3-02 -> V3-03 -> V3-04 -> V3-05
                |             |         |
                |             +---------+--> V3-14
                +-> V3-06 -> V3-07 -> V3-08 -> V3-09
                                        V3-02 + V3-09 -> V3-10
                                        V3-03 + V3-10 -> V3-11
                                        V3-02 + V3-11 -> V3-12 -> V3-13
                                        V3-02 .. V3-14 -> V3-15
                                        V3-03 .. V3-15 -> V3-16
                                        V3-06 .. V3-16 -> V3-17 -> V3-18
```

V3-06 may run alongside V3-02 to V3-05 because it touches only `src/recomp`
and the compilation graph.

## Integration contracts to keep stable

- `cpu::MemoryPort` / `MemoryAccessContext`: the only CPU path to guest memory.
- `core::ExportRegistry`: ordinals and handlers. The registry is the
  inventory source (`import-scanner --registry`).
- `graph::Store` and `Node::key()`: content-addressed cache identity.
  Producer hashes come from CMake-hashed sources.
- `Xenon_BindCompiledRegistry(ExecutionContext&)` and
  `Xenon_SupportedExecutableRevisions()`: keep until V3-16 publishes a
  versioned replacement and a migration path.
- `launch-config.json`, `status.json`, `log.txt`, `stop.signal`: the
  launcher and runtime host contract (`RUNTIME_HOST.md`).
- `tools/coverage/coverage.json`: human-reviewed approval record; automation
  never edits it.

## Risks

- **Concurrent sessions.** Another session edits this tree and pushes bot
  PRs. Stage files individually and fetch before any push.
- **Slow nested builds.** Tests that build generated projects take minutes
  each and hash `include/`, `src/` and `cmake/`; editing sources during a
  full CTest run can fail them.
- **Prepare-worker budget.** `xenon_prepare_worker_tests` needed 1,047 s
  alone on this 2-vCPU host against a 900 s CTest timeout. It passed within
  budget on earlier CI runs; if CI now times out, the fix is finding 3, not a
  longer timeout.
- **Disk.** About 4 GiB free here; a second full build directory may not fit.
- **No Windows, D3D12 hardware or title input locally.** V3-01's Windows fix
  and every AC6 claim need CI or the owner's machine.

## Decision log

| Date | Decision | Reason |
| --- | --- | --- |
| 2026-10-10 | This tracker is the single V3 status file; subsystem notes stay in their existing documents | The plan asks for no documentation sprawl |
| 2026-10-10 | The V3-00 audit itself changed no source | V3-00 is documentation-only |
| 2026-10-10 | After `03b26e5` broke every workflow, fix the build and coverage test, and pull the V3-03 wait fix forward rather than register a failing test or drop the reproducers | Owner decision; the defect was confirmed and the reproducers already existed |
| 2026-10-10 | Fixes stay local until the owner pushes | Owner decision |
| 2026-10-11 | Abandon mutants at the owning host thread's real exit, not at `terminate()` | `terminate()` can run while the thread is still inside the critical section its mutant guards |
| 2026-10-11 | A direct guest write to `SignalState` is adopted at the next `Ke*` call and wakes no blocked waiter | Matches NT dispatcher semantics; waking blocked waiters on memory writes would need polling that hardware does not do |
| 2026-10-11 | A profile setting with no value reads back as `XSOURCE_NO_VALUE` rather than failing the read | The references disagree (rexglue-sdk fails, xenia-canary omits); the documented source field covers the case without inventing data |
| 2026-10-14 | Strict AOT is a session/package policy: release packages run without Gen 7 or a runtime compile callback and stop with an `aot_missing_target` event on a miss; Gen 7 is an explicit development option | Owner's amended specification; today the fallback defaults on and interprets misses silently |
| 2026-10-14 | Launcher and standalone outputs come from one preparation graph, with thin hosts over the same session | Owner's amended specification; avoids two analysis pipelines and duplicated guest semantics |

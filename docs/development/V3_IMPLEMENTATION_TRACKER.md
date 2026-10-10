# Xenon V3 implementation tracker

This is the single maintained status record for the Xenon V3 programme
(Xbox 360 platform fidelity, advanced recompilation and adaptive
compatibility). It records what the tree actually does, measured on a named
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
| V3-01 | CI and build correctness | IN PROGRESS | `03b26e5` compile, coverage-test and ownership-audit failures fixed locally; Windows D3D12 debug-layer failure and Node 20 deprecation remain |
| V3-02 | Diagnostics contracts | NOT STARTED | |
| V3-03 | Guest scheduler and waits | IN PROGRESS | Atomic WaitAll and event-driven WaitAny done (pulled forward by owner decision to unblock CI); remaining items listed under V3-03 below |
| V3-04 | Kernel execution and interrupts | NOT STARTED | |
| V3-05 | Guest services and ABI | NOT STARTED | |
| V3-06 | Discovery database | NOT STARTED | |
| V3-07 | Deep analysis and decompiler views | NOT STARTED | |
| V3-08 | Adaptive knowledge | NOT STARTED | |
| V3-09 | AOT compilation | NOT STARTED | |
| V3-10 | CPU semantics and verifier | NOT STARTED | |
| V3-11 | Memory fidelity and efficiency | NOT STARTED | |
| V3-12 | Xenos command frontend | NOT STARTED | |
| V3-13 | Vulkan/D3D12 parity | NOT STARTED | |
| V3-14 | Audio and timing | NOT STARTED | |
| V3-15 | Replay and compatibility lab | NOT STARTED | |
| V3-16 | Standalone SDK and host | NOT STARTED | |
| V3-17 | Preparation and packaging | NOT STARTED | |
| V3-18 | Cross-title qualification | NOT STARTED | |

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
| Kernel objects (events, semaphores, mutants, timers) | implemented+tested individually | Single-object waits are covered by `xenon_kernel_synchronization_tests` |
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
10. **Kernel objects read guest `SignalState` once** (confirmed in source).
    `resolve_dispatcher_object()` (`src/xbox/xex_dispatcher_header.cpp`)
    reads `SignalState` only when it creates and stashes the host object;
    later guest writes to that field are not observed. Whether titles write
    it directly is unverified (the AC6 handshake-event probe asks exactly
    that). V3-03.
11. **The capability verdict cannot see a stall.** It reports `FAIL` only
    for a recorded session error or an unbound native module. V3-02, V3-15.

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

<!-- V3-00-BENCH -->

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

<!-- V3-03-RESULTS -->

Still open in V3-03:

- Guest `SignalState` read once (weakness 10).
- `WaitResult::Abandoned` is never reported; an abandoned mutant satisfies a
  wait as an ordinary success, as before this change.
- Duplicate objects in one WaitAll are consumed once; Xbox kernel behaviour
  is unverified.
- Every signal wakes every registered multi-object waiter. The cost under
  many concurrent waiters has not been measured.
- Alertable waits, APC delivery during waits, and mixed `Ke*`/`Nt*` flows on
  the same guest object need tests through the export registry.
- Timer firing through `TimerManager` is not covered by the latency test,
  which uses an immediate `set(0)`.

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

# Test Coverage Audit — AC6 Runtime Readiness pass, Part 18

Part 18 asked for a final sweep of test coverage against the plan's
minimum list. Each part of this pass (7 through 15, and 17) already added
or verified real tests as part of doing the work, rather than deferring
testing to a separate pass - see each part's own `docs/` report
(`GPU_CAPABILITY_AUDIT.md`, `EDRAM_RESOLVE_AUDIT.md`,
`SHADER_COVERAGE_REPORT.md`, `MEMORY_GPU_COHERENCY.md`,
`ASYNC_IO_DIAGNOSTICS.md`, `TITLE_UPDATE_FIDELITY_REPORT.md`,
`BOOT_CHECKPOINTS.md`, `EXCEPTION_SETJMP_LONGJMP_AUDIT.md`,
`AC6_CAPABILITY_VERDICT.md`) for what was tested and why. This document is
the cross-part sweep, not a restatement of each one.

## Confirmed real, passing coverage per part

- Part 7 (GPU capability/silent fallback): `xenon_gpu_frontend_tests`,
  `xenon_shader_translation_tests`, `xenon_session_tests` assert the real
  `GpuUnsupportedCounters` fields against live backend behavior, not fixed
  values.
- Part 8 (EDRAM/resolve): `xenon_texture_tests`/backend capability tests
  assert real MSAA-fallback padding-sample content.
- Part 9 (shader coverage): `xenon_session_tests` asserts real
  `GpuShaderCoverage` fields.
- Part 10 (memory/GPU coherency): `texture_cache_invalidations` asserted
  against real dirty/rebind behavior in the backend tests.
- Part 11 (async I/O diagnostics): `xenon_kernel_io_tests`'s
  `test_async_request_submit_and_completion_are_logged` asserts real log
  output from real async request lifecycle.
- Part 12 (title update fidelity): `xenon_title_update_integration_tests`
  asserts real base/effective SHA1 and version-string reporting against
  real fixture XEX+TU pairs.
- Part 13 (exception/setjmp-longjmp): confirmed via
  `xenon_kernel_exception_tests`, `xenon_cpu_setjmp_longjmp` (real codegen
  round trip), `xenon_cpu_runtime_helpers_register_range`, plus this pass's
  new `xenon_thread_creation_tests`
  (`test_two_concurrent_created_threads_have_independent_tls`).
- Part 14 (fallback accounting): `xenon_session_tests` asserts real
  `fallbackUniquePcCount`/`fallbackHotPcTopN` fields.
- Part 15 (boot checkpoints): `xenon_boot_checkpoint_tests`,
  `xenon_session_tests`, `xenon_guest_export_abi_tests` assert real
  checkpoint-reached ordering from real `load_game()`/`start()` calls.
- Part 17 (capability verdict): `xenon_session_tests`'s three new verdict
  assertions (PASS / PASS_WITH_FALLBACK / FAIL, each driven by a real
  signal, not a mocked one).

## Genuine gap: reviewer addition 4, multi-mission soak test

The reviewer's fourth addition asked for a soak test that runs a title
across multiple missions and checks for state/resource leakage between
them. This repository has no real AC6 game data (disc image, XEX, or
title-update files) checked in - none was found anywhere in the tree - and
none should be, since that is the user's own licensed content, not
something this project ships. A genuine "multi-mission" soak test requires
actually running AC6 across mission boundaries, which is not something
this pass can execute or fabricate here.

What *is* achievable and already exists without real game data: repeated
create/run/join cycling of synthetic guest threads under the real
`ExCreateThread` production path, which is exactly what
`tests/core/thread_creation_tests.cpp`'s existing tests already do (create,
suspend/resume, preemptive terminate, and now two-thread TLS isolation -
all through real repeated lifecycle transitions). Extending that into a
long-running iteration-count stress loop was considered, but a synthetic
loop revealing "no leak after N synthetic thread cycles" is not the same
claim as "no leak after N real missions", and dressing the former up as a
substitute for the latter would misrepresent what was actually verified.

**`STATUS: INCOMPLETE`** for the multi-mission soak test specifically.
Blocked on the absence of real AC6 title data in this environment; not a
scheduling deferral. If AC6-specific test fixtures become available (in
the game module the user maintains separately, per the Architecture rule -
Xenon itself never carries AC6-specific fixtures), a real multi-mission
soak test should be added at that layer, exercising this same
`XenonSession` lifecycle across real mission transitions and asserting
handle-table size, allocator live-block count, and `capability_report()`'s
verdict stay stable rather than growing across missions.

## Test run confirming this audit

Full solution rebuild (Debug) and full `ctest` run performed as part of
this pass; see the pass's finish report for the exact pass/fail counts.

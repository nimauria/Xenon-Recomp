# Exception / EH / setjmp-longjmp Audit — AC6 Runtime Readiness pass, Part 13

Part 13 asked for a smoke-test audit of six scenarios: normal return, the
exception path, setjmp -> longjmp, TLS on a secondary thread, the
register-save helper, and the register-restore helper. All six already have
real production implementations; this pass's job was to verify genuine test
coverage for each and close any real gap found, not to design new runtime
behaviour.

## Audit result, scenario by scenario

- **Normal return.** Every codegen smoke test
  (`xenon_cpu_function_flow`, `xenon_cpu_branch_matrix`,
  `xenon_cpu_control_boundaries`, etc., wired in `CMakeLists.txt` via
  `xenon_add_generated_cpu_test`) compiles real PPC `blr` returns through the
  full `StaticFunctionCompiler` -> `CppAotBackend` pipeline and executes the
  generated native code. Ordinary return is exercised on every run of the
  existing CPU test suite; no gap.

- **Exception path.** `tests/kernel/exception_tests.cpp`
  (`xenon_kernel_exception_tests`) already covers `fault_to_exception()`'s
  read/write/execute-fault-to-`AccessViolation` mapping with the real
  `ExceptionInformation` values Windows/PPC fault reporting would produce,
  and `ExceptionDispatcher`'s real handler-chain semantics: most-recently
  registered handler tried first, per-thread chains independent of each
  other, a thread with no handler correctly falling back to the process-wide
  chain, thread handlers tried before the process-wide chain, thread-id 0
  never registrable, and `clear_thread_handlers()` removing a whole chain.
  Real, substantial, passing coverage; no gap.

- **setjmp -> longjmp.** `tests/cpu/codegen/codegen_setjmp_longjmp.cpp` +
  `tests/cpu/x86_64/setjmp_longjmp.cpp` (`xenon_cpu_setjmp_longjmp`) is an
  end-to-end test: real PPC machine code (`mflr`/`bl`/`longjmp`, etc.) is
  compiled through the actual `StaticFunctionCompiler`/`CppAotBackend`
  codegen path into a generated translation unit, which is then compiled and
  linked into the test binary and actually executed - this is not a test
  that calls `runtime_helpers::setjmp_v2`/`longjmp_v2` directly, it is guest
  code doing the call. It verifies GPR/FPR/VR preservation across the jump,
  jump-buffer field offsets, independent jump buffers, TLS and
  `kernel_data()` preservation across the jump on a real `KernelThread`,
  zero-longjmp-value normalization, and invalid/unaligned/unmapped
  jump-buffer trap behaviour. Real, substantial, passing coverage; no gap.

- **Register-save / register-restore helper.**
  `tests/cpu/x86_64/runtime_helpers_register_range.cpp`
  (`xenon_cpu_runtime_helpers_register_range`) exercises these directly.
  Real, passing coverage; no gap.

- **TLS on a secondary thread — real gap, now closed.** The existing
  setjmp/longjmp test proves TLS survives a jump, but only on a single
  manually-constructed `KernelThread` built with the test-only
  `xenon::kernel` API, not through the production `ExCreateThread` path real
  guest code actually uses (`XenonSession::export_ex_create_thread()` ->
  `setup_guest_thread_tls_context()`, the same call the main thread's own
  startup makes). No existing test proved that two *concurrently executing*
  `ExCreateThread`-created threads actually receive independent KPCR/TLS
  allocations, as opposed to two independent calls made back-to-back with no
  concurrent execution (`tests/core/session_tests.cpp`'s guest-thread-context
  test) or a single thread's TLS surviving in isolation. An aliasing bug in
  `setup_guest_thread_tls_context()`'s allocator would have gone completely
  undetected by every existing test.

## What this pass added

`tests/core/thread_creation_tests.cpp`: a new
`test_two_concurrent_created_threads_have_independent_tls()` creates two
guest threads through the real `ExCreateThread` export handler, both running
the same compiled entry function concurrently. Each thread reads its own
KPCR address from `gpr[13]` (exactly as `run_created_guest_thread()` sets it
from `setup_guest_thread_tls_context()`'s output), writes a distinct tag into
guest memory at that address, waits for both threads to have written (a
shared atomic barrier, so a real aliasing bug has the chance to manifest as
one thread's write clobbering the other's), then reads its own address back
and records what it saw. The test asserts both KPCR addresses differ and
each thread read back its *own* tag, not the other's - proving the real
per-thread allocation is genuinely non-aliasing under real concurrent guest
execution, not just under sequential unit-level construction.

This test passed against the existing implementation on first run - it found
no bug, and closes a real, previously-untested path rather than adding
synthetic coverage of something already proven elsewhere.

## Tests run

- `xenon_thread_creation_tests` (rebuilt with the new test): all tests pass,
  including the new TLS isolation test.
- `xenon_kernel_exception_tests`: all pass (unchanged, re-run for
  confirmation).
- `xenon_cpu_setjmp_longjmp`: passes (unchanged, re-run for confirmation).
- `xenon_cpu_runtime_helpers_register_range`: passes (unchanged, re-run for
  confirmation).

## Remaining gaps

None identified for Part 13's six-scenario smoke-test list.

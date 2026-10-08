# Findings from the structural refactor

This note records defects exposed during the structural refactor. Open
findings remain below; resolved findings record the observed cause and fix.

## Build and test defects

1. **D3D12 backend does not compile without DXC.**
   `src/graphics/d3d12/backend.cpp` uses `Impl::shader_cache` and
   `Impl::decoded_shaders` outside the `#ifdef XENON_HAS_DXC` blocks that
   declare them: in the shader-load path, in `performance_counters()` and in
   `shader_coverage()`. A Windows build with `XENON_ENABLE_D3D12=ON` and no
   resolvable DXC fails. Observed with the MinGW + DirectX-Headers syntax
   check described in `TESTING.md`.

2. **`xenon_backend_capability_tests` cannot pass on non-Windows hosts.**
   `discover_backend_capabilities()` (`src/graphics/windows/backend_capabilities.cpp`)
   sets `runtime_available` only on the `_WIN32` path. The test asserts
   `capabilities[0].runtime_available` whenever `XENON_TEST_VULKAN` is
   defined, so it aborts on Linux even with a working Vulkan device
   (verified with Mesa llvmpipe).

3. **`XenonSession::mount_content()` is a no-op.** It ignores both
   arguments and returns success with a message pointing at
   `mount_content_graph()`.

## Resolved build and test defects

**Preparation identity hashed the whole compiler directory.**
`graph::preparation_identity()` (`src/recomp/compilation/graph/compilation_graph.cpp`)
fingerprinted every file under `native_compiler.parent_path()` recursively.
On Linux that included about 550 MB in `/usr/bin`, making preparation slow
and invalidating cached modules when unrelated tools were installed. The
preparation key now hashes the compiler executable and increments its
producer version. `compilation_graph_tests` checks that changing an unrelated
neighbouring file leaves the key stable while changing the compiler changes
it.

## Runtime observations

See `docs/runtime/AC6_RUNTIME_INVESTIGATION.md`. In short:

- Audio render callbacks stop after `kMaxQueuedRenderFrames` (8)
  invocations unless the guest submits frames.
- The GPU and audio pumps run guest callbacks synchronously, with no
  watchdog.

## Code-quality notes

- `src/xbox/rtl.cpp`: `read_guest_be16` is defined but unused
  (`-Wunused-function`).
- Several pre-existing `-Wunused-parameter` warnings in RuntimeServices
  overrides (`syscall`, `trap`, `read_spr`, `write_spr`, `read_time_base`)
  and in `mount_content()`.
- About forty `*_diag.log` investigation probes outside the session still
  call `fopen` directly. They are inventoried in the AC6 note.

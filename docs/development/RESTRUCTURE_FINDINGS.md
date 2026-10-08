# Findings from the structural refactor

This note records defects exposed during the structural refactor. Open
findings remain below; resolved findings record the observed cause and fix.

## Build and test defects

1. **Neither GPU backend compiles without DXC.**
   `performance_counters()` and `shader_coverage()` in both
   `src/graphics/vulkan/backend.cpp` and `src/graphics/d3d12/backend.cpp` use
   `shader_cache` and `decoded_shaders` outside the `#ifdef XENON_HAS_DXC`
   blocks that declare them. A build with a GPU backend enabled and no
   resolvable DXC fails. Observed for D3D12 with the MinGW + DirectX-Headers
   syntax check described in `TESTING.md`, and for Vulkan by compiling
   `backend.cpp` without `XENON_HAS_DXC`.

2. **`XenonSession::mount_content()` is a no-op.** It ignores both
   arguments and returns success with a message pointing at
   `mount_content_graph()`.

3. **`xenon_xbox_threading_exports_tests` hangs intermittently.** Its
   `slist_concurrent` case (four threads pushing 1,200 entries through
   `InterlockedPushEntrySList` while four pop them) occasionally never sees all
   entries popped, so the poppers spin until CTest's 900 s timeout. Locally the
   test hung in 2 of 15 runs on this branch and 4 of 30 runs built from
   `780d017`, before any code moved, so the defect predates the refactor.
   The SList exports (`src/xbox/exports/xboxkrnl_threading_exports.cpp`) rely
   on `AddressSpace::reserve64()`/`store_conditional64()`
   (`src/memory/guest/reservations/reservations.cpp`) to reject a store over a
   header another thread changed; a lost entry points at a
   window where a stale reservation still commits.

## Differences between the Vulkan and D3D12 backends

Both backends now share `src/graphics/common/backend_core*.hpp`. Where their
behaviour differed, the shared code keeps each backend's behaviour behind a
hook rather than choosing one. Whether each difference is intended is still
open:

- **Render area for color draws.** Vulkan uses the surface pitch and
  `scissor_bottom`. D3D12 uses the bound render target's width and height.
  Render targets are only replaced when they need to grow, so on D3D12 the
  height can exceed `scissor_bottom`, which changes the default viewport and
  the scissor clamp.
- **Negative viewport scale.** D3D12 takes the absolute value of the guest X
  and Y scale. Vulkan passes the signed values, so a negative X scale becomes
  a negative viewport width, which Vulkan does not allow (only a negative
  height is permitted).
- **`pipeline_cache_misses`.** Vulkan reports the number of native pipelines;
  D3D12 reports the number of distinct guest pipeline-state hashes.
- **`unsupported_sampler_behaviors`.** Vulkan adds a texture's count each time
  the texture image is created; D3D12 reports the count accumulated by its
  descriptor layout across `bind_texture()` calls.

These are API differences rather than open questions: D3D12 binds dummy render
targets for unused color slots where Vulkan leaves them undefined, and it
emulates a cull-both memexport draw with an empty scissor where Vulkan uses
`VK_CULL_MODE_FRONT_AND_BACK`.

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

**`xenon_backend_capability_tests` could not pass on Linux.**
`discover_backend_capabilities()` (now `src/graphics/common/backend_capabilities.cpp`)
set `runtime_available` only on the `_WIN32` path, so the test aborted on
Linux even with a working Vulkan device. Linux now loads `libvulkan.so.1` and
queries `vkEnumerateInstanceVersion`, as Windows does with `vulkan-1.dll`.
That let the test reach its depth-sample checks, which then failed on Mesa
llvmpipe: a D24S8 depth of 0.875 written through `SV_Depth` read back as
`0xE00000` instead of `0xDFFFFF`. Vulkan allows a float-to-UNORM conversion to
return either neighbouring integer when the scaled value is not exact, so the
test now accepts one unit of D24S8 depth error; stencil and D24FS8 depth still
compare exactly.

**The Windows build and tests had never completed in CI.** Once the pinned
FFmpeg built (MSYS2 tools first on `PATH`), each later Windows stage exposed a
defect that predates the refactor:

- `src/graphics/xenos/shader_translation.cpp` held a 17,345-byte HLSL raw
  string, past MSVC's 16 KB per-literal limit (C2026). It is now two adjacent
  literals with byte-identical content.
- The runtime host linked with `/MAP:launcher/xenon_runtime_host.map`, a path
  relative to the build root that exists only when the launcher is built
  (LNK1104). Plain `/MAP` writes the map beside the executable.
- `tools/deps/bootstrap.py` compared a resolved patch path with an unresolved
  root; on Windows `resolve()` expands 8.3 short names, so patches under a
  temp directory were rejected.
- `xenon-prepare` named its build workspace with a 64-hex-digit digest, which
  pushed nested object paths past the 260-character `MAX_PATH` (C1083). The
  name is now the first 16 digits.
- GitHub's Windows runners have a Vulkan loader but no Vulkan driver and no
  hardware D3D12 adapter. Tests now skip device checks where no device exists,
  using a loader-level probe (`tests/support/vulkan_probe.hpp`) so a Xenon
  regression still fails where a device is present.

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

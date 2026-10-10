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

2. Resolved; see "`XenonSession::mount_content()` reported success" below.
   The number is kept because other documents cite findings by number.

3. **`xenon-prepare` builds the Xenon runtime once per module.** Each
   module's workspace configures the generated project with
   `add_subdirectory(${XENON_RECOMP_ROOT})`, so a title with several modules
   compiles the whole runtime several times. `xenon_prepare_worker_tests` does
   three such builds and passed at 783 s and 540 s of its 900 s budget on
   Windows CI (runs 37967218940, 37968759726). The same applies to every test
   that builds a generated project.

4. **The D3D12 stencil-reference path checks an interface, not the
   capability.** `src/graphics/d3d12/backend.cpp` sets separate front/back
   stencil references whenever `ID3D12GraphicsCommandList8` is available;
   `OMSetFrontAndBackStencilRef` requires the same `OPTIONS14` capability the
   pipeline stream now checks. Not yet observed failing.

5. **Unverified: the reservation commit handshake relies on x86 ordering.**
   Plain writers increment `active_coherency_writers` and then load the commit
   gate, while a conditional store acquires the gate and then loads the
   counter, using acquire/release rather than sequentially consistent
   operations. That store-then-load exclusion is guaranteed on x86-64, the
   only supported host, but not by the C++ memory model in general.

6. **Unverified: `InterlockedPopEntrySList` reads the head entry's link
   after reserving the header.** If another thread pops and frees that entry
   and its page is released, the read faults instead of retrying, as the real
   kernel arranges. Guest memory normally stays mapped, so this has not been
   observed.

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

**`XenonSession::mount_content()` reported success.** It ignored both
arguments, mounted nothing and returned success with a message pointing at
`mount_content_graph()`. Nothing called it. It now returns a failure naming
`mount_content_graph()`, and `xenon_session_tests` asserts that.

**Preparation identity hashed the whole compiler directory.**
`graph::preparation_identity()` (`src/recomp/compilation/graph/compilation_graph.cpp`)
fingerprinted every file under `native_compiler.parent_path()` recursively.
On Linux that included about 550 MB in `/usr/bin`, making preparation slow
and invalidating cached modules when unrelated tools were installed. The
preparation key now hashes the compiler executable and increments its
producer version. `compilation_graph_tests` checks that changing an unrelated
neighbouring file leaves the key stable while changing the compiler changes
it.

**Preparation identity read the whole Windows SDK.** On Windows CI
`xenon_compilation_graph_tests` timed out at 900 s; on Linux it takes under a
second. `graph::preparation_identity()` also content-hashed every file under
the directories `INCLUDE` and `LIB` name, which under the MSVC developer
environment are the MSVC, ATL/MFC, NETFX and Windows SDK header and library
trees. The test computes the identity three times in a debug build;
locally, pointing `INCLUDE` at 88 MB of headers alone added 64 s.
`xenon-prepare` paid the same cost once per module. Those system trees are
now fingerprinted by path, size and last-write time (producer `prepare-12`);
Xenon's sources, CMake and the compiler are still content-hashed. With
`INCLUDE` and `LIB` covering 862 MB the test takes 1.6 s, and it checks that
size, timestamp and added-file changes still change the identity.

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
- GitHub's Windows runners have a Vulkan loader but no Vulkan driver. Tests
  now skip Vulkan device checks where no device exists, using a loader-level
  probe (`tests/support/vulkan_probe.hpp`) so a Xenon regression still fails
  where a device is present. The runners do expose a D3D12 device: the
  Microsoft Basic Render Driver creates a feature-level 12_0 device without
  the software-adapter flag, so the D3D12 checks run there.

**`xenon_xbox_threading_exports_tests` hung intermittently.** Its
`slist_concurrent` case hung in about 1 of 40 local runs and timed out on
every recent Windows CI run; it predates the refactor (4 of 30 runs built from
`780d017`). The cause was a lock-order inversion in the guest reservation
monitor, not the SList algorithm. `store_conditional{32,64}()` (and
`physical_write_window()`) take the reservation commit gate and then wait for
`active_coherency_writers` to drain. A plain store in
`reservation_monitor_detail::enter_write()` (`include/xenon/cpu/memory_port.hpp`)
counted itself as an active writer first and then, when its 128-byte granule
had ever held a reservation, waited for that gate to be released. Each side
waited for the other forever. `InterlockedPushEntrySList` writes
`entry->Next` with a plain store, and the test's entries share a granule with
the list header, so pushers and conditional stores met in exactly this state.
Thread stacks of a stalled run showed one thread in `store_conditional64()`
and the plain writers in `enter_write()`'s gate wait. A writer now waits for
either gate only while uncounted, backing out both counts if a gate was
raised after it counted itself. `xenon_memory_tests` races conditional and
plain stores in one granule under a no-progress watchdog: it stalled in 5 of
5 runs before the fix and passes after it.

**D3D12 draw pipelines failed on Windows 10 and Server 2022 runtimes.**
`d3d12::GraphicsPipeline` (`src/graphics/d3d12/pipeline.cpp`) built every
pipeline through a state stream with a `DEPTH_STENCIL2` subobject whenever the
device exposed `ID3D12Device2`. That subobject exists only in runtimes that
report `D3D12_OPTIONS14::IndependentFrontAndBackStencilRefMaskSupported`
(Agility SDK 1.610+ or the Windows 11 24H2 inbox runtime); the windows-2022
runner reports it unsupported, and `xenon_backend_capability_tests` aborted at
pipeline creation while DXC-built transfer pipelines on the same device, made
with `CreateGraphicsPipelineState`, succeeded. The stream path is now gated on
that capability, with the existing legacy description (which rejects divergent
front/back stencil masks) as the fallback. CI run 37968759726 shows the
pipeline building through the legacy path and the functional D3D12 checks
passing on the Basic Render Driver.

**The FFmpeg patch test wrote a CRLF patch on Windows.**
`tests/deps/bootstrap_tests.py` wrote its synthetic patch with
`Path.write_text()`, which translates newlines on Windows, so `git apply`
rejected the fixture's own patch. Pinned patches are `eol=lf` and hash-pinned,
so production was unaffected; the fixture is now written as bytes.

**Nested generated-project builds were serial on Windows.**
`recomp_driver`, `registry_numeric_format`, `guest_export_abi` and
`audio_guest_callback` tests build a generated project against a snapshot of
the Xenon tree. Their nested configure named no generator, so Windows used the
Visual Studio generator, which compiles a project's files one at a time; two
of them also built without `--parallel`. `registry_numeric_format` timed out at
900 s and later passed at 873 s. Nested builds now use the outer build's
generator and compiler (`tests/support/nested_cmake.hpp`) with
`--parallel 4`.

## Runtime observations

See `docs/runtime/AC6_RUNTIME_INVESTIGATION.md`. In short:

- Audio render callbacks stop after `kMaxQueuedRenderFrames` (8)
  invocations unless the guest submits frames.
- The GPU and audio pumps run guest callbacks synchronously, with no
  watchdog.

## Code-quality notes

- `src/xbox/rtl.cpp`: `read_guest_be16` is defined but unused. It has
  external linkage (it sits after the anonymous namespace), so no warning
  reports it.
- The Debug build's `-Wunused-parameter` warnings (RuntimeServices
  overrides, `mount_content()`) and ignored `[[nodiscard]]` results of
  `KernelThread::terminate()` were cleared without behaviour changes.
- About forty `*_diag.log` investigation probes outside the session still
  call `fopen` directly. They are inventoried in the AC6 note.

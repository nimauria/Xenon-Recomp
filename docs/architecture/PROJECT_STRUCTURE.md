# Project structure

This is the source-tree ownership guide for Xenon. `CMakeLists.txt`, the
configured CMake target graph, and executable tests determine what currently
builds and works. Documentation does not override those sources of truth.

## Boundary rules

- Xenon core implements reusable Xbox 360 mechanisms. Title-specific symbols,
  patches, render hooks, and content identities belong in game projects.
- Guest-visible CPU, memory, kernel, Xbox, XAM, and Xenos semantics must not
  depend on a host graphics API or launcher UI.
- The launcher and runtime host are process boundaries. The launcher manages
  libraries, settings, and launching; `xenon_runtime_host` executes guest code.
- Headers in `include/xenon/` should describe interfaces legitimately shared
  across targets. Implementation helpers belong beside their source files.
  Some current Vulkan and D3D12 helper headers still live in the public tree;
  that placement is technical debt, not a reason to add more there.
- When one subsystem spans several translation units, its private declarations
  live in a `*_internal.hpp` beside them (for example
  `src/core/session/session_internal.hpp`) and are included through the
  target's private `src/` include directory, as
  `"core/session/session_internal.hpp"`.
- Source files need an owning CMake target. Standalone C++ tests need a target
  and CTest registration, except build generators and benchmarks.

## Runtime and tool targets

| Owner | Shared headers | Implementation | Main target |
| --- | --- | --- | --- |
| Logging | `include/xenon/logging/` | `src/logging/` | `xenon_logging` |
| Core session, dispatch, diagnostics | `include/xenon/core/` | `src/core/` | `xenon_core` |
| Guest export registry (name/ordinal tables) | `include/xenon/core/export_registry.hpp` | `src/core/export_registry.cpp` | `xenon_export_registry` |
| CPU decode, IR, optimization, AOT | `include/xenon/cpu/` | `src/cpu/` | `xenon_cpu` |
| Guest address space and host VM | `include/xenon/memory/` | `src/memory/` | `xenon_memory` |
| Host kernel mechanisms | `include/xenon/kernel/` | `src/kernel/` | `xenon_kernel` |
| XEX format: parse, decrypt, decompress, title updates | `include/xenon/xbox/xex_*.hpp` | `src/xbox/xex/` | `xenon_xex` |
| XEX image mapping into guest memory | `include/xenon/xbox/xex_loader.hpp` | `src/xbox/xex/loading/` | `xenon_xex_loading` |
| Xbox guest exports, imports, module registry, guest I/O | `include/xenon/xbox/` | `src/xbox/` | `xenon_xbox_kernel_io` |
| XAM services and exports | `include/xenon/xam/` | `src/xam/` | `xenon_core` |
| Filesystem and content | `include/xenon/filesystem/` | `src/filesystem/` | `xenon_filesystem` |
| Graphics frontend and common data | `include/xenon/gpu/` | `src/graphics/xenos/`, `src/graphics/common/` | `xenon_graphics` |
| Vulkan host renderer | `include/xenon/gpu/vulkan/` | `src/graphics/vulkan/` | `xenon_graphics_vulkan` |
| D3D12 host renderer | `include/xenon/gpu/d3d12/` | `src/graphics/d3d12/` | `xenon_graphics_d3d12` |
| DXC shader compiler | `include/xenon/gpu/dxc_shader_compiler.hpp` | `src/graphics/dxc/` | `xenon_graphics_dxc` |
| Audio | `include/xenon/audio/` | `src/audio/` | `xenon_audio` |
| Input | `include/xenon/input/` | `src/input/` | `xenon_input`, `xenon_input_guest` |
| Network service client | `include/xenon/network/` | `src/network/` | `xenon_network` |
| Recompiler and analysis | `include/xenon/recomp/` | `src/recomp/` | `xenon_recomp` |

`tools/` contains host utilities and dependency/release tooling. `launcher/`
contains the Qt UI and its own tests. `runtime_host/` contains the guest-running
process. `tests/` follows the owner above, and `third_party/` holds committed
third-party material. Empty or reserved source directories do not imply a
working subsystem.

### CPU and memory

`src/cpu/ppc/` decodes (`decoder/`) and lifts (`lifter/`) the guest ISA; `ir/`
and `optimizer/` (with `optimizer/passes/`) own the intermediate representation
and its transformations; `codegen/` emits native C++ ahead of time
(`emission/`, `aot/`) and holds the dynamic fallback interpreter
(`fallback/`); `execution/` runs compiled and fallback code. CPU memory access goes through
`cpu::MemoryPort`; `src/memory/guest/` owns guest mapping, permissions, aliases,
reservations, physical backing, and coherency, one directory per concern
(`virtual/`, `physical/`, `access/`, `reservations/`, `coherency/`, ...). `src/memory/mapping/` selects the
platform host-VM implementation. Neither CPU nor GPU should create a separate
copy of guest RAM.

### Kernel, Xbox, and XAM

`src/kernel/` owns host-side handles, threads, synchronization, timers, memory
integration, and I/O mechanisms. `src/xbox/` owns guest-facing Xbox exports,
import resolution, and XEX/module compatibility; the XEX loader is split by
stage under `src/xbox/xex/` (`format/`, `security/`, `compression/`, `image/`,
`imports/`, `loading/`, `title_updates/`). XEX format work is its own
target, `xenon_xex`, which links no guest kernel or guest memory library and
builds with both disabled; only `loading/` (`map_xex_image()`, `load_xex()`)
needs guest memory and forms `xenon_xex_loading`. `xenon_xbox_kernel_io` keeps
the guest ABI bridge and links both XEX targets publicly, so its consumers,
including generated projects, are unchanged. `xenon_recomp` links `xenon_xex`
and `xenon_kernel` (its runtime helpers use guest heaps, its intake reads GDFX
images), not the guest export bridge, so `xenon-prepare`, `recomp-driver` and
`module-inspector` no longer link it. The guest I/O bridge (`GuestIoBridge`,
which publishes host I/O results into guest memory) is implemented in
`src/xbox/guest_io/` and built into `xenon_xbox_kernel_io`; its public header
is still `include/xenon/kernel/xbox_io_guest.hpp` in namespace
`xenon::kernel::xbox`, because `XenonSession`'s public header exposes it.
The export registry is its own target, `xenon_export_registry`, linked by
both `xenon_core` and `xenon_xbox_kernel_io`; the bridge registers its exports
through it and no longer depends on `xenon_core`, which links the bridge.
Tests that still link `xenon_core` only to reach the registry can link the
bridge directly; narrowing them is left to the build-ownership pass.
`src/xam/` owns XAM services
and their guest export registration. Export tests should invoke the production
registry with guest memory and actual ordinals.

### Graphics

The intended data flow is:

```text
Xenos PM4/register/shader frontend
    -> normalized Xenon graphics commands and resources
    -> API-neutral validation, dependency, EDRAM, and resolve planning
    -> Vulkan / D3D12 / future native host backend
```

`src/graphics/xenos/` owns PM4 command processing, register access, graphics
and resource IR, shader decoding/lowering, primitive conversion, texture
interpretation, EDRAM surfaces, and presentation descriptions.
`src/graphics/vulkan/` and `src/graphics/d3d12/` own native devices, command
queues, resources, pipelines, synchronization, and presentation. DXC is a
separate optional shader compiler target. A native Metal implementation does
not exist; it should be added only with a real host implementation.

`src/graphics/common/backend_core*.hpp` holds the command handling both native
backends share: EDRAM ownership, resolves, draw setup, shader variants,
pipelines, submission and presentation. Each backend's `Backend::Impl` derives
from `BackendCore<Impl, Api>` and supplies hooks for the steps whose native
calls differ, such as creating render targets, binding textures and recording
a draw. Where the two backends behave differently, the hooks preserve each
behaviour; `docs/development/RESTRUCTURE_FINDINGS.md` lists those differences.
`src/graphics/common/` also holds backend capability discovery.

### Recompiler and core session

`src/recomp/` separates the driver (`driver/`), module analysis (`analysis/`,
by discovery, control flow, value tracking, ownership and validation), code
and project generation (`compilation/`), caching, hints, intake and reporting.
`src/core/session/` splits `XenonSession` by lifecycle, exports, guest
threading, execution, the GPU and audio pumps, and diagnostics, around
`session_internal.hpp`. Cache keys and generated output keep deterministic
regression coverage (`xenon_recomp_driver_tests` and the compilation-graph
tests).

## Optional and platform targets

D3D12 and XInput compile on Windows. Vulkan and DXC require their native SDKs.
Audio requires SDL2 and XMA-capable FFmpeg. SDL2/SDL3 input support follows
available SDKs. The launcher requires Qt 6.6+ and its GUI dependencies; the
runtime host uses SDL2 for presentation. Configuration can omit optional
targets, so a passing build of one configuration does not establish coverage
of another platform or feature.

## Build accountability

Before configuring, run:

```sh
python3 tools/development/build_accountability.py prepare --build-dir build/linux-x64-debug
cmake --preset linux-x64-debug
```

After configuration, run:

```sh
python3 tools/development/build_accountability.py check --build-dir build/linux-x64-debug
```

The checker reads CMake's configured File API graph, finds production `.cpp`
owners, and checks each standalone C++ test against CTest. It reports files
excluded by the configured platform or unavailable dependency. Normal CI runs
the audit after Linux and Windows builds. A local audit covers only the options
and dependencies selected for that build directory.

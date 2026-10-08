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
- Source files need an owning CMake target. Standalone C++ tests need a target
  and CTest registration, except build generators and benchmarks.

## Runtime and tool targets

| Owner | Shared headers | Implementation | Main target |
| --- | --- | --- | --- |
| Logging | `include/xenon/logging/` | `src/logging/` | `xenon_logging` |
| Core session, dispatch, diagnostics | `include/xenon/core/` | `src/core/` | `xenon_core` |
| CPU decode, IR, optimization, AOT | `include/xenon/cpu/` | `src/cpu/` | `xenon_cpu` |
| Guest address space and host VM | `include/xenon/memory/` | `src/memory/` | `xenon_memory` |
| Host kernel mechanisms | `include/xenon/kernel/` | `src/kernel/` | `xenon_kernel` |
| Xbox exports, imports, XEX support | `include/xenon/xbox/` | `src/xbox/` | `xenon_xbox_kernel_io` |
| XAM services and exports | `include/xenon/xam/` | `src/xam/` | `xenon_core` |
| Filesystem and content | `include/xenon/filesystem/` | `src/filesystem/` | `xenon_filesystem` |
| Graphics frontend and common data | `include/xenon/gpu/` | `src/graphics/xenos/` | `xenon_graphics` |
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

`src/cpu/ppc/` decodes and lifts the guest ISA; `ir/` and `optimizer/` own
intermediate representation and transformations; `codegen/` emits native C++
ahead of time. The dynamic fallback is CPU-owned. CPU memory access goes through
`cpu::MemoryPort`; `src/memory/guest/` owns guest mapping, permissions, aliases,
reservations, physical backing, and coherency. `src/memory/mapping/` selects the
platform host-VM implementation. Neither CPU nor GPU should create a separate
copy of guest RAM.

### Kernel, Xbox, and XAM

`src/kernel/` owns host-side handles, threads, synchronization, timers, memory
integration, and I/O mechanisms. `src/xbox/` owns guest-facing Xbox exports,
import resolution, and XEX/module compatibility. `src/xam/` owns XAM services
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

`src/graphics/xenos/` currently owns PM4 command processing, register access,
graphics and resource IR, shader decoding/lowering, primitive conversion,
texture interpretation, EDRAM surfaces, and presentation descriptions.
`src/graphics/vulkan/` and `src/graphics/d3d12/` own native devices, command
queues, resources, pipelines, synchronization, and presentation. DXC is a
separate optional shader compiler target. A native Metal implementation does
not exist; it should be added only with a real host implementation.

The current Vulkan and D3D12 `Backend::consume()` functions still repeat large
amounts of Xenon-level command handling. Common semantics and planning should
move behind a narrow API-neutral boundary with equivalence tests before native
backend behavior changes. The current `Backend` interface and graphics/resource
IR are starting points, not completed proof of backend neutrality.

### Recompiler and core session

`src/recomp/driver.cpp` currently combines module analysis, candidate discovery,
control-flow work, and project generation. `src/core/session.cpp` combines title
loading, exports, guest threads, GPU runtime, execution, and reporting. Their
state ownership should remain obvious as member functions move into coherent
translation units. Cache keys and generated output need deterministic regression
coverage during that split.

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

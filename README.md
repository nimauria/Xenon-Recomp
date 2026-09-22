# Xenon Recomp

**Xenon Recomp** is an experimental, modular Xbox 360 static-recompilation platform and native runtime for modern systems.

The project is designed so multiple game recompilation projects can share one implementation of the Xbox 360 execution environment instead of rebuilding CPU, memory, graphics, filesystem, input, kernel services, content handling, and launcher infrastructure for every title.

Xenon is being developed as a **shared runtime plus multi-game launcher**, with individual games supplied through separate modules. The first intended real-title integration and validation target is **Project Gracemeria / Ace Combat 6**.

> [!IMPORTANT]
> Xenon is under active development and is not yet a finished end-user compatibility layer. Interfaces, module contracts, and runtime behaviour may change while real-title bring-up continues.

> [!NOTE]
> Xenon does not distribute Xbox 360 firmware, games, executables, title updates, DLC, encryption keys, or other proprietary content. Users and game-module projects must provide content from lawful sources.

---

## Project goals

Xenon aims to provide a reusable native execution stack for Xbox 360 recompilation projects:

```text
User-provided Xbox 360 content
            |
            v
       XEX Loader V2
            |
            v
  Static recompilation pipeline
            |
            v
      Game module / native extension
            |
            v
        XenonSession
            |
   +--------+---------+----------+----------+
   |        |         |          |          |
   v        v         v          v          v
  CPU     Memory   Filesystem   Kernel     XAM/Input
   |        |                                |
   +--------+---------------+----------------+
                            |
                            v
                     Xenos frontend
                            |
                    canonical graphics
                            |
                    +-------+-------+
                    |               |
                    v               v
                Vulkan 1.3      Direct3D 12
                    |               |
                    +-------+-------+
                            |
                            v
                       Host system
```

The central architectural rule is simple:

> **Game projects may depend on Xenon. Xenon must never depend on one specific game project.**

Title-specific symbols, patches, hooks, workarounds, artwork, manifests, compatibility metadata, and generated native code belong in the game module that owns them. Xbox 360 hardware and runtime behaviour belongs in Xenon.

---

## Current state

This README reflects the source tree in the repository snapshot dated **22 September 2026**.

| Area | Status | Current state |
| --- | --- | --- |
| CPU V2 | Advanced | PPC/VMX/VMX128 decoding, lifting, Xenon IR, optimisation, native C++ AOT generation, compiled-function lookup, and extensive validation infrastructure are present. Real-title execution validation remains ongoing. |
| Memory V2 | Advanced / production foundation | Unified 512 MiB physical RAM model, Xbox address-space aliases, page protections, MMIO, reservations, host-VM integration, DMA/GPU write notification, and CPU/GPU coherency infrastructure are implemented. |
| XEX Loader V2 | Implemented foundation | Parses XEX1/XEX2 metadata/security structures, handles encrypted/compressed images, maps PE sections into Memory V2, exposes imports/exports/TLS/relocation metadata, and contains title-update/delta support. Retail-title qualification remains ongoing. |
| Recompilation pipeline | Implemented foundation | `recomp-driver`, `ppc-disasm`, `ir-dump`, `import-scanner`, and `module-inspector` provide analysis and generated native build input for title modules. |
| Runtime session | Integrated | `XenonSession` coordinates memory, CPU, filesystem, kernel I/O, input, XEX loading, exports, native compiled registries, lifecycle, and execution state. |
| Runtime host | Integrated | `xenon_runtime_host` runs separately from the launcher and supervises one game session through an explicit file-based launch/status contract. |
| GPU V1 | Correctness foundation complete | Shared Xenos semantics, canonical EDRAM ownership, shader translation, texture/resource handling, resolves, captures, Vulkan, and D3D12 backends are implemented. Retail capture/replay and longer hardware qualification remain. |
| Filesystem | Advanced | Host paths, VFS, GDFX/STFS content sources, Xbox path semantics, kernel I/O integration, Memory V2 guest marshalling, and recompiled-title import dispatch are present. |
| Kernel V1 | Implemented foundation | Handles, objects, file I/O, threads, synchronization, timers, waits, time services, memory integration, modules, process state, and exception foundations are present. |
| XAM | Active | Offline user/profile, locale, content, notifications, achievements, storage/content services, and export infrastructure are present. Save/data and title-specific validation still need further integration. |
| Input V1 | Feature-complete architecture | SDL2/SDL3, Windows XInput, keyboard/mouse, multi-source routing, profiles, flight/HOTAS mapping, XAM guest ABI marshalling, diagnostics, and a native module API are present. |
| Content services | Implemented foundation | Content graph, title-update, DLC, save-manager, validation and mounting infrastructure are present and being connected through the runtime/launcher path. |
| Launcher | Advanced frontend/backend | Qt 6 Quick/QML multi-game launcher with library, modules, profiles, settings, diagnostics, update infrastructure, module catalogue/install/update services, input configuration, and runtime-session supervision. |
| Audio V1 | Implemented common path | Xbox render-driver/XMA context semantics, XMAFRAMES decode, bounded voice mixing/resampling, Memory V2 DMA/coherency, SDL2 host output, xboxkrnl audio exports, and regression tests are integrated. Windows x64 includes the vetted FFmpeg/XMA dependency; real-title playback qualification remains ongoing. |
| Networking | Planned | Build option exists, but the production subsystem is not yet implemented. |
| ARM64 | Planned | The architecture is kept host-neutral where practical, but current development and validation focus remains x86-64. |

There is currently no claim of general Xbox 360 compatibility or a completed playable-title release. The present goal is to close the shared runtime boundary, then use real games to expose correctness gaps without introducing title-specific hacks into Xenon itself.

---

## Static recompilation pipeline

Xenon is built around **native/static recompilation**, not a permanent guest instruction interpreter.

```text
Xbox 360 PPC / VMX128 code
        |
        v
     Decoder
        |
        v
   Xenon CPU IR
        |
        v
    Optimizer
        |
        v
 Native C++ AOT source
        |
        v
 Host C++ compiler
        |
        v
 Native game-module code
```

The current whole-game tooling includes:

```text
recomp-driver
ppc-disasm
ir-dump
import-scanner
module-inspector
```

The recompilation driver can analyse executable ranges, discover functions, build a function database, record unresolved control flow, apply module hints, generate native source shards, create a compiled-function registry, and emit CMake build input for a game module.

Module hints are deliberately narrow. They can supply game knowledge such as function boundaries, known symbols, ignored/data regions, hooks, and patches, but they do **not** replace Xbox CPU semantics.

See [`docs/recomp/RECOMPILATION_PIPELINE.md`](docs/recomp/RECOMPILATION_PIPELINE.md).

---

## XEX Loader V2

The current XEX pipeline is intended to turn a real Xbox 360 executable into the effective image needed by the static recompilation and runtime layers:

```text
XEX1 / XEX2 file
      |
      v
base + optional headers
      |
      v
security information / page descriptors
      |
      v
AES decryption when required
      |
      v
None / Basic / LZX / delta processing
      |
      v
effective PE image
      |
      v
sections / imports / exports / TLS / relocations
      |
      v
Memory V2 mapping with guest protections
```

The loader also exposes structured information to the runtime and recompilation tools rather than forcing game modules to reimplement XEX parsing.

See [`docs/xbox/XEX_LOADER_V2.md`](docs/xbox/XEX_LOADER_V2.md).

---

## CPU

The CPU subsystem currently contains:

- Xbox 360 PowerPC decoding and lifting.
- VMX and VMX128 handling.
- Architecture-neutral Xenon IR.
- IR validation and optimisation.
- Native C++ AOT generation.
- Executable-code cache and compiled-function lookup infrastructure.
- External-call/import integration seams.
- Big-endian guest memory semantics.
- Scalar, floating-point, vector, branch, control-flow, and memory instruction families.
- Test fixtures for generated/native execution paths.

The current production focus is **x86-64 first**. Direct x86-64 and future ARM64 backend directories exist as architectural boundaries, but the current native AOT path is C++-based.

See [`docs/cpu/CPU_V2_DESIGN.md`](docs/cpu/CPU_V2_DESIGN.md) and [`docs/cpu/VALIDATION.md`](docs/cpu/VALIDATION.md).

---

## Memory V2

`memory::AddressSpace` is the production guest-memory implementation. CPU-only `FlatMemory` remains a test/support fixture rather than a second production RAM model.

Implemented foundations include:

- 512 MiB unified Xbox 360 physical RAM.
- Xbox virtual and physical address regions.
- Virtual aliases and XEX image mappings.
- Host virtual-memory integration on Windows and POSIX systems.
- Fixed reserve/commit and aligned allocation.
- Guest page protection and structured memory faults.
- MMIO registration and routing.
- 8/16/32/64/128-bit guest accesses.
- Load-reserve/store-conditional tracking.
- Physical allocation and virtual mapping.
- CPU/GPU shared-memory coherency notifications.
- External DMA/GPU write notification.
- Physical write observers and dirty tracking.
- Executable/self-modifying-memory invalidation hooks.

A core rule is that the CPU, GPU, kernel, XEX loader, and future devices must agree on **one guest memory truth**.

See [`docs/memory/MEMORY_V2.md`](docs/memory/MEMORY_V2.md).

---

## Graphics / Xenos

Xenon separates guest GPU semantics from host graphics APIs:

```text
Xbox 360 command/state semantics
            |
            v
   shared Xenos frontend
            |
            v
canonical Xenon graphics representation
            |
      +-----+-----+
      |           |
      v           v
   Vulkan       D3D12
```

The native backends are not separate Xenos emulators. Shared code owns the guest-visible behaviour; host backends consume normalized state.

### Shared Xenos layer

Current work includes:

- PM4/register processing.
- Command-ring and indirect-buffer handling.
- Predication and draw normalization.
- Primitive expansion/processing.
- Shader loading, decoding, reflection, lowering, and caching.
- Fetch/resource descriptors and texture layout handling.
- Xenos tiling/endian conversion.
- 10 MiB EDRAM model.
- Canonical color/depth ownership tracking.
- MRT, raster, viewport, scissor, blend, depth/stencil, and color-mask state.
- D24S8/D24FS8 depth support.
- Resolve/copy paths into guest memory.
- Cross-backend canonical resource barriers.
- Capture/replay infrastructure.
- Backend-neutral GPU performance counters.

### Vulkan

The Vulkan backend targets Vulkan 1.3 and contains device/queue management, resources, guest-memory mirroring, textures, descriptors, pipelines, dynamic rendering, render/depth targets, submission, presentation foundations, and resolve/readback support.

### Direct3D 12

The D3D12 backend contains the corresponding Windows-native device, queue, resource, descriptor/root binding, pipeline, texture, target, draw, presentation, and resolve foundations.

### Shader compilation

Xenon owns a common shader path that lowers Xenos shader state into host shader source and uses **DXC** where available to produce DXIL and SPIR-V.

See [`docs/graphics/GPU_V1.md`](docs/graphics/GPU_V1.md) and the documents under [`docs/graphics/`](docs/graphics/).

---

## Filesystem and content

The filesystem stack is shared by game modules and higher Xbox-facing services.

Current layers include:

```text
Guest file call
    |
    v
Memory V2 validation / marshalling
    |
    v
Xbox I/O facade and import dispatch
    |
    v
Kernel file objects / handles
    |
    v
Xenon VFS
    |
    +--> host path device
    +--> GDFX content
    +--> STFS packages
    +--> read-only/null devices
```

The content layer includes probing/materialization, package sources, title metadata, title-update/DLC foundations, and launcher integration points.

See [`docs/filesystem/FILESYSTEM_V1.md`](docs/filesystem/FILESYSTEM_V1.md) and [`docs/modules/CONTENT_SERVICES.md`](docs/modules/CONTENT_SERVICES.md).

---

## Kernel and Xbox services

The runtime now contains a reusable kernel/service foundation rather than leaving each title to invent its own host glue.

### Kernel V1

Implemented foundations include:

- kernel object model and handle tables;
- file objects and I/O requests;
- threads and process state;
- events, semaphores, mutants, timers, and waits;
- time services;
- Memory V2 integration;
- module management;
- exception infrastructure;
- xboxkrnl-facing I/O bridge work.

### XAM

The XAM layer currently provides foundations for:

- offline users and XUIDs;
- locale/language state;
- storage/content management;
- notifications;
- achievements and per-title statistics;
- DLC/title-update/content graph services;
- integration with the unified export system.

The design favours useful offline/native behaviour and explicit unsupported states rather than silently pretending unimplemented dashboard or network functionality succeeded.

See [`docs/kernel/KERNEL_V1.md`](docs/kernel/KERNEL_V1.md), [`docs/xam/XAM_V1.md`](docs/xam/XAM_V1.md), and [`docs/modules/CONTENT_SERVICES.md`](docs/modules/CONTENT_SERVICES.md).

---

## Input V1

Input is designed around a host-neutral four-user Xbox controller model with optional platform backends.

Current features include:

- SDL2 and SDL3 controller support.
- Native Windows XInput support.
- Keyboard/mouse virtual-controller mapping.
- Stable device identities and hotplug routing.
- Per-user primary and additional input sources.
- State merging for multi-device configurations.
- Deadzone/calibration/response profiles.
- Rumble, capabilities, keystrokes, and power information.
- Flight/HOTAS mapping into normal Xbox controller semantics.
- Memory V2-backed XAM guest ABI marshalling.
- CPU external-call registration seam.
- Runtime diagnostics.
- Versioned native module Input API.
- Launcher-side device/profile/routing configuration.

Input intentionally contains no Ace Combat 6-specific actions; title-specific interpretation remains in the game module.

See [`docs/input/INPUT_V1.md`](docs/input/INPUT_V1.md) and [`docs/modules/INPUT_API_V1.md`](docs/modules/INPUT_API_V1.md).

---

## Runtime session and process separation

`xenon::core::XenonSession` is the coordination layer for one running title. It owns or coordinates the active memory, CPU, filesystem, kernel, input, export, XEX, and execution state.

The launcher itself does **not** execute guest code. It starts a dedicated `xenon_runtime_host` process for each game session.

```text
xenon_launcher
     |
     | launch-config.json
     v
xenon_runtime_host
     |
     v
XenonSession
     |
     +--> status.json
     +--> log.txt
     +<-- stop.signal
```

This keeps Qt out of the runtime dependency graph and allows a game process to remain isolated from launcher failures.

A game module supplies a native compiled-code extension exporting the Xenon compiled-registry binding entry point expected by the runtime host. The runtime then binds that registry into the CPU execution context and starts from the loaded XEX entry point.

Current runtime-session gaps include further renderer/window wiring, cooperative stop/pause checkpoints in generated code, TLS completion, broader export coverage, and multi-threaded real-title execution qualification.

See [`docs/runtime/RUNTIME_SESSION.md`](docs/runtime/RUNTIME_SESSION.md) and [`docs/runtime/RUNTIME_HOST.md`](docs/runtime/RUNTIME_HOST.md).

---

## Launcher

Xenon includes a standalone **Qt 6 Quick/QML** launcher intended to host multiple recompiled games and modules through one frontend.

Current launcher work includes:

- Home, Library, Modules, Profiles, Filesystem, and Settings surfaces.
- Local game/content import.
- Module discovery and management.
- GitHub-backed module catalogue provider.
- Module package installation and update infrastructure.
- Per-module update history and rollback metadata foundations.
- Module settings schemas.
- Game/DLC metadata and artwork slots.
- Profile creation, duplication, editing, activation, deletion, and storage.
- Per-profile paths and preferences.
- Input device/profile routing.
- Theme, accent, scaling, and appearance controls.
- Command palette/search.
- Notifications.
- Diagnostics and support-bundle generation.
- Recovery flows.
- Import/export infrastructure.
- Launcher self-update infrastructure.
- Runtime-session launch/status/stop supervision.

Production builds are intended to start empty: no commercial games, DLC, or title assets are bundled with Xenon.

See [`launcher/README.md`](launcher/README.md) and [`launcher/BACKEND.md`](launcher/BACKEND.md).

---

## Game modules

A game module is responsible for game-specific knowledge while consuming Xenon's shared APIs.

Typical module-owned data may include:

- supported title/version identifiers;
- symbols and function boundaries;
- static recompilation output;
- native replacement hooks;
- compatibility patches;
- title-update/DLC definitions;
- launcher metadata and artwork references;
- module settings;
- native runtime API requirements;
- unavoidable title-specific renderer or service adaptations.

Game modules should **not** duplicate Xbox 360 CPU semantics, guest memory, XEX parsing, Xenos command processing, or generic xboxkrnl/XAM behaviour.

The runtime currently supports a native-extension contract for binding generated compiled functions into an `ExecutionContext`.

---

## Building

### Requirements

Core requirements:

- **CMake 3.25+**
- **C++20** compiler
- **Ninja** for the supplied presets

Optional components:

- Vulkan SDK for developer/system-dependency builds (production presets provision pinned Vulkan headers/loader)
- Windows SDK for Direct3D 12
- DXC for developer/system-dependency builds (production presets provision pinned DXC)
- Qt 6 for the launcher
- SDL3 for the optional SDL3 input backend

Native Audio/Input dependencies are resolved through Xenon's managed dependency
layer. Development builds use `AUTO` mode by default: explicit or already-managed
dependencies are preferred, compatible system packages may be used, and missing
pinned dependencies can be provisioned automatically. Windows x64 `AUTO` builds
may also use the committed vetted FFmpeg/XMAFRAMES developer bundle under
`third_party/xenon-ffmpeg`.

Release presets use `MANAGED` mode. SDL2 is built static, while the pinned
Xenia-maintained FFmpeg fork is built shared and staged beside the runtime so a
published build does not depend on the user's local packages. Vulkan headers,
the Vulkan loader and DXC are pinned for the same production path. Normal
development builds do **not** enable installer packaging; `XENON_BUILD_INSTALLER`
remains `OFF` unless a release preset is selected. See
[`docs/development/DEPENDENCIES.md`](docs/development/DEPENDENCIES.md) and
[`docs/development/RELEASE_PACKAGING.md`](docs/development/RELEASE_PACKAGING.md).

### Supplied CMake presets

```text
linux-x64-debug
linux-x64-release
windows-x64-debug
windows-x64-release
```

Example:

```bash
cmake --preset windows-x64-debug
cmake --build build/windows-x64-debug
ctest --test-dir build/windows-x64-debug --output-on-failure
```

Linux example:

```bash
cmake --preset linux-x64-debug
cmake --build build/linux-x64-debug
ctest --test-dir build/linux-x64-debug --output-on-failure
```

### Future production build

The installer pipeline is present now for release engineering, but it is not part of ordinary development builds. When Xenon is ready for a public release, use the production presets:

```text
windows-x64-release -> full runtime + launcher -> NSIS installer + portable ZIP
linux-x64-release   -> full runtime + launcher -> Debian package + portable TGZ
```

Those presets force managed/pinned dependencies and enable the release verifier. End users receive the redistributable runtime libraries inside the installer/package; they are not expected to install Qt, SDL2, FFmpeg, DXC or the Vulkan SDK manually. A supported GPU driver remains a host requirement.

While Xenon remains under active development, tag-triggered publication is gated by the GitHub repository variable `XENON_PRODUCTION_RELEASES_ENABLED`. Leave it unset/false during development. Manual workflow runs can still validate installer generation. When Xenon is ready to ship, set it to `true` and a `vX.Y.Z` tag will use the same validated production pipeline.

### Major build options

The authoritative list is in the root [`CMakeLists.txt`](CMakeLists.txt). Current major options include:

```text
XENON_BUILD_TESTS
XENON_BUILD_BENCHMARKS
XENON_BUILD_LAUNCHER
XENON_BUILD_INSTALLER
XENON_ENABLE_MEMORY
XENON_MEMORY_DEFAULT_DIRECT_APERTURE
XENON_ENABLE_GRAPHICS
XENON_ENABLE_VULKAN
XENON_ENABLE_D3D12
XENON_ENABLE_DXC
XENON_ENABLE_AUDIO
XENON_DEPENDENCY_MODE
XENON_AUTO_BOOTSTRAP_DEPS
XENON_FETCH_MISSING_DEPS
XENON_MANAGED_DEPS_ROOT
XENON_SDL2_ROOT
XENON_AUDIO_FFMPEG_ROOT
XENON_PREFER_BUNDLED_FFMPEG
XENON_ENABLE_INPUT
XENON_INPUT_ENABLE_SDL2
XENON_INPUT_ENABLE_SDL3
XENON_INPUT_ENABLE_XINPUT
XENON_ENABLE_NETWORK
XENON_ENABLE_FILESYSTEM
XENON_ENABLE_KERNEL
```

### Launcher-only Windows build

A launcher-focused build can provide Qt through `CMAKE_PREFIX_PATH`:

```powershell
cmake -S . -B build\launcher-ui -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_PREFIX_PATH="C:\Qt\6.x.x\msvc2022_64" `
  -DXENON_BUILD_LAUNCHER=ON

cmake --build build\launcher-ui --target xenon_launcher
```

The launcher is intentionally isolated so Qt is not a dependency of CPU, memory, graphics, recompilation, or game-module interfaces.

---

## Repository layout

```text
Xenon-Recomp/
├─ cmake/                  Build/compiler/platform helpers
├─ docs/                   Architecture, subsystem and validation documentation
├─ examples/               Runtime/module integration examples
├─ include/xenon/          Public Xenon interfaces
│  ├─ core/
│  ├─ cpu/
│  ├─ filesystem/
│  ├─ gpu/
│  ├─ input/
│  ├─ kernel/
│  ├─ memory/
│  ├─ modules/
│  ├─ recomp/
│  ├─ xam/
│  └─ xbox/
├─ src/
│  ├─ core/                Runtime/session/export/call bridge
│  ├─ cpu/                 PPC frontend, IR, optimizer and AOT backend
│  ├─ filesystem/          VFS and content providers
│  ├─ graphics/            Xenos frontend + Vulkan/D3D12/DXC
│  ├─ input/               Host and guest input stack
│  ├─ kernel/              Kernel objects, I/O and execution services
│  ├─ memory/              Production guest address space
│  ├─ recomp/              Whole-game recompilation driver
│  ├─ xam/                 XAM/offline/content services
│  └─ xbox/                Xbox import/XEX infrastructure
├─ launcher/               Qt 6 multi-game launcher
├─ runtime_host/           Separate game/runtime process
├─ tests/                  CPU, memory, GPU, filesystem, input, XEX and runtime tests
├─ tools/                  Recompilation and inspection tools
├─ CMakeLists.txt
└─ CMakePresets.json
```

---

## Documentation

Useful starting points:

- [`docs/architecture/PROJECT_STRUCTURE.md`](docs/architecture/PROJECT_STRUCTURE.md) — dependency and ownership rules
- [`docs/recomp/RECOMPILATION_PIPELINE.md`](docs/recomp/RECOMPILATION_PIPELINE.md) — static recompilation pipeline
- [`docs/xbox/XEX_LOADER_V2.md`](docs/xbox/XEX_LOADER_V2.md) — retail XEX loading pipeline
- [`docs/runtime/RUNTIME_SESSION.md`](docs/runtime/RUNTIME_SESSION.md) — unified runtime lifecycle
- [`docs/runtime/RUNTIME_HOST.md`](docs/runtime/RUNTIME_HOST.md) — launcher/runtime process contract
- [`docs/development/DEPENDENCIES.md`](docs/development/DEPENDENCIES.md) — pinned native dependency/bootstrap and release packaging
- [`docs/cpu/CPU_V2_DESIGN.md`](docs/cpu/CPU_V2_DESIGN.md) — CPU architecture
- [`docs/memory/MEMORY_V2.md`](docs/memory/MEMORY_V2.md) — production memory architecture
- [`docs/graphics/GPU_V1.md`](docs/graphics/GPU_V1.md) — graphics completion/qualification state
- [`docs/filesystem/FILESYSTEM_V1.md`](docs/filesystem/FILESYSTEM_V1.md) — filesystem architecture
- [`docs/kernel/KERNEL_V1.md`](docs/kernel/KERNEL_V1.md) — kernel execution environment
- [`docs/xam/XAM_V1.md`](docs/xam/XAM_V1.md) — XAM services
- [`docs/modules/CONTENT_SERVICES.md`](docs/modules/CONTENT_SERVICES.md) — title update, DLC and save/content architecture
- [`docs/input/INPUT_V1.md`](docs/input/INPUT_V1.md) — input architecture
- [`launcher/README.md`](launcher/README.md) — launcher frontend/backend state
- [`docs/development/RESEARCH_PROVENANCE.md`](docs/development/RESEARCH_PROVENANCE.md) — research references and provenance

---

## Validation philosophy

Xenon uses subsystem tests and cross-layer validation rather than relying on a single "boots a game" milestone as proof of correctness.

The repository contains dedicated coverage for areas including:

- CPU decode/lift/native generation;
- CPU V2 control-flow and IR behaviour;
- Memory V2 mappings, faults, ordering, MMIO, host VM, coherency, and executable-code invalidation;
- XEX crypto, LZX and loader behaviour;
- recompilation-driver analysis;
- filesystem and Xbox guest I/O;
- input and XAM guest marshalling;
- Xenos textures, shaders, EDRAM, resolves, presentation, primitive processing, and resource barriers;
- Vulkan/D3D12 backend capability and ownership integration;
- cross-backend canonical EDRAM behaviour;
- runtime-session integration.

Native backend tests are conditional on the corresponding host SDK/runtime being available.

Real-title validation remains essential: portable tests catch regressions, but captured command streams and game execution are what ultimately reveal incorrect Xbox assumptions.

---

## Research and acknowledgements

Xenon has benefited heavily from public Xbox 360 emulation, recompilation, graphics, and hardware research. That work should be credited rather than presented as if Xenon discovered the platform in isolation.

Important public references used for behavioural research, architecture comparison, or implementation cross-checking include:

- **Xenia** (`xenia-project/xenia`) — Xbox 360 CPU, memory, kernel, XAM, Xenos, texture, EDRAM, shader and system-behaviour reference.
- **ReXGlue / rexglue-sdk** — Xbox 360 static-recompilation/runtime architecture reference.
- **AC6_recomp** (`sal063/AC6_recomp`) — Ace Combat 6 recompilation and Project Gracemeria bring-up reference.
- **UnleashedRecomp** (`hedge-dev/UnleashedRecomp`) — production-oriented Xbox 360 native recompilation and rendering reference.
- **XenosRecomp** (`hedge-dev/XenosRecomp` and related public work) — Xenos shader/recompilation research reference.
- Other public ReXGlue-based recompilation projects used for architectural comparison and compatibility research.
- IBM/PowerPC architecture documentation.
- AltiVec/VMX documentation and public VMX128 research.
- Public ATI/AMD R400/R500, AddrLib, Yamato/Adreno A2xx and related GPU research where applicable to Xenos.
- Khronos Vulkan specifications/documentation.
- Microsoft Direct3D 12, DXGI, Windows, and DXC documentation.

These projects remain the work of their respective authors and are governed by their own licenses. Researching or comparing behaviour does not transfer ownership to Xenon. Any third-party source code incorporated into Xenon must retain the attribution and licensing required by its original project.

More detail is recorded in [`docs/development/RESEARCH_PROVENANCE.md`](docs/development/RESEARCH_PROVENANCE.md).

---

## Development principles

- **Keep Xbox behaviour generic.** If behaviour belongs to Xenon/Xenos/Xbox runtime semantics, implement it in the shared layer.
- **Keep title knowledge in modules.** Never add hard-coded game checks to the generic runtime to get one title working.
- **Prefer one memory truth.** CPU, GPU, DMA, XEX loading, kernel services, and devices must agree on guest memory ownership and coherency.
- **Keep native backends native.** Vulkan and D3D12 consume canonical Xenon state rather than implementing divergent Xbox semantics.
- **Fail visibly.** Unsupported commands, formats, imports, or states should produce useful diagnostics instead of silently returning plausible but incorrect results.
- **Preserve layer boundaries.** Qt belongs to the launcher, game patches belong to modules, and host APIs should not leak upward into guest semantic layers without a defined abstraction.
- **Test shared behaviour.** A fix for one game should become a reusable semantic correction where possible.
- **Document research provenance.** External work that materially informs Xenon should be acknowledged.
- **Do not distribute proprietary Xbox content.** Xenon is infrastructure for user-supplied lawful content.

---

## Roadmap

Near-term priorities are focused on making the first real title exercise the complete stack rather than creating more disconnected subsystem prototypes:

1. Finish runtime-session integration of the real Vulkan/D3D12 renderer and presentation path.
2. Complete remaining XEX/title-update and retail-image qualification against real inputs.
3. Continue CPU V2 real-title control-flow, exception, threading, and generated-code validation.
4. Connect the completed filesystem/kernel/XAM/input services through the unified runtime export path required by real games.
5. Exercise the GPU capture/replay path with retail command streams and Project Gracemeria traces.
6. Complete module/native-extension packaging so generated game code, metadata, assets, DLC definitions, and runtime API requirements install cleanly through the launcher.
7. Bring Project Gracemeria through first execution, first frame, sustained rendering, input, saves/content, and then playable validation.
8. Generalize every genuine Xbox 360 behaviour discovered during that process instead of placing AC6-specific logic in Xenon.
9. Add additional game modules to prove the runtime is reusable.
10. Qualify Audio V1 against retail titles, then expand networking, ARM64, Linux packaging, and later platform support as the shared execution path stabilizes.

---

## Contributing

Xenon is still moving quickly, so changes should preserve subsystem ownership and avoid creating short-term game-specific dependencies in shared code.

Before making a large architectural change, review:

- [`docs/architecture/PROJECT_STRUCTURE.md`](docs/architecture/PROJECT_STRUCTURE.md)
- subsystem-specific documentation under [`docs/`](docs/)
- [`docs/development/RESEARCH_PROVENANCE.md`](docs/development/RESEARCH_PROVENANCE.md)

New behaviour should include focused tests where practical, and fixes based on third-party research should record the relevant provenance.

---

## Legal

Xenon Recomp is an independent open-source development project.

It is not affiliated with, endorsed by, sponsored by, or approved by Microsoft, Xbox, Bandai Namco Entertainment, Project Aces, or any other platform or game rights holder.

Xbox, Xbox 360, Direct3D, Xbox Live, game names, characters, artwork, and other marks/assets belong to their respective owners.

This repository does not include proprietary Xbox 360 firmware, operating-system files, game executables, copyrighted game data, title updates, DLC, encryption keys, or Microsoft-owned software.

Users are responsible for complying with the laws and licences that apply to software and content they use with Xenon.

---

## License

Xenon Recomp is released under the **MIT License**. See [`LICENSE`](LICENSE).
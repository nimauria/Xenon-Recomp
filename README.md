# Xenon Recomp

**Xenon Recomp** is an experimental modular static-recompilation platform for Xbox 360 software, built around a reusable native runtime and a multi-game desktop launcher.

The project is designed so individual game recompilation projects can share one implementation of Xbox 360 CPU behaviour, memory semantics, Xenos graphics translation, filesystem services, launcher infrastructure, module management, and host-platform support instead of rebuilding those foundations independently for every title.

The long-term user-facing model is:

```text
                     Xenon Launcher
                          |
          +---------------+---------------+
          |               |               |
          v               v               v
      Game Module     Game Module     Game Module
          |               |               |
          +---------------+---------------+
                          |
                          v
                     Xenon Runtime
                          |
       +------------------+------------------+
       |           |            |            |
       v           v            v            v
      CPU        Memory      Filesystem    Services
                    |
                    v
              Xenos Graphics
                    |
              +-----+-----+
              |           |
              v           v
           Vulkan       D3D12
              |           |
              +-----+-----+
                    |
                    v
               Host Platform
```

Xenon is **not a finished emulator or finished compatibility layer**. It is under active development, interfaces are still evolving, and real-title execution is not yet complete.

> [!IMPORTANT]
> Xenon does not distribute Xbox 360 firmware, executables, games, title updates, DLC, keys, or other proprietary content. Supported recompilation projects are expected to operate on content legally obtained and supplied by the user.

---

## Project direction

Xenon is built around **native/static recompilation**, rather than interpreting Xbox 360 PowerPC instructions at runtime.

The current CPU path is:

```text
Xbox 360 PPC / VMX128 machine code
        |
        v
   Xenon Decoder
        |
        v
     Xenon IR
        |
        v
    Optimizer
        |
        v
 Native C++ AOT
        |
        v
 Host C++ Compiler
        |
        v
 Native Host Code
```

The shared runtime is deliberately game-independent.

Game modules may depend on Xenon.

**Xenon must not depend on any individual game project.**

Title-specific hooks, patches, generated code, symbols, content definitions, artwork, compatibility workarounds, and title-specific services belong to the module/project that owns that title.

Project Gracemeria / Ace Combat 6 is the first intended real-game integration and validation target, but Ace Combat 6-specific behaviour must remain outside the generic Xenon runtime.

---

# Current development status

Status reflects the `dev` branch as of **18 September 2026**.

| Area | Status | Current state |
| --- | --- | --- |
| CPU | **Advanced stable foundation** | 455/455 canonical Xenon PPC/VMX/VMX128 opcode patterns decode, lift to Xenon IR and lower through the native AOT path. |
| Memory V2 | **Advanced development** | Phases 1–13 complete. Lock-free/atomic hot translation, physical ownership, reservation monitoring, range operations, controlled external writes and shared coherency are implemented. |
| Xenos frontend | **Advanced** | PM4 processing, register state, draw normalization, shaders, resources, textures, EDRAM, ownership and presentation contracts are implemented. |
| Vulkan | **Advanced native backend** | Vulkan 1.3 device/resources/pipelines, guest-memory mirror, textures, MRT/depth, rendering and presentation infrastructure exist. |
| Direct3D 12 | **Advanced native backend** | Native Windows device/resources/pipelines, guest-memory mirror, MRT/depth, rendering and presentation infrastructure exist. |
| EDRAM / resolves | **Advanced / incomplete** | Canonical EDRAM ownership and major colour/depth foundations exist; remaining fidelity and ownership paths are still being completed. |
| Filesystem | **V1 implemented** | Generic host-independent VFS, host-directory devices, mounts, aliases, sharing semantics, metadata, enumeration and filesystem tests are present. |
| Launcher UI | **Advanced frontend** | Qt 6 Quick launcher with Library, Modules, Profiles, Settings, themes, branding, diagnostics and responsive desktop UI. |
| Launcher Core | **Active implementation** | Persistent profiles/library/module state, settings, package staging, launch-contract assembly, filesystem integration, updates and runtime bridge are backend-owned. |
| Module ecosystem | **Initial infrastructure implemented** | Public module catalog, GitHub Releases-based discovery/update model, digest verification and catalog validation workflow exist. |
| Xbox kernel/XAM/XAPI | **Early / planned** | Kernel handles, guest API marshalling, Xbox exports and higher runtime services remain major future phases. |
| Audio / Input / Network | **Planned** | Module boundaries exist but runtime implementations remain future work. |
| ARM64 | **Planned** | Core architecture remains host-neutral; x86-64 remains the current execution target. |
| Linux | **Supported development target** | CPU/memory validation and Vulkan-oriented architecture are supported; broader launcher/runtime qualification remains ongoing. |
| Android | **Future target** | Intended through the host-neutral runtime and Vulkan path. |

---

# CPU

The Xenon CPU layer provides the static-recompilation frontend used by supported game projects.

Current architectural state includes:

- 32 × 64-bit GPRs;
- floating-point register state;
- 128 VMX128 vector registers;
- CR, XER, FPSCR and VSCR;
- LR, CTR, MSR, VRSAVE and PVR;
- guest time-base support;
- load-reserve/store-conditional state;
- integer and control-flow instructions;
- floating-point instructions;
- scalar and vector memory operations;
- VMX and VMX128 instruction families;
- big-endian Xbox memory semantics;
- explicit byte-reversed PPC operations;
- generated compile/execution validation;
- architecture-neutral IR.

All **455 canonical opcode patterns** in the current catalogue decode, lift and lower through the native AOT path.

The current production target is **x86-64 first**.

ARM64 remains an intended backend, but development is deliberately prioritising complete x86-64 game execution before expanding the host architecture matrix.

See:

[`docs/cpu/VALIDATION.md`](docs/cpu/VALIDATION.md)

---

# Memory V2

Memory V2 is the current production evolution of Xenon's Xbox 360 memory subsystem.

The design keeps Xbox-visible memory semantics owned by Xenon while replacing expensive V1 implementation mechanisms with native host-friendly equivalents.

```text
Xbox 360 memory semantics
          |
          v
 canonical Xenon model
          |
          v
efficient host representation
          |
    +-----+------+
    |            |
    v            v
 Windows       Linux
 x86-64        x86-64
                   |
                   v
              future ARM64
```

## Completed Memory V2 work

The first thirteen implementation phases are complete:

1. architecture and hot-path audit;
2. hot/cold access architecture;
3. compact atomic page translation;
4. removal of global normal-RAM access serialization;
5. host VM abstraction;
6. optional direct 4 GiB guest aperture;
7. coalescing physical allocator;
8. physical ownership and alias lifetime;
9. Xenon/PPC reservation monitor;
10. explicit PPC memory-ordering model;
11. block/range memory access;
12. dirty tracking and observer replacement;
13. safe controlled external/DMA writes.

Important improvements include:

- generated PPC code obtains a concrete `MemoryAccessContext`;
- normal RAM access avoids virtual dispatch and the global management lock;
- 1,048,576 guest pages use compact atomic hot translation entries;
- physical page retirement uses safe read-side quiescence;
- reservation state is physical rather than virtual;
- the old ~16 MiB reservation-generation table has been replaced by a much smaller monitor;
- Windows/POSIX host VM behaviour is separated from Xbox memory policy;
- controlled GPU/DMA writes automatically publish reservation, executable and coherency changes;
- synchronous physical-write observer work has been removed from normal scalar stores;
- range operations replace repeated byte/scalar loops where possible;
- Vulkan, D3D12 and texture tracking consume Xenon-owned coherency state.

## Current remaining Memory V2 phases

Work continues on:

- shared CPU/GPU coherency hardening;
- lazy/range GPU synchronization;
- complete memory-type semantics;
- MMIO slow-path refinement;
- richer memory faults and kernel exception translation;
- executable-page/native code-cache invalidation;
- expanded benchmarks;
- additional fuzzing and platform hardening.

The optional direct guest aperture is implemented but is **not currently the default**. Current Linux x86-64 measurements favour the compact translation path, so platforms should only opt into the aperture after qualification.

See:

- [`docs/MEMORY_V2.md`](docs/MEMORY_V2.md)
- [`docs/memory/BASELINE.md`](docs/memory/BASELINE.md)
- [`docs/memory/VALIDATION.md`](docs/memory/VALIDATION.md)

---

# Graphics / Xenos

Xenon separates Xbox 360/Xenos behaviour from native host rendering APIs.

```text
Xbox / Xenos state
       |
       v
Common Xenos frontend
       |
       v
Normalized Xenon graphics state
       |
   +---+---+
   |       |
   v       v
 Vulkan   D3D12
```

Vulkan and D3D12 do not maintain independent Xbox GPU semantics.

## Common Xenos layer

Current common functionality includes:

- PM4 packet processing;
- register-file state;
- circular command rings;
- indirect buffers;
- predication;
- draw normalization;
- graphics IR;
- DMA, immediate and auto-index draw sources;
- primitive topology conversion;
- primitive restart;
- shader loading and hashing;
- Xenos shader decoding;
- shader reflection;
- HLSL lowering;
- common shader/resource ABI;
- fetch constants;
- texture descriptors;
- texture format catalogue;
- tiled addressing and detiling;
- mip handling;
- endian conversion;
- EDRAM modelling;
- EDRAM ownership;
- colour/depth target planning;
- MRT state;
- D24S8 / D24FS8 / 20e4 foundations;
- raster/depth/stencil/blend state;
- resource barriers;
- resolve/copy infrastructure;
- presentation-frame preparation.

## DXC shader pipeline

A shared Xenon HLSL lowering path targets both native graphics APIs:

```text
Xenos shader
     |
     v
Xenon shader model
     |
     v
    HLSL
     |
     v
    DXC
   /   \
  v     v
DXIL   SPIR-V
 |       |
D3D12  Vulkan
```

Xenon owns its shader ABI, translation model and cache identity rather than inheriting title-specific bindings.

## Vulkan

`Xenon::GraphicsVulkan` currently includes:

- Vulkan 1.3 device and queue creation;
- synchronization and command submission;
- guest-memory mirroring;
- buffers and images;
- resource layouts;
- textures and samplers;
- SPIR-V pipelines;
- dynamic rendering;
- MRT;
- depth targets;
- indexed and non-indexed draws;
- target resolve/readback;
- presentation/swapchain infrastructure;
- resize and synchronization handling.

## Direct3D 12

`Xenon::GraphicsD3D12` provides equivalent Windows-native foundations including:

- device/queue/fence management;
- guest-memory mirroring;
- resources and transitions;
- descriptors/root resources;
- DXIL pipelines;
- textures and samplers;
- MRT;
- depth targets;
- native draw submission;
- resolve/readback;
- DXGI swapchain/presentation infrastructure.

## Presentation

A backend-neutral presentation contract now exists above both APIs.

The correctness-first GPU V1 presentation path can:

- consume a Xenos scanout texture;
- crop to the visible dimensions;
- convert supported source formats into RGBA8;
- scale to the host output;
- preserve aspect ratio with letterboxing;
- feed backend presentation swapchains.

Both Vulkan and D3D12 now contain native `PresentationSwapchain` implementations.

Remaining work is therefore no longer simply “implement a swapchain”; it is integration, hardware validation, frame scheduling/performance work and eventual real-title presentation.

## Remaining graphics work

Major work before real-game graphics can be considered mature includes:

- remaining EDRAM/depth ownership and conversion fidelity;
- additional resolve modes and destination formats;
- resolve clear behaviour;
- remaining primitive/copy/fill cases;
- improved per-frame upload/resource allocation;
- less immediate queue synchronization;
- full presentation validation on supported hardware;
- captured-command regression fixtures;
- Project Gracemeria first-frame testing;
- Project Gracemeria multi-frame/gameplay validation.

See [`docs/graphics/`](docs/graphics/) for the detailed architecture and validation documents.

---

# Filesystem

Filesystem V1 now provides the generic filesystem boundary required by future Xbox kernel/XAM APIs and game modules.

The filesystem is deliberately independent from CPU execution internals, graphics and guest-pointer marshalling.

Current VFS functionality includes:

- Xbox-style guest path normalization;
- case-insensitive path matching;
- host-independent device abstraction;
- device registration and removal;
- symbolic aliases such as `game:` and `d:`;
- bounded alias chaining;
- relative paths and working-directory support;
- longest-prefix mount routing;
- host-directory devices;
- case-insensitive host lookup;
- path sandboxing;
- `..` traversal protection;
- symlink/junction escape protection;
- read-only mounts;
- create/open dispositions;
- sequential and positional I/O;
- seek, resize and flush;
- file and directory metadata;
- directory creation;
- deterministic directory enumeration;
- wildcard filtering;
- rename and removal;
- disk-space queries;
- Xbox/NT-style open outcomes;
- explicit share flags;
- live-handle sharing conflict detection;
- `NullDevice`;
- mount and symbolic-link diagnostics.

Still deferred to higher runtime layers are:

- `NtCreateFile`, `NtReadFile`, `NtWriteFile` and other Xbox kernel exports;
- guest-pointer marshalling;
- kernel file objects;
- the global kernel handle table;
- overlapped/asynchronous I/O;
- Xbox `FILE_*_INFORMATION` structures;
- GDFX/ISO devices;
- STFS/container support;
- save/content policy;
- title-specific update and DLC discovery.

See:

[`docs/filesystem/FILESYSTEM_V1.md`](docs/filesystem/FILESYSTEM_V1.md)

---

# Xenon Launcher

Xenon is designed around a **single launcher capable of managing multiple independently developed recompiled-game modules**.

The launcher uses **Qt 6 Quick/QML**, but Qt is isolated entirely from the reusable Xenon runtime.

```text
QML UI
  |
  v
LauncherBridge
  |
  v
FrontendBackend
  |
  v
Launcher Core services
  |
  v
RuntimeBridge
  |
  v
Xenon Runtime
```

## Frontend

The current launcher contains mature frontend work for:

- Library;
- Modules;
- Profiles;
- Settings;
- game properties;
- module settings;
- module catalog;
- profile creation/editing;
- profile avatars;
- search;
- theme selection;
- accent colours;
- responsive layouts;
- accessibility/text scaling;
- diagnostics;
- custom window controls;
- Xenon branding;
- theme-specific backgrounds;
- reusable Xenon UI components;
- contextual right-click/action menus;
- notifications and confirmations.

Production builds start with an empty library.

Development/test builds may explicitly enable fictional fixtures for UI testing.

No commercial game content is bundled.

## Launcher Core and backend

The launcher is no longer just a QML prototype.

Authoritative application behaviour is owned by C++ backend/core services.

Current feature slices include:

- application state;
- branding;
- community/support integration;
- diagnostics;
- filesystem integration;
- import/export;
- launch configuration;
- library management;
- game properties;
- DLC projection;
- module management;
- module manifests;
- module settings;
- public module catalog;
- path management;
- profiles;
- runtime capability projection;
- settings;
- themes/appearance;
- launcher updates;
- module updates.

QML owns presentation and transient UI state.

It does **not** own persistence, filesystem mutation, module installation, update state, launch-contract creation or runtime capability decisions.

See:

- [`launcher/README.md`](launcher/README.md)
- [`launcher/BACKEND.md`](launcher/BACKEND.md)
- [`launcher/TESTING.md`](launcher/TESTING.md)

---

# Module ecosystem

A Xenon game is represented by an independently developed **module**, rather than game-specific logic being compiled into the generic runtime.

A module may provide:

- generated recompiled code;
- supported game IDs and versions;
- symbol maps;
- hooks and patches;
- manifest metadata;
- launcher artwork;
- module-specific settings;
- DLC definitions;
- game compatibility state;
- title-specific service adapters;
- genuinely unavoidable title-specific renderer patches.

## Public module catalog

The repository now contains:

```text
catalog/modules.json
```

This is a **discovery catalog**, not the runtime module manifest.

It tells Xenon Launcher where an open-source module is published and which package belongs to a supported host platform.

Runtime capabilities, game IDs, settings, DLC definitions and game-specific behaviour remain inside the installed module.

## GitHub Releases

Launcher and module updates are intentionally independent.

```text
Xenon Launcher
    |
    +--> Xenon-Recomp GitHub Releases
    |
    +--> Module Catalog
             |
             v
       Module repository
             |
             v
        GitHub Releases
```

This lets each module release on its own schedule.

Current module update infrastructure supports:

- semantic-version releases;
- stable/prerelease channels;
- platform-specific release assets;
- staged package installation;
- SHA-256 release-asset digest verification;
- independently versioned game modules.

The central catalog **never distributes commercial game data**.

See:

- [`catalog/README.md`](catalog/README.md)
- [`docs/modules/GITHUB_MODULE_RELEASES.md`](docs/modules/GITHUB_MODULE_RELEASES.md)

---

# Updates and release infrastructure

The repository now includes GitHub Actions workflows for:

- launcher release packaging;
- module-catalog validation.

Launcher updates come from Xenon-Recomp releases.

Game-module updates come from the GitHub repository associated with that module in the public catalog.

This separation is intentional and prevents the Xenon repository from becoming a distribution point for game-specific packages or commercial content.

---

# Game-specific behaviour

The generic Xenon runtime must not accumulate checks such as:

```cpp
if (game == ACE_COMBAT_6) {
    // title-specific behaviour
}
```

The correct rule is:

> If behaviour represents Xbox 360 hardware or runtime semantics, implement it in Xenon.  
> If behaviour exists only because of one game, implement it in that game's module.

This boundary is what allows Xenon to support multiple independently developed recompilation projects through one shared runtime and launcher.

---

# Xbox runtime services

The next major framework layers include Xbox-facing facilities such as:

- kernel objects;
- handles;
- threads;
- synchronization;
- events;
- timers;
- Xbox kernel exports;
- XAM;
- XAPI;
- profiles/sign-in;
- storage/content;
- saves;
- achievements;
- input;
- audio;
- networking;
- presence;
- friends;
- sessions and matchmaking.

Filesystem V1 now provides the host-independent VFS required by the later Xbox file APIs, but guest structure marshalling and kernel handle semantics intentionally remain above that layer.

Future online functionality should remain modular and should not require CPU, memory or graphics code to understand a specific online service.

---

# Host architectures and platforms

## CPU architectures

Current:

- x86-64

Planned:

- ARM64

## Host platforms

Current/planned:

- Windows
- Linux
- Android

Vulkan is intended to provide the common graphics path where practical.

Direct3D 12 is the native Windows alternative.

---

# Building

## Requirements

Core:

- CMake 3.25+
- C++20 compiler
- Ninja or another supported CMake generator

Graphics development:

- Vulkan SDK for Vulkan
- Windows SDK for D3D12
- DXC for shader compilation

Launcher:

- Qt 6.6+
- Qt Quick
- Qt Quick Controls 2
- Qt Quick Dialogs 2

## CMake presets

Current presets include:

```text
linux-x64-debug
linux-x64-release
windows-x64-debug
windows-x64-release
```

Example:

```sh
cmake --preset windows-x64-debug
cmake --build build/windows-x64-debug
ctest --test-dir build/windows-x64-debug --output-on-failure
```

## Launcher build

The launcher can be enabled with:

```text
-DXENON_BUILD_LAUNCHER=ON
```

For Windows launcher development, the provided helper can perform configuration, build, Qt deployment and execution:

```powershell
.\launcher\scripts\build-launcher.ps1 -TestMode -Clean -Deploy -Run
```

Omit `-TestMode` for normal production behaviour.

---

# Repository layout

```text
Xenon-Recomp/
├─ .github/
│  └─ workflows/           Release and catalog validation automation
│
├─ catalog/
│  ├─ modules.json         Public Xenon module discovery catalog
│  └─ README.md
│
├─ cmake/                  Build/platform helpers
├─ docs/                   Architecture and validation documentation
│
├─ include/xenon/
│  ├─ core/
│  ├─ cpu/
│  ├─ memory/
│  ├─ filesystem/
│  └─ gpu/
│
├─ src/
│  ├─ core/
│  ├─ cpu/
│  ├─ memory/
│  ├─ filesystem/
│  └─ graphics/
│     ├─ xenos/
│     ├─ dxc/
│     ├─ vulkan/
│     └─ d3d12/
│
├─ launcher/
│  ├─ qml/
│  ├─ resources/
│  ├─ scripts/
│  └─ src/
│     ├─ frontend_backend/
│     └─ services/
│
├─ tests/
│  ├─ cpu/
│  ├─ memory/
│  ├─ filesystem/
│  └─ graphics/
│
├─ tools/
├─ CMakeLists.txt
└─ CMakePresets.json
```

---

# Research, references and acknowledgements

Xenon benefits heavily from the Xbox 360 emulation, recompilation and hardware-research communities.

The project does not present this research as if it was discovered independently.

Public documentation and open-source projects are used as behavioural references, architecture comparisons and validation sources while Xenon maintains its own runtime architecture and implementation.

Important development references include:

- **Xenia** — Xbox 360 architecture, Xenon PPC/VMX128 behaviour, memory semantics, Xenos registers/packets, shader behaviour, texture formats, EDRAM and runtime research.
- **ReXGlue / rexglue-sdk** — practical static-recompilation runtime and VFS architecture reference.
- **UnleashedRecomp** — major reference for modern native Xbox 360 recompilation rendering and Vulkan/D3D12 architecture.
- **XenosRecomp** — shader decoding/recompilation and HLSL/DXC implementation reference.
- **AC6_recomp** — Ace Combat 6-specific bring-up and recompilation reference for Project Gracemeria.
- Other public ReXGlue/static recompilation projects used as comparative implementation references.

See:

[`docs/development/RESEARCH_PROVENANCE.md`](docs/development/RESEARCH_PROVENANCE.md)

Xenon's implementation is independently structured around its own CPU IR, memory subsystem, graphics abstraction, launcher architecture and module boundaries.

---

# Legal

Xenon Recomp is an independent open-source development project.

It is not affiliated with, endorsed by, or sponsored by Microsoft, Xbox Game Studios, Bandai Namco, Project Aces, Epic Games, or other game publishers/developers whose software may eventually be supported by independent modules.

Xbox, Xbox 360, Xbox Live and related names are trademarks of Microsoft Corporation.

No proprietary Xbox 360 firmware, executable, copyrighted game data, title updates, DLC, encryption keys or Microsoft-owned software are distributed with this repository.

Users and module developers are responsible for complying with applicable laws and licences when supplying game content to Xenon-compatible projects.

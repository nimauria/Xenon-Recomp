# Xenon Recomp

**Xenon Recomp** is an experimental, modular Xbox 360 static-recompilation runtime and native compatibility framework for modern hardware and operating systems.

The project is being built so that individual game recompilation projects can share one reusable implementation of the Xbox 360 CPU, memory model, Xenos-facing graphics layer, host graphics backends, runtime services, launcher infrastructure, and platform abstractions instead of rebuilding the same foundations for every title.

Xenon is **not a finished emulator or a finished compatibility layer**. It is under active development, interfaces are still changing, and game-level execution is not yet complete.

> [!IMPORTANT]
> Xenon does not distribute Xbox 360 firmware, executables, games, title updates, DLC, keys, or other proprietary content. Supported game projects are expected to work from content legally obtained and supplied by the user.

## Project direction

The core direction is native/static recompilation rather than a guest instruction interpreter:

```text
Xbox 360 title / recompiled game code
                |
                v
       Xenon CPU + Runtime
                |
        +-------+-------+
        |               |
        v               v
   Xenon Memory      Xbox Services
        |
        v
   Xenos Frontend
        |
        v
   Xenon Graphics IR
        |
   +----+--------------------+
   |                         |
   v                         v
Vulkan 1.3               Direct3D 12
   |                         |
   +------------+------------+
                |
                v
          Host platform
```

Game projects may depend on Xenon. **Xenon must not depend on any individual game project.** Title-specific hooks, patches, symbols, content definitions, artwork, workarounds, and game-specific renderer behavior belong in the game module/project that owns them.

Project Gracemeria / Ace Combat 6 is the first intended real-title integration and validation target, but AC6-specific behavior must remain outside the generic Xenon runtime.

---

## Current development status

Status reflects the source tree and validation work as of **18 September 2026**.

| Area | Status | Current state |
| --- | --- | --- |
| CPU | Advanced foundation | 455/455 canonical Xenon PPC/VMX/VMX128 opcode patterns decode, lift to Xenon IR, lower to the native C++ AOT path, and are covered by generated/native validation. |
| Memory | Production foundation | 512 MiB unified physical RAM model, Xbox virtual/physical aliases, protections, MMIO, reservations, coherency notifications, physical mappings, and CPU/GPU sharing are implemented. |
| Xenos frontend | Advanced | PM4/register processing, draw normalization, shader loading/decoding, resource state, primitive processing, EDRAM state, predication, and normalized graphics IR are implemented. |
| Shader translation | In progress / working foundation | Xenos shader state is translated through Xenon-owned HLSL generation and DXC to DXIL and Vulkan SPIR-V. |
| Vulkan | Active native backend | Vulkan 1.3 device/resource/pipeline, guest-memory mirror, textures, descriptors, dynamic rendering, MRT, depth, draw submission, and hardware validation are present. |
| Direct3D 12 | Active native backend | D3D12 device/resource/pipeline, guest-memory mirror, textures, root resources, MRT, depth, draw submission, and hardware validation are present. |
| EDRAM / resolves | Advanced but incomplete | Color ownership, native targets, MSAA mapping, raw bit-compatible color resolves, alias handling, and canonical EDRAM ownership are implemented; several fidelity paths remain. |
| Launcher | Frontend development | Optional Qt 6 Quick launcher UI is being developed as a generic multi-game/module frontend. Runtime-backed actions are not all connected yet. |
| Xbox runtime services | Early / planned | Filesystem, profiles, achievements, content, input, audio, networking, and higher Xbox API layers remain later phases. |
| ARM64 | Planned | The architecture is intended to remain host-neutral, but the current validation focus is x86-64. |
| Linux / Android | Planned / partial foundations | Vulkan is the intended cross-platform graphics path; desktop x86-64 bring-up remains the current priority. |

### Validation snapshot

The CPU baseline covers all **455 canonical opcode patterns** through decode, IR lifting, AOT source generation, compilation, and execution without adding a guest-opcode runtime interpreter fallback.

The current Windows graphics validation documented in the tree reaches **24/24 CTest targets in Visual C++ Release**, including hardware-backed Vulkan and Direct3D 12 validation. The corresponding Debug tree passes all configured tests; one recorded Debug tree contained 23 configured tests because DXC discovery was not enabled in that build directory.

Earlier Linux CPU/memory/frontend checkpoints were also validated with GCC, Clang, and sanitizer builds. See the detailed validation documents under `docs/` for the exact checkpoint, host, compiler, and test matrix rather than treating a single test count as a permanent project-wide number.

---

## CPU

The current CPU pipeline is:

```text
Xbox 360 PPC / VMX128 machine code
        -> Xenon decoder
        -> guest semantic frontend
        -> architecture-neutral Xenon IR
        -> optimizer
        -> native C++ AOT backend
        -> host C++ compiler
        -> native host executable code
```

Current CPU work includes:

- 32 64-bit GPRs.
- 32 floating-point register bit containers.
- 128 VMX128 vector registers.
- CR, XER, FPSCR, VSCR, LR, CTR, MSR, VRSAVE, PVR and time-base state.
- Integer, control-flow, floating-point, scalar memory, vector memory, VMX and VMX128 instruction families represented in the opcode catalogue.
- Load-reserve/store-conditional behavior.
- Big-endian Xbox memory semantics and explicit byte-reversed PPC operations.
- Generated compile/execution tests covering the canonical opcode corpus.
- A host-neutral IR boundary intended to keep ARM64 possible later.

The current production target is **x86-64 first**. ARM64 work is deliberately deferred until the first title path is running reliably.

Detailed CPU validation: [`docs/cpu/VALIDATION.md`](docs/cpu/VALIDATION.md)

---

## Memory

`memory::AddressSpace` is the production guest memory implementation. `FlatMemory` remains a CPU/test fixture.

Implemented memory foundations include:

- 512 MiB unified Xbox 360 physical RAM.
- Xbox virtual address-space regions and allocation page sizes.
- Physical RAM views and alias relationships.
- XEX dual-view aliasing.
- GPU/writeback physical view handling.
- Fixed reserve/commit and aligned allocation.
- 4 KiB, 64 KiB and large-page semantics where required by the guest map.
- Decommit, release, protection and region queries.
- Virtual-to-physical translation.
- Explicit physical allocation and virtual mapping.
- Read/write/execute protection enforcement.
- 8/16/32/64/128-bit CPU accesses.
- Reservation tracking by physical granule rather than virtual alias.
- External DMA/GPU write notification.
- Physical write observers.
- MMIO registration and overlay routing.
- Instruction-cache invalidation callback routing.

A core design rule is that the CPU and GPU do **not** maintain separate guest RAM implementations. Xenos-facing work consumes the same production physical backing used by recompiled CPU code.

Memory architecture: [`docs/memory/BASELINE.md`](docs/memory/BASELINE.md)  
Memory validation: [`docs/memory/VALIDATION.md`](docs/memory/VALIDATION.md)

---

## Graphics / Xenos

Xenon keeps Xbox/Xenos behavior in a common host-independent graphics layer and keeps Vulkan/D3D12 as native host backends.

The host backends consume normalized Xenon graphics state. They do not contain a second title-specific Xbox GPU command processor.

### Common Xenos layer

Current common graphics work includes:

- PM4 packet processing and register-file state.
- Circular command ring and indirect-buffer handling.
- Type-3 packet predication.
- Draw-state normalization and graphics IR.
- DMA/immediate/auto-index draw sources.
- Primitive topology processing and primitive-restart handling.
- Shader container loading, hashing, decoding and reflection.
- Host-neutral shader/resource ABI.
- Fetch constants, texture descriptors and constant banks.
- Texture formats, tiled addressing, mip handling and endian conversion.
- Shared guest-memory write/coherency handling.
- 10 MiB Xenos EDRAM model.
- EDRAM surface identity and circular tile ownership.
- Color/depth target planning.
- MRT slot preservation.
- Raster, depth/stencil, blend, color-mask, viewport and scissor state.
- D24FS8 / 20e4 depth support foundations.
- Backend-neutral resource barrier planning.
- Raw color resolve/copy paths back into Xbox tiled guest memory.

### Vulkan

`Xenon::GraphicsVulkan` currently targets Vulkan 1.3 and includes foundations for:

- device and queue creation;
- synchronization and submission;
- guest physical-memory mirroring;
- buffers and images;
- descriptor/resource layouts;
- texture realization and samplers;
- SPIR-V shader pipelines;
- dynamic rendering;
- multiple render targets;
- depth targets;
- native draw submission;
- render-target readback/resolve support.

### Direct3D 12

`Xenon::GraphicsD3D12` contains equivalent native foundations for Windows, including:

- device/queue/fence handling;
- guest physical-memory mirroring;
- native resources and state transitions;
- root-resource/descriptor handling;
- DXIL pipelines;
- textures and samplers;
- MRT/depth targets;
- native draw submission;
- render-target readback/resolve support.

### DXC shader path

Xenon uses a shared HLSL lowering boundary and DXC so the same normalized shader work can target:

- DXIL for Direct3D 12;
- SPIR-V for Vulkan 1.3.

The project intentionally owns its shader ABI, translation state, cache identity, and backend integration rather than tying the generic runtime to mappings from one specific game.

### Remaining graphics completion gates

Major work still required before a real title renderer can be considered complete includes:

1. Reversible native depth/stencil upload and readback for complete EDRAM depth ownership/alias fidelity.
2. Native resolve support beyond the completed raw/full-sample color path, including individual MSAA sample selection, converted color destinations, and depth destinations.
3. Resolve-region color/depth clear values through the EDRAM ownership system.
4. Remaining primitive expansion paths, including RectangleList and copy/fill cases.
5. Rotating frame contexts, per-frame upload/constant/descriptor allocation, and less immediate queue synchronization.
6. Vulkan and D3D12 swapchain creation, resize, synchronization, and presentation.
7. Captured-command regression fixtures followed by Project Gracemeria / Ace Combat 6 first-frame and multi-frame validation.

Graphics documentation:

- [`docs/graphics/NATIVE_BACKENDS.md`](docs/graphics/NATIVE_BACKENDS.md)
- [`docs/graphics/XENOS_DRAW_IR.md`](docs/graphics/XENOS_DRAW_IR.md)
- [`docs/graphics/XENOS_SHADER_IR.md`](docs/graphics/XENOS_SHADER_IR.md)
- [`docs/graphics/HLSL_DXC.md`](docs/graphics/HLSL_DXC.md)
- [`docs/graphics/NATIVE_RESOURCE_ABI.md`](docs/graphics/NATIVE_RESOURCE_ABI.md)
- [`docs/graphics/TEXTURES.md`](docs/graphics/TEXTURES.md)
- [`docs/graphics/EDRAM_RENDERING.md`](docs/graphics/EDRAM_RENDERING.md)
- [`docs/graphics/XENOS_DEPTH_20E4.md`](docs/graphics/XENOS_DEPTH_20E4.md)
- [`docs/graphics/VALIDATION.md`](docs/graphics/VALIDATION.md)

---

## Launcher

Xenon is also moving toward a **single generic launcher for multiple recompiled games/modules**, rather than requiring a separate launcher for every title.

The current frontend direction uses **Qt 6 Quick/QML** and is deliberately isolated from the runtime libraries so Qt does not leak into the CPU, memory, graphics, or game-module interfaces.

The launcher design includes:

- Library, Modules, Profiles, and Settings views.
- Generic module-provided game metadata and artwork slots.
- Local game-content/module/DLC import flows.
- Per-game information and compatibility state.
- Profile creation and selection.
- Theme support.
- Frontend feedback for actions whose runtime services are not connected yet.

A normal launcher build should start with **no games, modules, or DLC pre-populated**. Development builds can enable a code-level test mode that injects fictional data solely to exercise the UI.

The launcher is not intended to provide commercial game downloads. Game content, title updates, and DLC must come from the user's own local sources and be identified/validated by the appropriate game module.

---

## Module architecture

The long-term runtime is intended to remain split into reusable layers. Names may evolve, but the dependency direction is intentionally similar to:

```text
Game module / recompilation project
             |
             v
        Xenon Core
             |
   +---------+----------+
   |         |          |
   v         v          v
  CPU      Memory    Runtime services
             |
             v
          Graphics
             |
       +-----+-----+
       |           |
       v           v
    Vulkan       D3D12

Host platform services sit below the reusable runtime layers.
```

Planned or emerging areas include:

- `xenon-core`
- `xenon-cpu`
- `xenon-memory`
- `xenon-graphics`
- `xenon-graphics-vulkan`
- `xenon-graphics-d3d12`
- `xenon-kernel`
- `xenon-xbox`
- `xenon-filesystem`
- `xenon-audio`
- `xenon-input`
- `xenon-network`
- host platform modules
- launcher/module registry services

See [`docs/architecture/PROJECT_STRUCTURE.md`](docs/architecture/PROJECT_STRUCTURE.md).

---

## Game-specific behavior

The generic runtime must not grow title checks such as:

```cpp
if (game == ACE_COMBAT_6) {
    // game-specific behavior
}
```

A game project/module should instead provide its own:

- executable/version definitions;
- symbol maps;
- static recompilation output;
- hooks and patches;
- game-specific content metadata;
- module manifests;
- artwork/launcher metadata;
- compatibility overrides where genuinely unavoidable;
- title-specific networking/service adapters;
- title-specific renderer patches only when they cannot be generalized into correct Xbox 360 behavior.

If behavior is genuinely part of Xbox 360/Xenos hardware or runtime semantics, it belongs in Xenon. If it exists only for one title, it belongs in that title's project.

---

## Xbox-facing services

Higher runtime layers are planned to cover Xbox-facing facilities such as:

- profiles and sign-in state;
- storage and content mounting;
- saves;
- achievements;
- presence and friends;
- sessions and matchmaking;
- filesystem services;
- input;
- audio;
- networking abstractions.

Future online support should be modular rather than being tightly coupled to the original Xbox Live infrastructure. A replacement service can be explored later by game projects/runtime services without changing the CPU, memory, or native graphics architecture.

---

## Host architectures and platforms

### Host CPU

Current focus:

- x86-64

Planned:

- ARM64

The IR and module boundaries should avoid unnecessary assumptions about x86-64 so ARM64 does not require rewriting game-specific recompilation logic.

### Host platforms

Current/planned targets include:

- Windows
- Linux
- Android

Vulkan is intended to be the common graphics path where practical. Direct3D 12 is the native Windows alternative.

---

## Building

### Requirements

Core requirements:

- CMake 3.25+
- C++20 compiler
- Ninja or another supported CMake generator

Optional graphics/development requirements:

- Vulkan SDK for the Vulkan development target
- Windows SDK for Direct3D 12
- DXC for shader compilation to DXIL/SPIR-V

Optional launcher requirements:

- Qt 6.6+
- Qt Quick
- Qt Quick Controls 2
- Qt Quick Dialogs 2

### CMake presets

The repository currently provides presets for:

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

For a launcher-focused Windows build, Qt can be supplied through `CMAKE_PREFIX_PATH` and the launcher enabled with `XENON_BUILD_LAUNCHER=ON` in source trees containing the launcher frontend.

Example:

```powershell
cmake -S . -B build\launcher-ui -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_PREFIX_PATH="C:\Qt\6.x.x\msvc2022_64" `
  -DXENON_BUILD_LAUNCHER=ON `
  -DXENON_BUILD_TESTS=OFF

cmake --build build\launcher-ui --target xenon_launcher
```

Build options are intentionally modular. Check the root `CMakeLists.txt` for the authoritative set of options available in the current tree.

---

## Repository layout

```text
Xenon-Recomp/
├─ cmake/                  CMake helpers and platform/compiler configuration
├─ docs/                   Architecture, validation, graphics and research notes
├─ include/xenon/          Public/runtime interfaces
│  ├─ core/
│  ├─ cpu/
│  ├─ memory/
│  └─ gpu/
├─ src/
│  ├─ core/
│  ├─ cpu/
│  ├─ memory/
│  └─ graphics/
│     ├─ xenos/            Common Xbox 360 graphics behavior / normalized frontend
│     ├─ dxc/              Shader compiler boundary
│     ├─ vulkan/           Native Vulkan backend
│     └─ d3d12/            Native Direct3D 12 backend
├─ launcher/               Generic launcher frontend and launcher integration
├─ tests/                  CPU, memory, graphics and native-backend validation
├─ tools/                  Development/build tools
├─ CMakeLists.txt
└─ CMakePresets.json
```

---

## Research, references, and acknowledgements

Xenon has progressed much faster because the Xbox 360 emulation/recompilation community and hardware-research community have already published a large amount of valuable work.

**This project should not present that research as if it was discovered in isolation.** Public documentation and open-source projects have been used as research references, behavioral cross-checks, architecture comparisons, and bring-up references while Xenon develops its own runtime, IR, memory implementation, and native graphics architecture.

Important references used during development include:

- **Xenia** (`xenia-project/xenia`) — a major public reference for Xbox 360 architecture, Xenon PPC/VMX128 behavior, Xenos registers/packets, memory behavior, texture formats, EDRAM semantics, shader behavior, and correctness cross-checking.
- **ReXGlue / rexglue-sdk** — a useful Xbox 360 recompilation/runtime reference and source of practical lessons from existing recompilation projects.
- **AC6_recomp** (`sal063/AC6_recomp`) — used as a title-specific static-recompilation and Ace Combat 6 bring-up reference for the future Project Gracemeria integration.
- **UnleashedRecomp** (`hedge-dev/UnleashedRecomp`) — an important public example of a purpose-built native Xbox 360 recompilation renderer using modern Vulkan/D3D12 techniques.
- **XenosRecomp** (`hedge-dev/XenosRecomp` and related public work/forks) — consulted for Xenos shader decoding/recompilation approaches and the practical DXC/HLSL boundary.
- Other public **ReXGlue-based recompilation projects**, including Project8Recomp, The Simpsons Game Recompiled, reDAHM, GTA IV recompilation work, and related community projects, as compatibility and implementation references.
- IBM / PowerPC architectural documentation.
- The AltiVec Technology Programming Environments Manual.
- Public VMX128 reverse-engineering documentation.
- Public AMD/ATI R400/R500-era, AddrLib, Yamato/Adreno A2xx and related graphics research where relevant to Xenos behavior.
- Khronos Vulkan specifications and documentation.
- Microsoft Direct3D 12 / DXGI / DXC documentation.

These references are used to understand **guest-visible hardware/runtime behavior and proven translation techniques**. Xenon's design goal is to keep its generic implementation independent and reusable rather than reproducing another project's emulator architecture or importing title-specific assumptions into the runtime.

No acknowledgement above transfers ownership of those projects to Xenon. Their code, documentation, trademarks, licenses, and copyrights remain with their respective authors and rights holders. Where any third-party code is incorporated in the future rather than merely studied or compared, its original license and required attribution must be preserved.

More detailed research provenance is recorded in [`docs/development/RESEARCH_PROVENANCE.md`](docs/development/RESEARCH_PROVENANCE.md).

---

## Development principles

- **Generalize hardware behavior.** Fix Xenos/Xenon behavior in the shared layer when it is genuinely architectural.
- **Keep title-specific behavior outside Xenon.** Game projects should not leak into the generic runtime.
- **Prefer one memory truth.** CPU, GPU, DMA and future devices must agree on physical ownership and coherency.
- **Keep native backends native.** Vulkan and D3D12 should consume normalized Xenon state instead of each inventing different Xbox semantics.
- **Fail loudly on unsupported behavior.** An actionable error is preferable to silently rendering or executing incorrect state.
- **Validate continuously.** CPU, memory and graphics work should keep earlier regression suites passing.
- **Document research provenance.** External research that materially informs Xenon should be credited rather than obscured.
- **Do not distribute proprietary content.** Xenon is infrastructure, not a source for commercial Xbox 360 software.

---

## Roadmap

Near-term priorities are:

1. Finish the remaining native GPU/EDRAM resolve and depth ownership work.
2. Complete primitive expansion and fixed-function parity required for real title command streams.
3. Introduce mature frame contexts, resource lifetime management, and presentation/swapchains.
4. Continue strengthening CPU, memory, and CPU↔GPU coherency rather than allowing backend-specific semantics to diverge.
5. Capture and replay real command streams for regression testing.
6. Bring Project Gracemeria to first-frame, then multi-frame, then playable validation against Xenon.
7. Connect the launcher to real module discovery, local content validation, profiles, saves, and runtime lifecycle services.
8. Build higher Xbox kernel/runtime services needed by additional games.
9. Add further game modules without adding game-specific behavior to Xenon itself.
10. Expand host/platform support after the x86-64 desktop path is stable.

The project is intentionally being developed in layers: a working first title is important, but the framework should remain reusable enough that the next title does not require rebuilding the console from scratch again.

---

## Legal

Xenon Recomp is an independent open-source development project.

It is not affiliated with, endorsed by, sponsored by, or approved by Microsoft, Xbox, Bandai Namco, Project Aces, or any other game/platform rights holder.

Xbox, Xbox 360, Xbox Live, Direct3D, and related names and marks belong to their respective owners. Game names and assets belong to their respective owners.

This repository does not include proprietary Xbox 360 firmware, console operating-system files, game executables, copyrighted game data, title updates, DLC, encryption keys, or Microsoft-owned software.

Users are responsible for complying with the laws and licenses that apply to any software or content they use with Xenon.

---

## License

Xenon Recomp is released under the **MIT License**. See [`LICENSE`](LICENSE).

Third-party projects and research referenced by Xenon remain subject to their own licenses and copyright terms.

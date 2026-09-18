# Project Xenon module structure

This document defines where implementation work belongs so the runtime remains reusable
across multiple Xbox 360 recompilation projects.

## Dependency rule

```text
Game project
    |
    v
Xenon runtime
    |
    +-- graphics -> memory -> cpu
    +-- future kernel/services -> core hardware contracts
    |
    v
Host platform modules
```

A lower-level module must not depend on a game support project.

## CPU

Public API: `include/xenon/cpu/`

Implementation:

- `src/cpu/ppc/` — Xbox PPC/VMX128 architectural decoding and lifting.
- `src/cpu/ir/` — architecture-neutral Xenon CPU IR.
- `src/cpu/optimizer/` — IR transformations.
- `src/cpu/codegen/` — current portable native C++ AOT backend.
- `src/cpu/x86_64/` — future direct x86-64 machine-code backend.
- `src/cpu/arm64/` — future ARM64 backend.

`FlatMemory` is CPU-test support. Production translated loads/stores target
`cpu::MemoryPort`, implemented by the memory module.

## Memory

Public API: `include/xenon/memory/`

Implementation:

- `src/memory/guest/` — Xbox guest address-space semantics and unified physical RAM.
- `src/memory/mapping/` — future host virtual-memory providers / mapping policy.
- `src/memory/protection/` — future host page protection and fault integration.

Memory owns guest addresses, mappings, permissions, aliases, RAM, reservations and shared
CPU/GPU visibility. It does not own Xenos command processing, textures or EDRAM.

## Graphics

Public API: `include/xenon/gpu/`

Implementation:

- `src/graphics/xenos/` — Xenos-visible PM4, registers, shader microcode, EDRAM and graphics IR.
  Shared EDRAM surface addressing and primitive conversion also live here so
  Vulkan and D3D12 cannot develop divergent interpretations of guest draws.
- `include/xenon/gpu/resource_ir.hpp` — GPU 08 fetch/render-state decoding and
  the backend-neutral native resource ABI.
- `include/xenon/gpu/texture.hpp` — GPU 09 format catalogue, subresource layout,
  Xenos detiling and guest-memory dirty tracking.
- `src/graphics/dxc/` — optional HLSL-to-DXIL/SPIR-V compiler and shader cache.
- `src/graphics/vulkan/` — Vulkan host backend.
- `src/graphics/d3d12/` — optional Windows D3D12 backend.

Xenos uses the production memory module's physical backing. It must never create a second,
disconnected copy of Xbox system RAM.

## Kernel and Xbox services

`src/kernel/` owns generic kernel mechanisms such as threads, events, handles,
synchronization and timing.

`src/xbox/` owns Xbox-facing compatibility APIs, exports/imports, XAM/XAPI behavior and
module-level service glue.

Profiles, saves, achievements, content mounting and similar services belong here or in a
higher service module — not CPU, RAM or Xenos.

## Game-specific code

Game-specific knowledge stays outside Xenon, including:

- title/version checks
- symbol maps
- native replacement functions
- game patches
- game-specific render hooks
- content/version definitions
- networking adapters

Xenon owns mechanisms; game support packages own game knowledge.

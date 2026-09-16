# Xenon GPU Phase 1

This checkpoint adds the first production Xenos GPU layer on top of the frozen
CPU + memory baseline.

## Architecture

```
Xbox 360 CPU / D3D bare driver
           |
           v
  512 MiB shared physical RAM
           |
           v
     Xenos PM4 frontend
           |
           v
      Xenon Graphics IR
           |
           +---- shader programs / CF decode
           |
           +---- ordered register state
           |
           +---- shared-memory writes
           |
           v
       Host Backend API
           |
           +---- Vulkan (next)
           +---- D3D12 (later)

Render-target storage:
       Xenos RB
          |
          v
       10 MiB EDRAM
          |
       resolve/copy
          |
          v
  512 MiB shared physical RAM
```

## Implemented in this checkpoint

- Xenos PM4 packet header decoding for packet types 0, 1, 2 and 3.
- Circular primary command-ring execution with wraparound.
- Nested indirect buffers with recursion and submission safety limits.
- Big-endian command-buffer reads from the same physical RAM used by the CPU.
- Xenos register file with 0x5003 dword slots.
- Ordered register-write graphics IR.
- Type-0 sequential and one-register writes.
- Type-1 two-register writes.
- Type-2 no-op handling.
- Lossless type-3 packet retention for commands not yet lowered further.
- PM4 `MEM_WRITE`, including Xenos 32-bit endian modes and memory coherency
  notification back into the CPU memory system.
- `SET_CONSTANT`, `SET_CONSTANT2`, `SET_SHADER_CONSTANTS` and
  `LOAD_CONSTANT_CONTEXT` register-state effects.
- Pointer and immediate shader loads.
- Stable shader identity hashing.
- 96-bit shader grouping.
- Unpacking of two 48-bit control-flow instructions from each 96-bit group.
- Control-flow opcode plus exec address/count/yield/sequence/addressing fields.
- Draw commands represented in ordered graphics IR with the register generation
  active at submission time.
- 10 MiB Xenos EDRAM backing: 2048 tiles * 5120 bytes with circular addressing.
- Host backend interface that consumes graphics IR rather than PM4.
- `GraphicsSystem` tying command processor, register file, graphics IR, shared
  physical RAM and EDRAM together.

## Shared memory rule

The GPU does not have a second copy of system RAM. PM4, indirect buffers,
shaders, vertex/index data, texture backing and resolve destinations use the
same `memory::AddressSpace` physical backing as the CPU. GPU physical writes use
`notify_external_write`, so CPU load-reserve/store-conditional reservations and
future cache observers see DMA activity.

## EDRAM rule

Xenos EDRAM is separate from the 512 MiB unified RAM. It is represented as the
architectural 10 MiB circular render-backend store. Resolve/copy operations will
bridge EDRAM to the unified physical backing in later GPU work.

## Deliberately not complete yet

This checkpoint is the GPU frontend/hardware-boundary milestone, not a complete
renderer. Still required:

- full Xenos ALU instruction decoding and semantics
- full vertex/texture fetch instruction decoding and semantics
- shader control-flow graph construction and validation
- architecture-neutral shader IR
- shader IR -> SPIR-V translation
- fetch constants / vertex layout decoding
- texture format, tiling, mip and endian handling
- draw initiator/index-buffer lowering
- raster/depth/stencil/blend state lowering
- EDRAM render-target layout, MSAA and resolves
- memory-export support
- synchronization/query/event details
- Vulkan pipeline/resource caches
- presentation/swap integration
- AC6 frame-level validation

ARM64 and D3D12 remain deferred, consistent with the current x86-64 first path.

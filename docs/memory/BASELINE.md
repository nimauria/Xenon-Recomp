# Xenon Memory Baseline

This document freezes the first production memory subsystem for Project Xenon and the
contract between the CPU recompiler and the rest of the Xbox 360 runtime.

## Architecture

```
PPC / VMX128 native-recompiled code
              |
              v
        cpu::MemoryPort
              |
              v
       memory::AddressSpace
          /         \
         v           v
 guest virtual    512 MiB physical RAM
 mappings             |
                      +-- CPU physical aliases
                      +-- future Xenos GPU
                      +-- future XMA/APU DMA
```

`FlatMemory` remains a CPU test fixture. `memory::AddressSpace` is the production
`MemoryPort` implementation.

## Guest address-space layout

The baseline models the public Xbox 360 memory ranges used by retail software:

| Guest range | Size | Allocation page | Xenon model |
| --- | ---: | ---: | --- |
| `00000000-3FFFFFFF` | 1 GiB | 4 KiB | normal virtual |
| `40000000-7EFFFFFF` | 1008 MiB | 64 KiB | large-page virtual |
| `7F000000-7FFFFFFF` | 16 MiB | 64 KiB | GPU writeback/XPS physical view |
| `80000000-8FFFFFFF` | 256 MiB | 64 KiB | XEX view |
| `90000000-9FFFFFFF` | 256 MiB | 4 KiB | alias of the XEX view |
| `A0000000-BFFFFFFF` | 512 MiB | 64 KiB | physical RAM view |
| `C0000000-DFFFFFFF` | 512 MiB | 16 MiB | physical RAM view |
| `E0000000-FFCFFFFF` | 509 MiB | 4 KiB | physical RAM view with 4 KiB offset |
| `FFD00000-FFFFFFFF` | 3 MiB | device-defined | MMIO/device space |

The XEX `0x800...` and `0x900...` windows share the same physical backing in Xenon.
The `0x7F...`, `0xA...`, `0xC...`, and `0xE...` views resolve to the same unified
physical RAM where their ranges overlap.

## Physical RAM

- 512 MiB unified physical RAM.
- Linux uses an anonymous sparse `mmap` (`MAP_NORESERVE` when available), so reserving the
  console's full physical RAM does not eagerly consume 512 MiB of host memory.
- Physical frames are allocated at 4 KiB granularity internally.
- Virtual 64 KiB and 16 MiB allocation semantics are preserved independently of the
  internal physical frame granularity.
- The first 16 MiB physical window is kept for the GPU writeback/XPS view.

## Virtual memory

Implemented operations:

- fixed reserve
- fixed commit
- allocate with alignment and top-down/bottom-up policy
- 4 KiB and 64 KiB virtual heaps
- decommit
- release
- protection changes
- region/allocation queries
- virtual-to-physical translation
- explicit virtual mappings of physical allocations
- XEX dual-view aliasing
- zero/fill/copy helpers

Protections are enforced on CPU virtual accesses (`Read`, `Write`, `Execute`).
`fetch32_be` provides an execute-protected instruction-fetch primitive for code loaders or
future dynamic recompilation paths.

## CPU integration

All CPU memory operations continue to target `cpu::MemoryPort`; no CPU instruction knows
about the concrete RAM implementation. `AddressSpace` implements that contract for:

- 8/16/32/64/128-bit accesses
- normal big-endian Xbox accesses
- explicit byte-reversed little-endian PPC accesses
- load-reserve/store-conditional
- memory barriers
- `dcbz`/`dcbz128`
- `icbi`

Normal same-page scalar/vector accesses use one translation/protection check and a host
`memcpy` + endian conversion. Cross-page operations retain precise bytewise behavior.

## Reservations and coherency

Reservations are keyed by physical RAM, not by virtual address. Xenon currently uses a
128-byte reservation/coherency granule, matching the Xenon cache-line width used as the
implementation model.

A reservation token contains both the physical granule and its write generation. Writes
through any virtual/physical alias invalidate a reservation on the same granule. GPU/APU
DMA writers using `physical_data()` must call `notify_external_write`, which performs the
same invalidation and shared-memory notification path.

## MMIO

Arbitrary MMIO ranges may be registered with width-aware read/write callbacks. MMIO may
also overlay otherwise normal guest virtual ranges (for example Xbox device register
windows). Unhandled top-of-address-space MMIO faults rather than silently touching RAM.

## GPU/APU handoff

The memory phase deliberately exposes only generic hardware-facing mechanisms:

- physical allocation/free
- direct physical backing pointer
- virtual-to-physical mapping
- CPU-visible physical aliases
- external DMA write notification
- physical-write observers

Xenos command processing, tiling, textures, eDRAM and GPU MMIO do **not** belong in this
module and are the next Project Xenon phase.

## Public research basis

The memory map and alias relationships were cross-checked against the public Xenia memory
implementation and public Xbox 360 architecture research. PowerPC reservation behavior was
cross-checked against the public PowerPC Virtual Environment Architecture. Xenon's source is
an independent implementation rather than a line-by-line port of those projects.

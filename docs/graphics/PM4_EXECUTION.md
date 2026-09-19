# Xenos PM4 execution semantics

This document records the command-processor operations that Xenon executes in
its backend-neutral Xenos frontend. Vulkan and D3D12 must not decode PM4 or
invent independent Xbox command semantics.

## Executed command-processor side effects

The frontend currently performs the architectural state or memory effects for:

- Type-0 sequential and one-register writes.
- Type-1 dual register writes.
- `REG_RMW`, including immediate/register AND and OR operands.
- `REG_TO_MEM`, including packet-selected Xenos endian conversion and the
  hardware repeat count for sequential register saves.
- `MEM_WRITE`, using controlled Memory V2 physical writes.
- `COND_WRITE`, with register or physical-memory compare sources and register
  or physical-memory destinations.
- `COND_EXEC`, including its two physical-memory conditions and exact skipping
  of the following command dword block when the condition fails.
- `WAIT_REG_MEM`, with all eight comparison functions and register or physical
  memory polling.
- `WAIT_REG_EQ` and `WAIT_REG_GTE` compact register waits.
- `SET_CONSTANT`, `SET_CONSTANT2`, `SET_SHADER_CONSTANTS`,
  `LOAD_CONSTANT_CONTEXT`, and `LOAD_ALU_CONSTANT`.
- Pointer and immediate shader loads.
- `SET_SHADER_BASES` partition decoding (instruction-store size code plus vertex
  and pixel instruction-store bases).
- Vertex/pixel `IM_STORE` shader instruction-store export, including the
  start/size shadow dword used by a later `IM_LOAD`.
- The verified vertex/pixel portions of `INVALIDATE_STATE` (`0x100` / `0x200`),
  which invalidate the corresponding active shader identity and stored program.
- Primary rings and nested indirect buffers.
- Bin mask/select/base state and Type-3 predication.
- `EVENT_WRITE` initiator state.
- `EVENT_WRITE_SHD` completion/fence writes, including the bit-31 swap/progress
  counter selector and Xenos destination endian modes.
- `EVENT_WRITE_EXT` conservative architectural extent writeback.
- `VIZ_QUERY` begin/end state with scoped draw visibility tracking and query
  status-register writeback.
- `INTERRUPT` dispatch through the six-Xenon-hardware-thread callback seam.
- Normalized draw packet side effects and implicit draw/DMA register writes.

Physical reads use Memory V2 snapshots. Physical writes use Memory V2 controlled
write APIs, so CPU reservations, executable generations, dirty tracking and
CPU/GPU coherency are not bypassed.

## Wait safety policy

Xenos wait packets are genuine hardware stalls. Xenon polls the requested
register or memory location and periodically yields the host thread so a
concurrent recompiled CPU/GPU producer can satisfy the condition. A bounded
host-side poll limit is retained as a corruption/capture safety guard rather
than allowing a malformed command stream to deadlock the process forever.

The packet's hardware poll interval is not treated as a host-time duration.
The runtime does not attempt to emulate command-processor clock timing.

## Explicit major mode

Primitive encodings `0x10` and above force the explicit-major-mode path even
when the `VGT_DRAW_INITIATOR` major-mode bits contain the implicit encoding.
This decision is made in the common Xenos frontend because those values overlap
2D primitive and tessellation/patch interpretations depending on surrounding
VGT state.

## Commands intentionally not guessed

The following command families are still retained losslessly in graphics IR
when Xenon does not yet have enough verified semantics to execute them fully:

- `SET_STATE` state-block fetch/DMA behavior; the opcode purpose is known, but
  the exact Xenos block descriptor format has not been established strongly
  enough to execute it.
- `MEM_WRITE_COUNTER` program-counter write semantics.
- `IM_STORE` selector 2/shared-instruction-store behavior, which is documented
  by related A2xx hardware but is not promoted to a Xenos shader stage without
  independent Xbox evidence.
- `REG_TO_MEM` 64-byte and accumulate modes.
- `INVALIDATE_STATE` mask bits other than the verified vertex/pixel instruction
  invalidation bits.
- `EVENT_WRITE` extended payload forms whose memory-write layout has not been
  independently established.
- `EVENT_WRITE_CFL` and `EVENT_WRITE_ZPD` completion semantics beyond their
  lossless packet representation.
- hardware-accurate occlusion sample counting; `VIZ_QUERY` currently records
  whether a valid draw occurred in the query scope rather than inventing a
  backend-independent sample count.

`WAIT_FOR_IDLE`, `WAIT_UNTIL_READ`, and `WAIT_INDIRECT_BUFFER_PFD_COMPLETE`
remain explicit synchronization IR. Their host-backend synchronization meaning
is intentionally separate from register/memory polling performed directly by
the PM4 frontend.

Unknown opcodes are preserved as raw Type-3 IR rather than silently discarded.
A title capture that reaches one of these paths can therefore identify the next
required semantic implementation without corrupting packet ordering.

## Research provenance

The execution rules above were cross-checked against current Xenia command-
processor behavior and public A2xx-era PM4 sources whose command packet family
shares the relevant Xenos opcode numbers. Where public sources disagree or do
not establish the Xbox-specific packet layout, Xenon preserves the command
rather than promoting a guessed implementation into the common runtime.

## State execution extension — 2026-09-19

The second PM4 execution pass closes state-save/restore behavior exposed by
Xenos-era command streams. `COND_EXEC` uses the four-dword address/address/ref/
count form and treats the first two addresses as dword-addressed physical
memory. `REG_TO_MEM` decodes the source register from bits 0..17 and the repeat
count from bits 18..29, with zero meaning one transfer.

Shader context save/restore now retains the active vertex/pixel `ShaderProgram`
so `IM_STORE` can export the exact instruction dwords and write the start/size
descriptor consumed by a subsequent `IM_LOAD`. The exported instruction words
and self-modified PM4 descriptor are written in Xbox command/shader big-endian
byte order through Memory V2 controlled physical-write APIs.

`SET_SHADER_BASES` is decoded as state only; Xenon does not invent instruction
store capacities from the encoded size field. `INVALIDATE_STATE` clears active
vertex/pixel shader identities only for mask bits whose meaning is independently
visible in Xenos-era context streams. Unknown state bits remain preserved in IR.

## Event/query and replay extension — 2026-09-19

The next frontend pass executes the Xenos event/query behavior that has a
well-supported packet layout. `EVENT_WRITE_SHD` writes either the packet's
literal value or Xenon's monotonically increasing present/progress counter when
initiator bit 31 selects the counter source. The physical destination retains
the address low-bit endian selector and all writes go through Memory V2 rather
than a raw physical pointer.

`EVENT_WRITE_EXT` writes a conservative full architectural screen extent in the
Xenos 8-in-16 form. This intentionally over-reports until native raster-extent
feedback is connected; it does not pretend that the frontend knows the exact
pixel footprint of a completed draw.

`VIZ_QUERY` tracks all 64 query IDs. Beginning a query clears the corresponding
visibility bit; a successfully decoded draw marks active query scopes; ending a
query publishes that conservative visible/not-visible result through the two
Xenos query-status registers. True backend sample counting remains a later
title-driven fidelity refinement.

`INTERRUPT` now exposes the six CPU-mask bits through a callback seam owned by
`GraphicsSystem`. The common GPU layer therefore models the command-processor
side of interrupt generation without importing kernel scheduling policy.

The frontend also supports deterministic in-process replay of a normalized
submission. A capture stores the initial/final register snapshots and exactly
the IR appended by one buffer or ring submission. Replay emits a complete
register preamble followed by that captured IR without consuming the live guest
command stream. This is deliberately not yet a standalone frame dump: referenced
vertex, texture, shader-resource and resolve bytes are read from the current
Memory V2 guest state during replay. Resource-complete persistent capture is a
separate final bring-up gate.

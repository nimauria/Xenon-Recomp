# Xenon GPU 05 — Xenos Draw IR

This phase moves Xenos draw execution out of opaque PM4 payloads and into
backend-independent Xenon graphics IR.

## Goal

Vulkan and D3D12 must not parse Xbox 360 PM4 packets. The Xenos frontend now
lowers `DRAW_INDX` / `DRAW_INDX_2` semantics into `ir::DrawPacket` before a host
backend sees the command.

The normalized draw contains:

- primitive topology
- index source (`DMA`, immediate, or auto-index)
- index count and 16/32-bit index format
- major mode and the derived explicit-major-mode state
- `not_eop`
- visibility-query condition for `DRAW_INDX`
- DMA index-buffer physical address, byte length and endian mode
- active vertex/pixel shader hashes and start slots
- packed immediate index dwords when present
- decoded binned-draw base/size, base offset, mask and select state
- register-generation identity for the completed draw state

The raw packet payload is retained only for diagnostics. A host renderer should
never need to re-decode a draw packet.

## Implicit Xenos register writes

Draw packets program `VGT_DRAW_INITIATOR`, and DMA-indexed draws also program
`VGT_DMA_BASE` and `VGT_DMA_SIZE`. Xenon emits these as ordinary graphics-IR
register writes before the normalized draw. This gives every backend the same
ordered register state without duplicating Xenos packet knowledge.

## Shader binding

`IM_LOAD` and `IM_LOAD_IMMEDIATE` now update the frontend's active vertex or
pixel shader. A draw references the active programs by stable `ShaderProgram`
hash plus start slot. The shader load itself remains an earlier IR command, so a
backend can translate/cache the shader once and bind it by identity at draw
time.

This follows the useful part of Xenia's Xenos research (active shader state and
hardware draw-field definitions) without adopting its virtual GPU execution
architecture.

## Binned and immediate draws

Immediate index payloads are preserved in normalized draw IR so the future
primitive-conversion stage can unpack them once for all host backends.

`DRAW_INDX_BIN` and `DRAW_INDX_2_BIN` are fully split into their common draw
state plus `BIN_BASE` / `BIN_SIZE` before source-specific index data. Xenon also
tracks `SET_BIN_BASE_OFFSET`, the 64-bit bin mask/select packets and their
low/high half-write variants, and snapshots those values into each binned draw.

The original hardware uses this state for bin-ID/visibility predication while
rendering tiled EDRAM passes. A native Vulkan/D3D12 renderer targeting a full
host render target does not need to reproduce that internal tiling mechanism:
it executes the normalized draw once. Keeping the decoded bin state in IR makes
the frontend lossless without turning the host backend into a virtual Xenos.

## Native renderer direction

The architecture is deliberately closer to native Xbox 360 recompilation
renderers such as Sonic Unleashed Recompiled than to an emulator GPU:

```
Xbox 360 game / recompiled CPU
        |
        v
Xenos command + API semantics
        |
        v
Xenon frontend lowering
        |
        v
Xenon graphics IR
       / \
      v   v
 Vulkan  D3D12
```

Xenia, ReXGlue and related projects remain research references for guest-visible
Xenos behaviour. Their runtime emulation-style GPU architecture is not a Xenon
design dependency.

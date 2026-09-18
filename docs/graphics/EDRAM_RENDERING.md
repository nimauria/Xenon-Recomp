# GPU 10: EDRAM and native rendering

GPU 10 is the final native-frame boundary. It is not complete merely because
host images can be allocated: completion requires a real draw to flow from
Xenos IR through a compiled pipeline, EDRAM ownership and resolve, and into a
presentable image on both supported APIs.

## Implemented foundation

- Shared 2048-tile, 10 MiB EDRAM surface geometry with circular addressing.
- 80x16-sample tiles, 1x/2x/4x MSAA expansion and 32/64bpp tile pitch.
- Xenos depth-tile 40-sample half swapping.
- Lossless per-sample access and raw rectangular resolve/readback.
- Native Vulkan and D3D12 color render-target allocation and clear paths.
- Exact native mappings are accepted; formats requiring conversion are
  rejected explicitly.
- Backend draw-state consumption realizes color surfaces by EDRAM base, pitch,
  height, MSAA and format.
- Shared DMA, immediate and auto-index decoding with endian conversion and
  point/line/triangle topology expansion. Xenos multi-primitive reset markers
  are split in the common layer before backend submission.
- Native Vulkan dynamic-rendering and D3D12 graphics-pipeline objects built
  directly from GPU 07 SPIR-V/DXIL output.
- Production shader and pipeline caches keyed by generic Xenos shader/state
  identity, with no game semantics or fixed vertex locations.
- Actual indexed and non-indexed draw recording, including converted 32-bit
  host index buffers, descriptor/root binding, viewport, scissor and color
  attachment transitions.
- Color write masks, independent Xenos blend controls and blend constants are
  reflected into host pipeline state. Dual polygon fill and front/back culling
  are normalized consistently across the two native APIs.
- Up to four simultaneous color attachments are submitted natively without
  compacting Xenos export slots. Sparse Vulkan MRTs use undefined/null slots;
  sparse D3D12 MRTs use zero-write dummy RTVs to preserve `SV_TargetN`.
- Native depth/stencil attachments are realized on both backends. `D24S8` maps
  directly; `D24FS8` draw-time depth is quantized to the Xenos 20e4 lattice via
  pixel-shader variants, including explicit PS `e61.x` depth exports. Native
  depth-only passes are accepted without requiring a dummy color attachment.
- The hardware validation test compiles shaders with DXC, draws a triangle on
  both APIs, reads the target back and verifies the rendered pixel.
- Copy-mode register state is decoded generically, including source/sample
  selection, clear flags, command, destination address/pitch/height, 128-bit
  endian mode, array/slice, format/number type, exponent bias and red/blue swap.
- The conventional three-float2 resolve rectangle is extracted from vf0 with
  pixel-center correction, window offset, scissor clipping, pitch clamping and
  8x8 resolve expansion.
- A backend-neutral barrier planner batches/coalesces transitions and represents
  aliasing and memory dependencies.
- The texture codec has a tested inverse path that writes linear native data
  back to tiled/endian Xbox guest memory.
- Raw color copy-mode draws now read a realized Vulkan/D3D12 render target,
  perform native full-sample MSAA resolve where requested, write the result to
  tiled/endian guest memory and notify guest-memory dirty observers.
- `RB_COPY_DEST_PITCH` and height are treated as tiled storage strides, not
  clipping bounds. Offset resolves may address beyond them, and array/slice
  destinations use the Xenos 3D tiled address function.
- A transactional 2048-entry ownership map identifies the authoritative owner
  of every physical EDRAM tile. Exact 1x color aliases transfer through the
  canonical 10 MiB bit store on both native APIs; first use loads canonical
  bits, and the native upload/readback path is hardware-tested. Raw resolves
  reacquire the requested source before readback, so an overlapping newer
  alias is materialized instead of returning stale cached pixels.
- The resolve raster boundary uses deterministic D3D signed 16.8 conversion
  with nearest-even ties, saturation and NaN handling; non-finite inputs pass
  through that conversion rather than invalidating the rectangle early.
- Color target cache identity is based on EDRAM base, pitch, MSAA, bit width
  and format, not scissor-derived height. A later taller use grows the native
  image after preserving its old authoritative bits in canonical EDRAM.
- Depth target identity now follows the same height-independent rule. Until
  reversible depth transfer exists, a taller reuse fails explicitly rather
  than creating a second host authority for the same EDRAM surface.
- Type-3 packet predication is enforced once in the common command processor
  from `BIN_SELECT & BIN_MASK`; failed packets are consumed but produce no
  register, memory, synchronization or draw side effects.
- A well-formed resolve clipped to an empty rectangle is a successful no-op.
  Malformed resolve geometry remains a reported error.
- Guest-to-host sample order is centralized in the common EDRAM API. Native
  2x maps guest samples 0/1 to standard host samples 1/0; a 4x fallback uses
  host samples 0/3; 1x and 4x are identity mappings. Every mapping is covered
  independently by portable tests.
- Texture-fetch result exponent adjustment is decoded separately from LOD
  bias, included in descriptor identity and applied after sampling with the
  equivalent of `ldexp(result, exp_adjust)` in generated HLSL.

## Required completion gates

1. Complete depth ownership/transfer fidelity. Native depth targets and
   draw-time 20e4 quantization are implemented, but full `[0, 2)` D24FS8 EDRAM
   transfer still needs reversible native depth+stencil readback/upload. Stable
   depth identity is connected, but growth and alias preservation remain
   explicit failures until that reversible path exists.
2. Extend native resolve execution from the completed raw bit-compatible/full-
   sample color path to individual MSAA sample selection using the common
   guest/host mapping, converted color and D24S8/D24FS8 depth destinations.
3. Implement resolve-region color/depth clear values through the ownership map,
   including wrapped and overlapping tile ranges.
4. Add dedicated RectangleList shader/geometry expansion (three guest vertices
   produce four host vertices), then copy/fill primitive expansion.
5. Replace immediate queue waits and mutable shared bindings with rotating
   frame contexts, per-frame constant/descriptor pages and batched submission.
6. Swapchain creation, resize, synchronization and presentation for Vulkan and
   D3D12.
7. Captured-command regression fixtures followed by Project Gracemeria/AC6
   first-frame and multi-frame validation.

The architecture and rendered-frame path reach 100% only when these gates are
closed without title-specific hardcoded locations or an emulated Xenos command
processor inside either host backend.

## Research basis

The tile dimensions, circular addressing, MSAA expansion, 64bpp pitch and
depth-half behavior are cross-checked against the public Xenia `xenos.h` and
register definitions. Xenia remains a read-only hardware-behavior reference;
Project Xenon keeps its host-independent IR and native renderer architecture.

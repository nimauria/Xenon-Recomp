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
  point/line/triangle topology expansion.
- Native Vulkan dynamic-rendering and D3D12 graphics-pipeline objects built
  directly from GPU 07 SPIR-V/DXIL output.
- Production shader and pipeline caches keyed by generic Xenos shader/state
  identity, with no game semantics or fixed vertex locations.
- Actual indexed and non-indexed draw recording, including converted 32-bit
  host index buffers, descriptor/root binding, viewport, scissor and color
  attachment transitions.
- Color write masks, independent Xenos blend controls and blend constants are
  reflected into host pipeline state.
- The hardware validation test compiles shaders with DXC, draws a triangle on
  both APIs, reads the target back and verifies the rendered pixel.

## Required completion gates

1. Finish applying reflected depth/stencil, primitive restart and dual polygon
   modes; viewport, scissor, culling, MSAA, blending and color masks are wired.
2. Multiple simultaneous color attachments, including sparse MRT slots.
3. Depth targets and the exact Xenos 24-bit integer/20e4 depth conversions.
4. EDRAM tile ownership transfers when overlapping targets reinterpret memory.
5. Copy-mode rectangle extraction, MSAA sample selection/averaging, conversion,
   endian and red/blue swap, exponent bias, resolve clearing and guest-memory
   dirty notification.
6. Rectangle-list and copy/fill primitive expansion.
7. Swapchain creation, resize, synchronization and presentation for Vulkan and
   D3D12.
8. Captured-command regression fixtures followed by Project Gracemeria/AC6
   first-frame and multi-frame validation.

The architecture and rendered-frame path reach 100% only when these gates are
closed without title-specific hardcoded locations or an emulated Xenos command
processor inside either host backend.

## Research basis

The tile dimensions, circular addressing, MSAA expansion, 64bpp pitch and
depth-half behavior are cross-checked against the public Xenia `xenos.h` and
register definitions. Xenia remains a read-only hardware-behavior reference;
Project Xenon keeps its host-independent IR and native renderer architecture.

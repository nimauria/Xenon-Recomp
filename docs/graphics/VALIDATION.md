# Xenon GPU Phase 1 Validation — 2026-09-16

## Result

The CPU + memory + GPU Phase 1 source tree passes all current tests in three
independent build modes:

- GCC Release, warnings as errors: 13/13
- Clang Release, warnings as errors: 13/13
- Clang Debug, AddressSanitizer + UndefinedBehaviorSanitizer: 13/13

## GPU frontend coverage

`xenon_gpu_frontend_tests` currently validates:

- packet type 0/1/2/3 header decoding
- type-0 sequential register writes
- type-0 one-register writes
- type-1 dual register writes
- circular primary ring wraparound
- nested indirect buffers
- constant register block mappings
- constant context loads from shared physical RAM
- PM4 memory writes and Xenos endian conversion
- CPU readback of a GPU physical-memory write
- draw packet ordering in graphics IR
- pointer-based shader load
- immediate shader load
- stable matching shader hash across equivalent load forms
- 96-bit shader groups and 48-bit control-flow unpacking
- EDRAM size, tile wrap and byte-address wrap
- backend command consumption and one-shot IR draining
- malformed/truncated command rejection

All pre-existing CPU and memory tests remain part of the same CTest run, so GPU
changes are checked for regressions against the 455-opcode CPU baseline and the
production memory implementation.

## GPU 05 draw-state lowering — 2026-09-17

Validated in this snapshot with both GCC 14 Debug and Clang 17 Release builds,
with the repository warning policy enabled. CTest passes 13/13 in both builds.

Additional frontend checks now cover:

- hardware-layout `VGT_DRAW_INITIATOR` decoding
- auto-index draw normalization
- DMA index address, size, format and endian normalization
- implicit `VGT_DRAW_INITIATOR`, `VGT_DMA_BASE` and `VGT_DMA_SIZE` writes
- visibility-query token preservation
- explicit-major-mode derivation
- immediate index payload preservation
- active vertex/pixel shader identity captured by each draw

### GPU 05 binned-draw completion

Additional frontend coverage validates `DRAW_INDX_BIN` DMA ordering, bin base
and size normalization, base-offset/mask/select state capture, DMA fields after
the bin pair, and `DRAW_INDX_2_BIN` immediate payload placement after the bin
metadata.

## GPU 06 shader decoding and reflection — 2026-09-17

The decoder fixture passes under Visual C++ 19.51 with `/W4`. The shader
container, decoder, GPU 05 command processor integration, and existing GPU
frontend fixture all compile and run natively on Windows.

GPU 06 checks cover mixed fetch/ALU exec scheduling, stable hash/start-slot
association, vertex and texture fetch resources, float constants, exports,
texture dimensions and offsets, predicates, loop constants, and diagnostic
handling of truncated 96-bit streams.

## Windows-native validation — 2026-09-17

After adding the Windows `VirtualAlloc` physical-memory backing and enabling
memory/graphics in the Windows presets, Visual C++ 19.51 Debug passes 14/14
CTest targets natively. This includes all CPU/codegen tests, production memory,
CPU-memory integration, shader decoding/reflection and the Xenos GPU frontend.

## Native backend foundation — 2026-09-17

Visual C++ 19.51 Debug passes 15/15 CTest targets. The additional native
backend validation discovers the installed Vulkan loader and API version, then
creates a D3D12 feature-level 12_0 hardware device, direct command queue,
fence-backed submission and mapped resources. It also uploads the full 512 MiB
Xbox physical-memory mirror, propagates a dirty-page update, copies the probe
byte back from the GPU and verifies its value on the CPU. The tested adapter is
reported by the fixture rather than encoded in project policy.

LunarG Vulkan SDK 1.4.357.0 is installed on the validation host. CMake discovers
and builds `Xenon::GraphicsVulkan`; the test creates a validation-enabled Vulkan
1.3 device on the NVIDIA GeForce RTX 3050 Ti, uploads the full 512 MiB mirror,
propagates a dirty-page update and verifies GPU readback. The equivalent D3D12
path runs on the same adapter. Both Visual C++ 19.51 Debug and Release pass
15/15 CTest targets, and the direct Vulkan run emits no validation errors or
warnings.

The CPU/RAM/GPU integration audit additionally validates discontiguous physical
page notifications for cross-page CPU stores and full mirror invalidation after
an address-space reset. Backend allocation/submission failures now preserve
actionable errors and Vulkan command buffers are released on every failure
path.

## GPU 07 HLSL/DXC translation — 2026-09-17

The Windows-native suite now includes `xenon_shader_translation_tests`. It
checks deterministic HLSL and translation hashing, vertex and pixel profiles,
DXC availability, a valid DXIL container signature, a valid SPIR-V magic word,
Vulkan 1.3 targeting, option-sensitive in-memory caching, cache hits/misses and
rejection of incomplete decoded shaders. The generated shader is compiled by
the same DXC API used by the Vulkan and D3D12 backend shader-load paths.
Visual C++ 19.51 Debug and Release both pass 16/16 CTest targets.

## GPU 08 native resource ABI — 2026-09-17

Visual C++ 19.51 Debug and Release both pass 17/17 CTest targets. The new
resource fixture validates bit-exact texture and vertex fetch decoding, render-
target/raster snapshots, stable pipeline hashes, and the packed vertex/pixel,
boolean, loop and fetch constant-buffer ABI.

The native backend fixture creates the Vulkan descriptor-set/pipeline layout
with validation enabled and the equivalent D3D12 root signature and shader-
visible heaps on the hardware adapter. Both bind the real 512 MiB guest-memory
mirror. GPU 07 shaders compile against the finalized ABI as DXIL and Vulkan 1.3
SPIR-V in both configurations.

## GPU 09 native textures — 2026-09-17

Visual C++ 19.51 Debug and Release both pass 18/18 CTest targets. The texture
fixture checks all 64 format records, BC block sizes, 2D and 3D tiled-address
uniqueness, endian conversion, linear decoding, mip-tail sharing and overlap-
based dirty invalidation.

The hardware-backed fixture uploads a decoded RGBA8 texture into a real Vulkan
optimal image and D3D12 default-heap texture, performs the required state
transitions, creates image views/SRVs and samplers, and binds both through the
GPU 08 ABI. DXC validation additionally compiles register-gradient,
unnormalized-coordinate and half-texel-offset sampling to both DXIL and Vulkan
1.3 SPIR-V.

## GPU 10 EDRAM/native-rendering foundation — 2026-09-17

The common fixtures validate EDRAM surface pitch and tile count for 1x, 2x and
4x MSAA, circular 32/64bpp sample addressing, depth-tile half swapping and raw
rectangle resolves with independent destination row pitch. Primitive tests
cover DMA, immediate and auto-index sources, endian conversion, and common
line/triangle topology expansion.

The hardware-backed fixture creates and clears real Vulkan and D3D12 color
targets. Draw-state consumption now recognizes `RB_MODECONTROL` and realizes
native targets from EDRAM base, surface pitch, scissor height, MSAA and color
format. Formats without an exact native representation remain explicit errors.

The GPU 10 pipeline checkpoint extends this into a verified rendered-pixel
path. The fixture compiles vertex and pixel shaders with DXC to SPIR-V and
DXIL, creates Vulkan dynamic-rendering and D3D12 graphics pipelines, records a
triangle draw, copies each target back and verifies the center pixel. The
production backends use the same pipeline classes and now cache compiled
shaders/pipelines and submit normalized indexed or non-indexed draws with the
native resource ABI, viewport, scissor, culling, MSAA, color masks, independent
blend controls and blend constants. Visual C++ 19.51 Debug and Release pass
20/20 CTest targets.

# GPU 07: HLSL and DXC translation boundary

GPU 07 converts the backend-neutral shader model produced by GPU 06 into HLSL
and compiles that source with Microsoft's DirectX Shader Compiler. The D3D12
backend requests DXIL and the Vulkan backend requests SPIR-V for Vulkan 1.3.
Both formats therefore share one Xenos semantic lowering path.

## Public API

- `HlslShaderLowerer` produces deterministic HLSL, a translation hash,
  reflection, entry point, profile and diagnostics.
- `DxcShaderCompiler` owns the DXC interfaces and returns an immutable
  `CompiledShader` containing either DXIL or SPIR-V.
- `ShaderCache` is thread-safe and keyed by translated source plus every option
  that changes the binary. It is intentionally independent of a disk cache.
- Vulkan and D3D12 consume `ir::ShaderLoad`, request their native binary format
  and retain the common diagnostics boundary. They never decode PM4 or Xenos
  microcode themselves.

## Generic ABI

The generated HLSL uses Xenos architectural indices rather than game semantics:

- independent 256-vector vertex and pixel float-constant banks, packed bool
  constants and 32 loop constants
- 64 temporary registers and 64 export slots
- 32 vertex-fetch descriptors
- 32 resources for each Xenos texture dimension and 32 samplers
- vertex interpolator exports 0–15, position export 62 and pixel color exports
  0–3

There are no hard-coded Sonic, Ace Combat, vertex-layout or material meanings.
GPU 08 fixes these bindings as the native Xenon resource ABI. Vulkan remaps
the HLSL register classes into non-overlapping binding ranges while D3D12 uses
the corresponding root signature and descriptor tables.

## Lowering behavior

The translation includes vector/scalar ALU dispatch, source modifiers and
swizzles, masked destinations, predicates, address registers, pixel kills,
vertex fetch conversion, texture dimensions, exports, conditional execution,
loops, calls, returns and jumps. Control flow is emitted as a bounded program-
counter state machine so arbitrary legal Xenos branches do not depend on HLSL
structured-control-flow reconstruction.

Unknown/reserved ALU operations, vertex formats that do not yet have an exact
conversion, texture state/query operations and memory exports are rejected
with diagnostics. GPU 09 adds native texture resources plus computed/register
LOD, explicit gradients, offsets and unnormalized coordinates. Unsupported
operations are never silently emitted
as a different operation. These are semantic expansion points for later trace
fixtures, not permission for a title-specific fallback.

## Discovery and deployment

CMake discovers `dxc/dxcapi.h`, `dxcompiler.lib` and `dxcompiler.dll` from the
installed Vulkan SDK. `XENON_ENABLE_DXC` may disable the optional compiler
target. Tests copy the runtime DLL next to their executable; applications that
link `Xenon::GraphicsDXC` must deploy the matching DXC runtime in the same way.

GPU 08 consumes the reflection and compiled blobs through this boundary. Later
texture realization and draw submission must not duplicate or bypass it.

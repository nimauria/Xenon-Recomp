# Native graphics backends

Xenon keeps PM4 parsing, Xenos registers, normalized draw IR, shader IR,
resource state, EDRAM semantics and guest physical memory in the common
graphics layer. Vulkan and Direct3D 12 consume that host-independent state as
native renderers; neither backend contains an Xbox GPU command processor.

## Runtime and development discovery

`discover_backend_capabilities` distinguishes an installed graphics runtime
from development files. On Windows it probes the Vulkan loader/API version and
independently checks hardware D3D12 feature-level 12_0 support. The Vulkan
target is built only when CMake finds the Vulkan SDK. End users need the loader
and a Vulkan-capable driver, while developers need the SDK headers/import
library. D3D12 uses the Windows SDK.

## Shared native-rendering contract

The common frontend now supplies both backends with the same normalized state:

- compiled Xenos vertex/pixel shaders through the GPU 07 HLSL/DXC path;
- the stable GPU 08 descriptor/root resource ABI and packed constant banks;
- the 512 MiB physical-memory mirror plus decoded native textures;
- normalized point/line/triangle primitive batches and host index buffers,
  including common-layer Xenos primitive-restart splitting;
- viewport, scissor, culling, dual polygon fill, MSAA, per-target color masks
  and independent blend state;
- up to four Xenos color exports without compacting sparse MRT slot numbers;
- native depth/stencil state, including depth-only passes;
- the GPU 10A D24FS8/20e4 fragment-depth shader variant when required.

Required resource failures are fail-fast. A draw is not submitted with a
missing texture, failed constant upload, failed depth target or failed active
color target merely to keep rendering; the backend reports the native error so
the unsupported state can be fixed instead of producing misleading pixels.

## Multiple render targets

`plan_color_targets` is backend-neutral and preserves Xenos export numbering.
If color slot 3 is enabled, the native attachment count is four even when one
of slots 0-2 is disabled. This keeps Xenos `eN` / generated `SV_TargetN`
semantics stable instead of renumbering exports.

Vulkan represents sparse slots with `VK_FORMAT_UNDEFINED` in pipeline-rendering
state and a null image view for the matching dynamic-rendering attachment. The
slot's blend state and write mask must be disabled/zero.

D3D12 has no equivalent undefined RTV entry in this path, so sparse holes are
backed by zero-write-mask dummy RTVs of the same extent/sample count. They
preserve `SV_TargetN` numbering but never receive observable guest color data.
This is a native-host implementation detail and is not exposed in Xenon IR.

## Vulkan

`Xenon::GraphicsVulkan` uses Vulkan 1.3 dynamic rendering, synchronization2,
timeline-semaphore submission, native buffer/image allocation, the guest-memory
mirror, texture views/samplers, descriptor sets, compiled SPIR-V pipelines,
color/depth attachment transitions and native draw commands. The production
path now binds all active MRT slots rather than only the first render target.

## Direct3D 12

`Xenon::GraphicsD3D12` selects a hardware feature-level 12_0 adapter and owns
the direct queue/list/fence path, committed resources, descriptor heaps/root
signature, guest-memory mirror, textures, DXIL pipeline-state objects and
native draw submission. `OMSetRenderTargets` now receives the complete Xenos
MRT slot array, including zero-write dummy entries for sparse layouts.

## Validation boundary

The backend hardware fixture contains a two-target native draw/readback check:
the same pixel shader writes different values to `SV_Target0` and
`SV_Target1`, and both host images are copied back and checked independently.
The common/source-side portion is validated in the portable Linux build. The
expanded fixture now also passes on Windows with real Vulkan and D3D12 devices
and generated SPIR-V/DXIL shaders.

## 2026-09-17 reference audit

A second read-only comparison with current Xenia and UnleashedRecomp found and
closed several fixed-function and cache-boundary gaps:

- D24S8/D24FS8 polygon offset is converted to the correct host units; D24FS8
  integer bias remains a multiple of eight and slope bias uses the Xenos
  1/16-subpixel scale.
- Alpha test and the Xenos 1x/2x/4x dithered alpha-to-mask thresholds are
  generated in HLSL with the researched quadrant and host sample-bit ordering.
  Both are skipped when color target 0 is not exported.
- Blend constants and independent front/back stencil masks/references are
  dynamic where the native API permits. D3D12 uses depth/stencil v2 pipeline
  state for independent masks.
- Pipeline keys exclude dynamic and inactive state, avoiding needless PSO
  creation without hiding real native state changes.
- Indexed draws use one mapped transient arena instead of one allocation/map
  per draw.
- A shared resource-barrier planner represents coalesced transitions, aliasing
  and memory dependencies without exposing native handles to Xenos IR.

The same audit confirms that real multi-draw batching still depends on per-draw
constant/descriptor lifetime. Converted/depth EDRAM resolve, full alias
preservation and presentation remain completion gates, not finished features.

## Raw resolve and ownership checkpoint — 2026-09-18

Both native backends execute bit-compatible raw color resolves into guest
memory. The path validates source/destination format identity, preserves exact
Xenos 2D/3D tiled destination addressing (including offset and array/slice
resolves), applies Endian128 in independent 16-byte groups, and publishes the
modified physical-memory range to texture/mirror observers.

The common EDRAM ownership tracker now makes ownership changes transactional at
physical-tile granularity, including circular ranges. Vulkan and D3D12 can
preserve and reinterpret overlapping 1x color surfaces by reading exact native
bits back to canonical EDRAM and uploading them to the new surface. First-use
surfaces also load canonical EDRAM, and native upload/readback round trips pass
on both APIs. Resolve-source lookup goes through the same acquisition path, so
a cached surface partially superseded by an alias is refreshed before its bits
are copied to guest memory. Color target identity is height-independent and a
taller later extent safely preserves, recreates and reloads the target rather
than creating unrelated scissor-keyed aliases. Nonzero MSAA or depth alias
transfers remain explicit errors rather than silent corruption until
per-sample/depth transfer is implemented.

## Predication, empty resolves and sample identity — 2026-09-18

Type-3 predication is resolved before commands enter backend IR, so Vulkan and
D3D12 cannot diverge on skipped register writes, synchronization packets,
memory writes or draws. Empty but valid resolve rectangles also terminate as a
successful no-op before format, depth, clear or sample-path rejection.

The common EDRAM layer owns guest-to-host sample identity. Native 2x reverses
the guest indices to match standard host sample positions; 2x emulated by 4x
uses host samples 0 and 3; 4x is spatially identical. Native per-sample
readback/upload is still deliberately rejected until both backends consume
this mapping for ownership transfer.

Depth caches now use height-independent EDRAM identity. Because exact packed
depth/stencil preservation is not yet available, growth is a visible backend
error instead of silently allocating an unrelated depth authority.

## GpuUnsupportedCounters catalog — capability/silent-fallback observability

`GpuUnsupportedCounters` (`include/xenon/gpu/backend.hpp`) is a plain counter
struct, one field per audited "unsupported GPU operation" category, plus
`total()`. It is queryable via `Backend::unsupported_counters()` (default `{}`
for a backend that does not override it, e.g. `NullBackend`) and is published
as `XenonSession::capability_report()`'s `"gpu"` section whenever a GPU
backend exists — the section is omitted entirely, not reported as zero, when
no backend exists, so "not measured" stays distinguishable from "measured and
clean". Both `d3d12::Backend` and `vulkan::Backend` implement
`unsupported_counters()` identically, incrementing the shared counters at the
exact point each category is detected:

- **`unknownPackets`** — a guard at the top of `consume()` counts and logs
  (category `"gpu"`, `Logger::Level::Warning`) any `ir::Command` variant other
  than `RegisterWrite`/`DrawPacket`/`ShaderLoad` instead of silently dropping
  it.
- **`unknownRegisters`** — `CommandProcessor::Statistics::unknown_register_writes`
  (`include/xenon/gpu/command_processor.hpp`) increments in
  `emit_register_write()` immediately before it throws for a register index
  outside `RegisterFile::kRegisterCount`; this lives on the PM4 frontend's own
  `CommandProcessor::Statistics`, not on `Backend::unsupported_counters()`.
- **`unsupportedFetchFormats` / `unsupportedShaderInstructions` /
  `unsupportedShaderFeatures`** — `LoweredShader` carries matching counts,
  incremented by `HlslShaderLowerer::lower()` at a reserved ALU opcode, an
  unmapped vertex-fetch format, a non-fetch texture opcode, an over-limit
  temporary-register count, or an incomplete decoded shader.
- **`unsupportedTextureFormats`** — `TextureImage::initialize()` sets an
  `unsupported_format()` signal exactly when `host_texture_format()` returns
  Unknown/Undefined; the texture-bind path in `consume()` increments the
  counter when set.
- **`unsupportedSamplerBehaviors`** — both backends' `address_mode()`
  increments a counter on the `default:` case where a guest clamp value
  outside Xenos's real 0-3 range previously mapped silently to
  `BORDER`/`CLAMP_TO_BORDER`.
- **`unhandledResolveModes`** — incremented alongside the existing distinct
  `error()` string for an unsupported Xenos copy command or unsupported MSAA
  sample-set selection during resolve.
- **`unexpectedOwnershipTransitions`** — incremented alongside the existing
  rejection when `acquire_color_ownership()`/`acquire_depth_ownership()`'s
  `EdramOwnershipTracker::commit()` call returns false for a stale ownership
  plan.
- **`fallbackShaderUses`** — incremented on every draw that actually uses the
  host-only RectangleList geometry-shader fallback, not only on that
  fallback's rare compile failure.
- **`unhandledDepthStencilPaths`** — tracked for completeness/future-proofing;
  Xenos depth/stencil format space is currently closed (`D24S8`/`D24FS8` only,
  both fully handled), so nothing increments this field today. This is an
  honest zero, not a wired-up placeholder.
- **`failedResourceBarriers`** — also currently always zero:
  `ResourceBarrierPlanner`'s `request()`/`alias()`/`memory_dependency()` only
  return `false` for genuine no-ops (null resource, `before == usage`, a
  duplicate dependency), never for a barrier that could not be satisfied, so
  there is no real failure condition to hook this counter into yet.

`GpuPerformanceCounters` (`submissions`, `draws`, `shaderCacheMisses`,
`resolveOperations`, `texture_cache_invalidations` — see
`docs/graphics/TEXTURES.md`) is also surfaced alongside
`GpuUnsupportedCounters` in the same `capability_report()` `"gpu"` section.

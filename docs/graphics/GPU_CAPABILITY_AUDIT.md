# GPU Capability / Silent Fallback Audit — AC6 Runtime Readiness pass, Part 7

This tracks the "every unsupported GPU operation must be observable, never
silently approximated" work from the AC6 Runtime Readiness / Platform
Fidelity pass. It deliberately does not claim more than has been verified by
a build and a passing test - see `docs/kernel/THREADING_V2.md` for the same
convention on the kernel side.

## What this pass added

`GpuUnsupportedCounters` (`include/xenon/gpu/backend.hpp`) is a plain
counter struct, one field per audited category, plus `total()`. It is
queryable at any point via `Backend::unsupported_counters()` (default `{}`
for any backend that does not override it, e.g. `NullBackend`) and is
published as `XenonSession::capability_report()`'s `"gpu"` section whenever a
GPU backend exists - omitted entirely, not reported as zero, when it does
not, so "not measured" stays distinguishable from "measured and clean".

**These counters must reflect reality.** Never suppressed, reclassified, or
downgraded to make a report read as `unsupportedOperationsTotal: 0` - a
correct, fully-supported run against a title Xenon genuinely handles end to
end should read all zeros because nothing unsupported actually happened, not
because the counting was tuned to hide something.

Both `d3d12::Backend` and `vulkan::Backend` implement `unsupported_counters()`
identically (their `consume()`/resource paths are structurally the same),
incrementing the shared counters at the point each category is actually
detected:

- **`unknownPackets`** - `consume()` previously matched only
  `RegisterWrite`/`DrawPacket`/`ShaderLoad` via sequential `if (get_if<...>)`
  checks; every other `ir::Command` variant
  (`PhysicalMemoryWrite`/`IndirectBuffer`/`ShaderPacket`/
  `SynchronizationPacket`/`EventPacket`/`StatePacket`/`Type3Packet`) fell
  through silently. A guard at the top of `consume()` now counts and logs
  (category `"gpu"`, `Logger::Level::Warning`) any command that is none of
  the three handled variants, then returns - this is the observability half
  of `ir::Type3Packet`'s own doc comment promise ("packets are never
  silently discarded merely because the host backend does not consume them
  yet"); actually lowering/consuming those variants is separate future work.
- **`unknownRegisters`** - `xenon::gpu::CommandProcessor::Statistics`
  (`include/xenon/gpu/command_processor.hpp`) gained
  `unknown_register_writes`, incremented in `emit_register_write()`
  immediately before it throws `std::out_of_range` for a register index
  outside `RegisterFile::kRegisterCount`. The throw itself is unchanged - a
  deliberate, pre-existing hard-fail for a corrupt/invalid command stream,
  not something this pass relaxed into a silent continue. This lives on
  `CommandProcessor::Statistics`, not `Backend::unsupported_counters()`:
  `CommandProcessor` (Xenos PM4 frontend) and `Backend` (D3D12/Vulkan) are
  separate layers with their own pre-existing telemetry surfaces; a future
  capability-report aggregation can pull from both without forcing a
  cross-layer dependency here.
- **`unsupportedFetchFormats` / `unsupportedShaderInstructions` /
  `unsupportedShaderFeatures`** - `HlslShaderLowerer::lower()`
  (`shader_translation.cpp`) already refused (returned early, never emitted
  HLSL) on a reserved ALU opcode, an unmapped vertex-fetch format, a
  non-fetch texture opcode, an over-limit temporary-register count, or an
  incomplete decoded shader - but only as free-text `diagnostics` strings.
  `LoweredShader` (`shader_translation.hpp`) now also carries
  `unsupported_instructions`/`unsupported_features`/`unsupported_fetch_formats`
  counts, incremented at the exact point each case is recognized, so a
  caller folds them into `GpuUnsupportedCounters` without string-matching
  `diagnostics`. Every `HlslShaderLowerer::lower()` call site in both
  backends folds these in. The primary `ShaderLoad` path
  (`consume()`'s `ir::ShaderLoad` branch) previously did not even check
  `lowered.complete` before attempting to compile an incomplete lowering -
  fixed to skip that doomed compile attempt and preserve the real lowering
  diagnostic instead of a generic "shader compilation failed".
- **`unsupportedTextureFormats`** - `TextureImage::initialize()` (both
  backends) gained a structured `unsupported_format()` signal, set exactly
  when `host_texture_format()` returns `Unknown`/`Undefined` - alongside the
  existing `error()` string, not replacing it. The `consume()` texture-bind
  path increments the counter when set.
- **`unsupportedSamplerBehaviors`** - both backends' `address_mode()`
  (D3D12: a lambda in `ResourceLayout::bind_texture()`; Vulkan: a free
  function in `texture.cpp`) silently mapped any guest clamp value outside
  Xenos's real 0-3 range to `BORDER`/`CLAMP_TO_BORDER` with zero
  observability. Both now increment a counter on that `default:` case -
  D3D12's lives on `ResourceLayout` (cumulative, since `bind_texture()` is
  the only caller), Vulkan's on `TextureImage` (per-instance, folded into
  `Backend::unsupported_counters()` regardless of whether the overall
  texture bind succeeded or failed).
- **`unhandledResolveModes`** - both backends already produced a distinct
  `error()` string for an unsupported Xenos copy command or an unsupported
  MSAA sample-set selection during resolve; both sites now also increment
  the counter.
- **`unexpectedOwnershipTransitions`** - both backends' EDRAM
  `acquire_color_ownership()`/`acquire_depth_ownership()` already rejected a
  stale ownership plan via `EdramOwnershipTracker::commit()` returning
  false; both rejection sites now also increment the counter.
- **`fallbackShaderUses`** - the host-only RectangleList geometry-shader
  fallback (used when Xenos primitive expansion needs host-side
  triangle-strip conversion) previously only surfaced a diagnostic on its
  rare *compile* failure. Every draw that actually *uses* the fallback path
  now increments the counter, regardless of whether that shader still
  needed compiling this run.

`GpuPerformanceCounters`'s `unsupported_counters()`-adjacent accessor is also
now surfaced in `capability_report()`'s `"gpu"` section (`submissions`,
`draws`, `shaderCacheMisses`, `resolveOperations`) so the section is useful
context, not just a bare unsupported-total.

Logging: `xenon::logging::Logger` (Phase 0) is now actually used from GPU
code (`consume()`'s unknown-packet guard, the primary shader-lowering
failure path) - previously zero call sites existed anywhere under
`src/graphics/**`.

## Known, tracked gaps (explicitly incomplete, not silently assumed done)

### `unhandledDepthStencilPaths` has no real trigger today

Xenos depth/stencil format space is closed - `DepthRenderTargetFormat` only
has two real values (`D24S8`, `D24FS8`), both fully handled. The counter
field exists for completeness/future-proofing (a title-visible depth format
this pass has not encountered) but nothing currently increments it. This is
accurate, not a placeholder pretending to be wired up.

### `failedResourceBarriers` is not modeled at all yet

`ResourceBarrierPlanner` (`xenos/resource_barrier.cpp`) has no failure
concept to hook a counter into - its `request()`/`alias()`/
`memory_dependency()` only return `false` for genuine no-ops (null resource,
`before == usage`, a duplicate dependency), never for an actual barrier that
could not be satisfied. Giving this category a real trigger requires first
deciding what "a resource barrier failed" even means for this planner - a
design question, not a missing counter - and was deliberately not guessed at
in this pass. The field exists in `GpuUnsupportedCounters` and always reads
zero until that design work happens; that is an honest zero, not a fabricated
one.

### Shader lowering diagnostics remain flattened to one `error()` string

`LoweredShader::diagnostics` can (rarely) contain more than one entry - e.g.
both a reserved vector *and* scalar ALU opcode in the same shader. Every
consumption site still keeps only `diagnostics.front()` in `impl_->error`
(pre-existing behavior, unchanged by this pass). The new structured counts
(`unsupported_instructions`/`features`/`fetch_formats`) are not affected by
this - they are incremented once per actual occurrence during lowering,
independent of how many diagnostic strings survive into the error string -
but a human reading only `error()` still sees just the first reason.

## Tests

- `tests/graphics/xenos/gpu_frontend.cpp`: `GpuUnsupportedCounters::total()`
  sums correctly; `NullBackend`'s inherited default `unsupported_counters()`
  reads a real, honest zero; an out-of-range register write increments
  `CommandProcessor::Statistics::unknown_register_writes` while still
  throwing.
- `tests/core/session_tests.cpp`: `capability_report()` publishes a real
  `"gpu"` section (all-zero counters for a fresh Null-backend session) when
  a GPU backend exists, and omits the section entirely when one does not.
- `tests/graphics/backends/backend_capability_tests.cpp`,
  `backend_ownership_integration_tests.cpp`, `cross_backend_canonical_tests.cpp`:
  unaffected pre-existing real-device D3D12/Vulkan integration tests,
  reconfirmed green against real hardware after this pass's changes (both
  backends needed a new `xenon_logging` link dependency - see below).

## Build note: new `xenon_logging` library

`src/logging/logger.cpp` was previously compiled directly into `xenon_core`.
GPU code now calls `xenon::logging::Logger::instance()`, but
`xenon_graphics`/`xenon_graphics_d3d12`/`xenon_graphics_vulkan` do not (and
should not) depend on all of `xenon_core`. `logger.cpp` was extracted into
its own small static library, `xenon_logging`, linked by both `xenon_core`
and `xenon_graphics` - avoiding both the missing-symbol link failure and a
potential ODR violation from two libraries each compiling their own copy of
`Logger::instance()`'s function-local static singleton.

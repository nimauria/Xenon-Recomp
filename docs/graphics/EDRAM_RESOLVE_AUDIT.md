# EDRAM / Resolve Correctness Audit — AC6 Runtime Readiness pass, Part 8

Part 8 asked for real correctness coverage (not merely observability - that
was Part 7) of EDRAM tile ownership, color/depth resolve, MSAA, resolve
rectangles, format conversion, resource aliasing, and CPU/GPU coherency,
with regression tests for seven specific scenarios. This records what was
already true, what was fixed, and what remains a real, tracked gap - see
`docs/graphics/GPU_CAPABILITY_AUDIT.md` for the identical convention.

## Audit result: six of seven scenarios were already solidly covered

Before this pass, `tests/graphics/backends/cross_backend_canonical.cpp` and
`backend_ownership_integration.cpp` already provided genuine, bit-exact
regression coverage (real pixel/word comparisons, not smoke checks) for:

1. 4x MSAA resolve (color and depth, both backends, cross-backend)
2. Depth resolve (bit-exact D24S8/D24FS8, both backends)
3. Partial rectangle resolve (untouched-region assertion in
   `backend_ownership_integration.cpp`)
4. Overlapping EDRAM ownership (color/depth alias, both backends
   independently and cross-backend ping-pong)
6. Render → resolve → CPU read (the best-covered scenario - practically
   every handoff test ends here)

Scenario 3's coverage does not sweep partial rectangles across MSAA X2/X4,
and scenario 1's fixed 16x16 full-region resolve rectangle means partial-rect
and MSAA-level variation aren't combined in one test - noted as a minor gap,
not a correctness risk (each dimension is independently exercised).

## What this pass fixed: 2x MSAA color fallback was under-tested

`DepthTargetImage::readback_native_sample()`/`host_msaa()` already existed
for backend validation (raw host-sample readback, bypassing the guest-to-host
sample remap) - explicitly documented as needed because Xenos X2 falls back
to a native 4x attachment (host samples 0/3 real, 1/2 padding) when the host
lacks native 2x MSAA support. `RenderTargetImage` (the color counterpart) had
no equivalent, and its existing 2x-fallback test
(`tests/graphics/backends/backend_capabilities.cpp`) only used
`readback_sample()` (the guest-mapped accessor), which cannot observe whether
padding samples 1/2 actually stayed untouched - it can only ever see samples
0/3 by construction of the remap. The depth test proved this correctly;
color did not.

Fixed by adding `RenderTargetImage::readback_native_sample()` and
`host_msaa()` to both `d3d12::RenderTargetImage` and `vulkan::RenderTargetImage`
(mirroring `DepthTargetImage`'s exact API and doc comment), refactoring the
existing `readback_sample()` to compute the guest→host mapping and delegate
to it (matching depth's existing structure - no behavior change for any
existing caller), and adding the same padding-sample assertion depth already
had to both backends' 2x-fallback tests in `backend_capabilities.cpp`:
host samples 0 and 3 must contain the two real guest samples' colors; host
samples 1 and 2 must remain exactly at the clear color. Verified against
real hardware (NVIDIA RTX 5090, both Vulkan and D3D12).

## Known, tracked gaps (explicitly incomplete, not silently assumed done)

### Scenario 5 (render → resolve → texture sample) has no end-to-end test

No test renders, resolves to guest memory, then re-binds that guest address
as a `TextureImage` and samples it through an actual draw whose shader
output is read back and compared - `backend_capabilities.cpp` creates
textures from CPU-written guest memory and binds them, but never draws with
them. Building this properly requires either a real guest pixel shader
(constructed as `DecodedShader` IR directly, bypassing PPC decode - see
`tests/graphics/xenos/shader_lowering.cpp` for the pattern) that samples a
bound texture and writes it to the render target, or independently verifying
`TextureDescriptor`'s exact pitch/tiling field semantics against a resolve
destination's tiled layout before trusting a `decode_texture()`-only
round-trip. Neither was attempted in this pass without that verification -
a test that merely looks plausible but silently encodes the wrong tiling
convention would be worse than no test, per this project's standing rule
against writing something and reporting done just because it compiles.

### Scenario 7 (CPU write → GPU sample, cache invalidation) has no regression test

`TextureDirtyTracker::consume_dirty()` is wired into both backends'
`consume()` (draw-time texture rebind path) and unit-tested in isolation
(`tests/graphics/xenos/texture.cpp`'s hash+epoch logic), but no test writes
guest memory *after* a texture has already been uploaded/cached, redraws,
and asserts the GPU-visible result reflects the new bytes rather than a
stale cached image. This is the same "needs a real sampling draw to observe
the result" constraint as scenario 5, and was deferred for the same reason.

### `ResourceBarrierPlanner` correctness beyond same-tile EDRAM alias is untested

The color↔depth EDRAM alias tests (scenario 4) prove same-tile aliasing
correctness, but `resource_barrier.cpp`'s own test coverage is pure
state-machine bookkeeping (no GPU resource, no pixel data) - general
resource aliasing (e.g. a texture importing directly from a resolve
destination address that overlaps a live, different-sized render target)
has no correctness assurance beyond the EDRAM-specific case. Tracked, not
fixed, in this pass.

## Tests

- `tests/graphics/backends/backend_capabilities.cpp`: both backends' 2x
  MSAA color fallback now asserts padding samples 1/2 remain at the clear
  color, matching depth's existing assertion.

# Memory / GPU Coherency Assertions — AC6 Runtime Readiness pass, Part 10

Part 10 asked to audit Memory V2's interaction with GPU V1 and add
assertions/telemetry rather than silently returning stale data, without
reintroducing a global RAM lock.

## Audit result: the coherency mechanism already exists and is correct

CPU-write -> GPU-sample coherency is handled by `TextureDirtyTracker`
(`include/xenon/gpu/texture.hpp`) keyed by `memory::GuestMemoryCoherency`
epochs, and is genuinely wired into both backends' draw-time texture bind
path (`consume()`'s `ir::DrawPacket` handling): a texture already in
`impl_->textures` is re-validated via `consume_dirty(key, coherency,
dirty_epoch)` before every draw that samples it, and only re-decoded/
re-uploaded from guest memory when a real CPU write landed in its range
since it was last uploaded. This is per-texture, lock-free epoch tracking -
no global RAM lock exists or was reintroduced. `GuestMemoryMirror`
(`make_cpu_visible`) provides the GPU-write-then-CPU-read direction, and
EDRAM's canonical/native ownership handoff (`EdramOwnershipTracker`,
audited in Part 8) already covers ownership handoff and aliasing.

## What this pass added: the invalidation itself was silent

Both backends' texture-rebind branch computed `refresh` (was `!cached ||
dirty`) but never distinguished "first-time upload" from "re-upload because
the cache was found stale" - the actual coherency EVENT Part 10 asks to make
observable had no telemetry at all. Fixed by splitting the condition into
named `already_cached`/`dirty` locals (behavior-preserving - `consume_dirty`
is still only called when `already_cached`, exactly matching the original
short-circuit `||`) and incrementing a new
`GpuPerformanceCounters::texture_cache_invalidations` counter exactly when
`dirty` is true, in both `d3d12::Backend` and `vulkan::Backend`. Surfaced as
`textureCacheInvalidations` in `capability_report()`'s existing `"gpu"`
section (Part 7).

## Known, tracked gap

No test exercises this counter's nonzero case: doing so needs a texture
actually sampled by a real draw (the same "needs a real guest pixel shader"
constraint noted in `docs/graphics/EDRAM_RESOLVE_AUDIT.md`'s scenario 5/7
gaps) followed by a second CPU write and a second draw. The zero-default
case is tested (`NullBackend`, fresh session). The underlying mechanism
itself (`TextureDirtyTracker`'s hash+epoch logic) is already unit-tested in
isolation in `tests/graphics/xenos/texture.cpp`.

# GPU 09: native Xenos textures

GPU 09 realizes Xenos texture fetch constants as native Vulkan and Direct3D 12
images without introducing an emulated guest GPU backend.

## Common texture model

The common graphics layer owns a complete catalogue of all 64 Xenos surface
formats. Each entry records block geometry, bits per pixel, storage class and a
lossless native format where one exists. Formats requiring an exact conversion
that is not implemented remain explicit diagnostics; they are never silently
substituted with a merely similar format.

`build_texture_layout` produces base, mip, array, cube and 3D subresources from
the generic GPU 08 descriptor. It handles guest pitch, 256-byte linear mip
rows, 4 KiB subresource alignment, 32x32x4 storage alignment and packed mip-tail
origins. `decode_texture` applies Xenos 2D/3D tiled addressing and endian modes
while producing tightly packed host subresources.

## Native realization

Vulkan creates device-local optimal images, staging uploads, image views and
samplers, transitions them with synchronization2 and writes the dimension-
specific descriptor and sampler array elements established by GPU 08.

Direct3D 12 creates default-heap textures, uses device-provided copyable
footprints for upload, transitions to shader-resource state and writes the
matching SRV and sampler heap slots. Both mappings include uncompressed integer
and floating-point formats plus BC1/2/3/4/5. Image views apply the generic
fetch-constant component swizzle, sampler filters remain independent, and
anisotropy is enabled where the host device supports it. Native-format support
is checked before allocation.

Shader reflection controls realization: a draw uploads only fetch constants
actually referenced by its vertex or pixel shaders. The cache key is the
generic texture descriptor hash. Physical-memory write callbacks mark every
overlapping cached resource dirty, and the next referencing draw recreates and
rebinds it.

Texture fetch lowering now supports computed LOD, register LOD, register
gradients, instruction bias, half-texel offsets and unnormalized coordinates
for 1D, 2D, 3D and cube resources. Query opcodes and formats without exact host
representations remain diagnosed for later trace-driven expansion.

The Xenos 24/8 depth encodings deliberately remain unsupported as sampled
images here: their exact conversion and EDRAM interaction belong to GPU 10,
rather than being misrepresented as a bit-compatible host depth image.

## CPU-write to GPU-sample coherency

CPU-write -> GPU-sample coherency is owned by `TextureDirtyTracker`
(`include/xenon/gpu/texture.hpp`), keyed by `memory::GuestMemoryCoherency`
epochs rather than a global RAM lock. It is wired into both backends'
draw-time texture-bind path: a texture already present in the backend's
texture cache is re-validated via `consume_dirty(key, coherency,
dirty_epoch)` before every draw that samples it, and is only re-decoded and
re-uploaded from guest memory when a real CPU write landed in its range since
the last upload. This is per-texture, lock-free epoch tracking. The
GPU-write-then-CPU-read direction is handled separately by
`GuestMemoryMirror::make_cpu_visible`, and EDRAM ownership handoff/aliasing is
owned by `EdramOwnershipTracker` (see `docs/graphics/EDRAM_RENDERING.md`).

Both backends distinguish "first-time upload" from "re-upload because the
cache was found stale" via named `already_cached`/`dirty` locals, and
increment `GpuPerformanceCounters::texture_cache_invalidations` exactly when
`dirty` is true (behavior-preserving: `consume_dirty` is still only called
when `already_cached`). The counter is surfaced as `textureCacheInvalidations`
in `capability_report()`'s `"gpu"` section alongside `GpuUnsupportedCounters`
(see `docs/graphics/NATIVE_BACKENDS.md`).

No test currently exercises this counter's nonzero case end-to-end: that
needs a texture actually sampled by a real draw followed by a second CPU
write and a second draw — the same "needs a real guest pixel shader"
constraint tracked as scenarios 5 and 7 in
`docs/graphics/EDRAM_RENDERING.md`'s resolve-correctness-audit section. The
zero-default case is tested (`NullBackend`, fresh session), and the
underlying `TextureDirtyTracker` hash+epoch logic is unit-tested in isolation
in `tests/graphics/xenos/texture.cpp`.

## Next boundary

GPU 10 owns EDRAM color/depth surfaces, MSAA, clears and resolves; concrete
Vulkan/D3D12 graphics pipeline creation and native draw submission; and the
swapchain/presentation boundary. It consumes GPU 05 draw IR, GPU 07 binaries,
GPU 08 state/layouts and GPU 09 native textures.

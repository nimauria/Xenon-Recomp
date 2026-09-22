# GPU V1 completion checklist

GPU V1 preserves the existing architecture:

Xenos semantics -> canonical Xenon graphics representation -> native backend.

The backends do not emulate the Xenos command processor. They consume the
shared IR, EDRAM model, ownership tracker, resource barriers, texture codecs,
and translated shader representation.

## Completed correctness gates

- Color resolves support raw and converted destinations, tiled addressing,
  destination pitch/height, 128-bit endian modes, offsets, format conversion,
  gamma destinations, red/blue swap, and selected MSAA samples.
- Multi-sample color averaging is performed by the shared host-format codec
  rather than a backend-native resolve operation, keeping Vulkan and D3D12
  byte-equivalent.
- D24S8 and D24FS8 depth resolves preserve packed depth and stencil values.
- Resolve rectangles are decoded and clipped in the common Xenos layer.
- Post-resolve color and depth clears reconcile native ownership before
  modifying canonical EDRAM.
- EDRAM ownership is tracked per physical tile and alias transitions are
  explicit, transactional, and shared by both native backends.
- Native resources are retired behind queue completion values and backend
  destruction drains the queue before releasing resources.
- Shader translation and pipeline caches are keyed by canonical shader/state
  identity.
- Portable capture/replay stores register state, normalized IR, guest memory,
  shaders, and canonical EDRAM state for submission-level replay.
- RectangleList, CopyRectList0-3, and FillRectList are normalized through the
  shared rectangle-expansion path used by both native backends.
- Resolve-region clears clip safely at surface boundaries, preserve all MSAA
  samples, and reject overflowing source/destination ranges.
- Vulkan and D3D12 expose cumulative GPU performance counters for submissions,
  commands, draws, shader/pipeline cache misses, and submission time. These
  counters are backend-neutral observations and do not alter Xbox semantics.
- Native command queues use rotating frame contexts with completion-value
  retirement; presentation back buffers and upload allocations are held until
  their associated queue value completes.

## Remaining qualification work

The following are validation and platform-qualification gates rather than
alternative GPU semantics:

- Run captured retail command streams, including AC6 traces, through replay.
- Qualify resize, fullscreen/windowed transitions, device-loss recovery, and
  vsync modes on each presentation platform. The presentation APIs now return
  explicit `OutOfDate`, `SurfaceLost`, `Suboptimal`, and minimized statuses;
  actual device-loss recovery still requires a host runtime to recreate the
  native device and surface.
- Run long-lived native stress tests on hardware for transient descriptors,
  command allocators, swapchain images, and shader variants. The queue
  implementation already rotates three protected frame contexts.
- Record release-build backend counters during real workloads. The counters
  are now available through `Backend::performance_counters()`; texture upload,
  EDRAM transfer, readback, and barrier-specific attribution remains a
  workload/trace measurement step.

Unsupported formats or commands must remain explicit diagnostics. They must not
be silently treated as a different Xenos operation.

## Validation

Portable Xenos tests cover EDRAM layout and ownership, depth conversion,
texture codecs, shader translation, resource barriers, and canonical resolve
results. Backend integration tests exercise native ownership and resolve
paths where the corresponding SDK and runtime are available.

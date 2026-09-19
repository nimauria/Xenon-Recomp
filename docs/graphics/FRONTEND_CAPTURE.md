# Xenos frontend capture and replay

The Xenos frontend provides two deterministic capture layers for bring-up,
differential testing and backend debugging:

1. **Normalized in-process capture** (`FrontendSubmissionCapture`) records the
   register boundary and normalized Xenon graphics IR for one PM4 submission.
2. **Portable resource-complete submission capture**
   (`PortableSubmissionCapture`, schema v1) adds the guest-memory and canonical
   EDRAM state needed to replay that submission without depending on the live
   resource contents from the original run.

The portable layer is intentionally built on the common Xenos frontend and
Memory V2. Vulkan and D3D12 do not decode PM4 independently and do not maintain
a second guest-memory model for capture.

## Normalized frontend capture

`FrontendSubmissionCapture` records:

- whether the source was a linear command buffer or circular primary ring;
- physical command-stream location and submitted dword bounds;
- ring read/write metadata and the resulting read index for ring submissions;
- a complete `RegisterFile` snapshot immediately before submission;
- a complete `RegisterFile` snapshot immediately after submission;
- the active vertex and pixel programs at the submission boundary, including
  shaders loaded by an earlier submission; and
- exactly the normalized graphics IR commands appended by this submission.

Capturing executes the frontend normally. PM4 validation, register writes,
Memory V2 side effects and command-processor state therefore match an ordinary
submission.

`GraphicsSystem::replay_capture` emits the captured initial register state,
restores the captured active shaders as a replay preamble, then emits the
captured IR in original order. It does not consume the live command stream and
it does not mutate the live frontend `RegisterFile`.

## Portable capture schema v1

`PortableSubmissionCapture` extends the normalized submission with:

- a schema version and Memory V2 coherency epoch;
- merged physical-memory snapshots tagged by their usage;
- the complete canonical 10 MiB Xenos EDRAM image;
- a `complete` flag; and
- diagnostics for dependencies that cannot yet be proven statically.

Physical range usage currently distinguishes:

- root command-stream data;
- indirect command buffers;
- pointer-loaded shader source;
- vertex buffers;
- index buffers;
- texture subresources;
- resolve destinations;
- statically known memory-export destinations; and
- frontend physical-memory side effects such as fences and shader-store
  metadata.

Active shaders are retained directly in the capture, so a submission is not
silently dependent on a backend shader cache populated by an earlier frame.

### Resource discovery

The portable builder replays the initial register snapshot through the common
`ResourceStateTracker`, then follows the normalized commands in order. Draw
resource snapshots are derived from the same descriptors and planning code used
by the native backends:

- indexed draws retain their active index-buffer range;
- all active vertex-fetch ranges are retained conservatively;
- shader reflection identifies sampled texture fetch slots where possible;
- texture descriptors are expanded with `build_texture_layout`, including
  mip/slice storage;
- static memory-export ranges are taken from the common memexport planner;
- resolve vertices are snapshotted and passed through the common resolve
  planner before the destination footprint is retained; and
- the full canonical EDRAM image is retained rather than encoding backend
  render-target ownership into the file format.

If shader reflection is unavailable, the capture keeps all active texture slots
and marks the artifact incomplete/conservative. Dynamically addressed memory
exports also mark the capture incomplete because arbitrary destinations cannot
be proven statically. These conditions are diagnostics, not guessed ranges.

## Memory V2 integration

Portable capture does **not** bypass Memory V2.

Before a physical range is copied, `GraphicsSystem` asks the active backend to
make that guest range CPU-visible. This resolves any native GPU-newer mirror
back into the canonical Memory V2 physical store. The bytes are then read with
`AddressSpace::copy_physical_range`, the race-safe snapshot primitive already
used by production GPU resource paths.

Before the EDRAM image is copied, the backend is asked to make native EDRAM
ownership canonical. This prevents a capture from archiving stale CPU-side
EDRAM while a Vulkan or D3D12 render target still owns newer samples.

Replay restores captured guest RAM through `AddressSpace::write_physical`
rather than writing the backing store directly. Consequently Memory V2 remains
the authority for:

- physical alias identity;
- dirty tracking;
- reservation invalidation;
- executable-generation notifications where relevant; and
- CPU/GPU coherency epochs.

After restoring canonical EDRAM, replay tells the backend to discard stale
native EDRAM ownership. Cached native images may continue to exist, but none is
considered authoritative until reacquired from the restored canonical store.

No Memory V2 redesign or separate GPU RAM implementation is required for this
capture layer.

## Persistent format

`save_portable_capture` and `load_portable_capture` implement a versioned binary
format with:

- magic/version/endian compatibility checks;
- register-count compatibility validation;
- normalized IR serialization for every current command variant;
- active shader programs with hash validation and decoder reconstruction;
- tagged physical resource blobs;
- the canonical EDRAM image; and
- diagnostics/completeness metadata.

Decoded shader IR is intentionally reconstructed from the captured shader
program rather than serialized as a second long-lived ABI.

The schema is currently submission-oriented rather than a multi-submission
frame bundle. Presentation metadata and aggregation of several submissions into
one portable frame artifact can be layered above schema v1 without moving PM4
semantics into a host backend.

## Remaining capture qualification

The resource-complete submission format closes the previous dependency on live
Memory V2 contents, but it still needs real-title qualification:

- capture and reload real Project Gracemeria / Ace Combat 6 submissions;
- compare replay on Vulkan and D3D12 on Windows hardware;
- confirm whether AC6 exposes currently lossless/unexecuted PM4 packets such as
  `SET_STATE`, `MEM_WRITE_CNTR` or additional event/query forms;
- add any capture-driven shader/tessellation/memexport edge cases; and
- optionally add a higher-level full-frame bundle that groups multiple
  submissions plus presentation metadata.

Those are bring-up/qualification gates, not reasons to fork the memory or PM4
model per backend.

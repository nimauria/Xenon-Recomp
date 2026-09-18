# GPU 08: native resource and pipeline-state boundary

GPU 08 turns Xenos register writes into host-independent draw resource state
and establishes the matching Vulkan and Direct3D 12 shader-resource layouts.
Host backends do not decode PM4 packets or infer title-specific semantics.

## Common resource state

`ResourceStateTracker` consumes normalized `ir::RegisterWrite` commands and
decodes all 32 six-dword texture fetch constants, all 96 vertex-buffer fetch
descriptors, four color targets, depth target, surface pitch, MSAA mode,
scissor and raster culling. Descriptors and draw pipeline state have stable,
field-wise hashes that do not depend on C++ padding or process addresses.

The tracker also materializes the shader constant buffer used by GPU 07:

- 256 vertex and 256 pixel `float4` constants in separate Xenos banks
- eight packed boolean words
- 32 loop constants
- the base word of all 32 vertex fetch constants
- the signed texture result exponent adjustment for all 32 fetch constants

The exponent adjustment comes from fetch-constant word 3 and is distinct from
the LOD bias. Generated shaders apply it to the sampled result after the
texture operation, while the descriptor hash keeps it part of resource
identity.

The remaining fetch descriptor words stay in `DrawResourceState`; later native
vertex realization must use that decoded structure rather than extending the
constant-buffer ABI with game-specific input layouts.

## Native layouts

Vulkan owns one descriptor-set layout and pipeline layout. DXC register-class
shifts map `b0`, `t0..t128`, `s0..s31` and `u0` into distinct Vulkan bindings.
The full 512 MiB guest-memory mirror is bound as a storage buffer for vertex
fetch and memory export. Texture and sampler arrays preserve Xenos fetch-
constant indices.

Direct3D 12 owns the equivalent root signature, shader-visible 129-entry SRV
heap, separate 32-entry sampler heap and an upload-backed root constant buffer.
The descriptor increments are queried from the device. Null descriptors and
default samplers make every ABI slot defined before a texture is realized.

Both backends track unique pipeline-state identities on normalized draws and
upload constants at the draw boundary. Shader compilation remains exclusively
owned by the GPU 07 DXC cache.

## Deliberate next boundary

GPU 08 does not pretend an Xbox tiled texture or EDRAM surface is a linear host
image. GPU 09 now provides generic format metadata, endian conversion,
tiling/detiling, mip and sampler realization, native image lifetime and dirty
tracking. GPU 10 can therefore build concrete Vulkan/D3D12 graphics pipelines,
EDRAM render/resolve behavior and presentation on top of real image formats.
Texture query formats without exact semantics and memory exports remain
diagnostic rather than receiving title-specific fallbacks.

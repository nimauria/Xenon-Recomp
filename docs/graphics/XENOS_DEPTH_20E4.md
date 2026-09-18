# GPU 10A: Xenos 20e4 floating depth

Xbox 360 `D24FS8` is not IEEE 32-bit depth. Its 24-bit depth payload is a
positive 20-bit-mantissa / 4-bit-exponent floating-point value (20e4) with a
representable range of `[0, 2)`. A host `D32_FLOAT` attachment is therefore only
a storage container: draws targeting `D24FS8` must still observe the Xenos
precision lattice.

## Implemented

- `float32_to_20e4` converts IEEE float32 to the 24-bit Xenos representation.
- `float20e4_to_float32` expands every Xenos code back to IEEE float32.
- Both truncation and round-to-nearest-even conversion policies are available.
- The CPU implementation preserves Xenos denormals, zero/negative/NaN handling
  and saturation at the largest 20e4 value.
- Pixel-shader reflection recognizes Xenos depth export `e61.x`.
- Scalar and vector export ALU results both use the Xenos vector export
  destination, rather than treating the scalar temporary destination as an
  export register.
- Generated HLSL contains the same 20e4 codec and emits `SV_Depth` when the
  guest pixel shader writes depth.
- For a `D24FS8` target, the native backends lazily request a pixel-shader
  variant that quantizes either guest `e61.x` or implicit raster depth to 20e4
  before the host depth test/write.
- The compatibility-first policy is round-to-nearest-even. Truncation remains
  exposed for future performance/compatibility tuning.
- Multisampled D24FS8 variants consume `SV_SampleIndex` so conversion is run at
  sample frequency rather than once per pixel.

## Validation

The common graphics suite exercises known values, denormal boundaries, rounding
mode differences, shader reflection and generated HLSL. An additional exhaustive
validation iterated every one of the 16,777,216 possible 20e4 codes and verified
that expand -> nearest-even encode returned the original code.

Native Vulkan/D3D12 compilation and hardware execution still require the
platform SDKs. The Linux validation environment used for this section does not
contain Vulkan development headers or the Windows/DXC SDK, so the backend
variant wiring is source-validated here and must also pass the existing Windows
hardware matrix before the full D24FS8 gate is considered closed.

## Still required for full D24FS8 fidelity

This section closes draw-time precision, not EDRAM ownership. The remaining
EDRAM work must preserve the complete `[0, 2)` 20e4 range while moving depth
between overlapping native surfaces, EDRAM tiles and guest memory. Ordinary
Vulkan/D3D12 viewport depth ranges are `[0, 1]`, so ownership/resolve transfers
need an explicit reversible mapping rather than relying on a native depth image
alone.

The implementation is based on public Xenos hardware research (including the
20e4 conversion behaviour documented by Xenia), but remains part of Xenon's
host-independent native renderer rather than reproducing Xenia's GPU execution
architecture.

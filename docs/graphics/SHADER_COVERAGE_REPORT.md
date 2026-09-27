# Shader Coverage Report — AC6 Runtime Readiness pass, Part 9

`GpuShaderCoverage` (`include/xenon/gpu/backend.hpp`), queryable via
`Backend::shader_coverage()` and published as `capability_report()`'s
`"shader"` section (omitted, like `"gpu"`, when no GPU backend exists):

- `shadersDiscovered` - distinct `ir::ShaderLoad` programs seen
  (`Impl::decoded_shaders.size()` on both backends). This is a live count:
  shaders are discovered dynamically as a title streams them, not a static
  "every shader known before boot" requirement, per this Part's own
  instruction not to require that.
- `shadersTranslated` / `translationFailures` - `HlslShaderLowerer::lower()`
  outcomes on the primary `ir::ShaderLoad` path specifically (not the
  float20-depth/writable-guest-memory re-lowerings of an already-discovered
  shader, which would double-count). `translationFailures` is incremented at
  the exact point `!lowered.complete` is observed - the same site Part 7's
  `unsupportedShaderInstructions`/`unsupportedShaderFeatures`/
  `unsupportedFetchFormats` counters increment from, so a translation
  failure and its specific reason are always counted together.
- `cacheHits` / `cacheMisses` - `ShaderCache::hits()`/`misses()`, which
  already existed (`include/xenon/gpu/dxc_shader_compiler.hpp`) and needed
  no changes; this Part only surfaces them.

The AC6 target is `translationFailures == 0` for a fully-supported run -
like every other counter introduced in this pass, this must reflect reality
and must never be tuned to read as zero.

## Known simplification

`shadersTranslated = shadersDiscovered - translationFailures` (floor 0) is
an approximation, not a per-shader ledger: if the *same* guest shader hash
is reloaded and fails translation more than once (a title that repeatedly
issues an `ir::ShaderLoad` for the same broken program), `translationFailures`
increments each time but `shadersDiscovered` does not (it is keyed by hash).
This does not affect the two counters that matter operationally
(`shadersDiscovered`, `translationFailures` - both stay accurate on their own
terms), only the derived `shadersTranslated` convenience field.

## Tests

- `tests/graphics/xenos/gpu_frontend.cpp`: `NullBackend`'s inherited default
  `shader_coverage()` reads real, honest zeros.
- `tests/core/session_tests.cpp`: `capability_report()` publishes a real
  `"shader"` section when a GPU backend exists (all-zero for a fresh
  session) and omits it when one does not.

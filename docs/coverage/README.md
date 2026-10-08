# Xenon implementation coverage dashboard

![Source-declared coverage treemaps](dashboard.svg)

The [generated coverage report](REPORT.md) gives exact counts and limitations. The separate [kernel export reference reconciliation](KERNEL_REFERENCE.md) compares Xenon's source registrations with a pinned Xenia research table. Its registration percentage does not measure implementation. This audit measures traced operations in selected Xenon source inventories. It does **not** measure game compatibility or the complete Xbox 360 API/ISA. An unassessed purple cell means the implementation and tests have not yet been audited; it does not assert that the operation is missing. Green requires a behavior-oriented test, blue has an implementation trace without test verification, yellow is partial or a stub, and grey is explicitly unimplemented.

The [Phase 2 metadata audit](PHASE2_METADATA_AUDIT.md) records corrected discrepancies and remaining source-inventory gaps.

## Inventories

- Kernel: statically recoverable ordinal/name registrations and variables extracted from `src/xbox/exports/`, `src/audio/exports.cpp`, and `src/core/session/exports/`, grouped by subsystem. Audio exports are included even though their registration depends on build configuration. The ten-row diagnostic RTL table is validated against registration sources, not used as a denominator. Operations absent from Xenon source remain outside the dashboard inventory. The pinned Xenia table is a separate research reference, not an official or version-qualified console export inventory.
- PPC: all `X(...)` rows in `src/cpu/ppc/decoder/opcode_catalog.inc`, grouped by its declared instruction family. These are recognized mnemonics, not proof of execution semantics.
- Shader: `ControlFlowOpcode` declarations, ALU ranges and reserved scalar opcode from lowerer checks, texture fetch cases from the lowerer, and the decoder's vertex fetch opcode. These are frontend-recognized forms, not an exhaustive hardware instruction list. Scalar opcode 41 is reserved and excluded.
- PM4: packet header types and `Type3Opcode` declarations in `include/xenon/gpu/types.hpp`, grouped by command processor classifiers. An IR passthrough is not full packet execution.

## Updating coverage

1. Change the authoritative source inventory when adding an opcode or export. Do not copy all inventory rows into the manifest.
2. Add a sparse entry to `tools/coverage/coverage.json` for each operation whose behavior has been audited. Use the source-derived `id` shown in the SVG tooltip. Choose `verified`, `implemented_unverified`, `partial`, or `unimplemented`. Include an implementation source path and an exact token in that file if the identifier itself is absent. A verified entry also needs a test path and an exact test token. Partial and unimplemented entries need an explanatory note; a stub should use `"kind": "stub"`.
3. Ensure a verified test asserts behavior, not just that decoding or registration succeeds. Run the relevant test executable. If the test only checks registration, use `implemented_unverified` until a behavior test exists.
4. Run `python3 -m unittest discover -s tools/coverage -p 'test_*.py' -v` and `python3 tools/coverage/generate.py`. Commit the manifest, SVG, and reports together. `python3 tools/coverage/generate.py --check` is the CI stale-asset check. The kernel reference snapshot stays pinned to its cited upstream commit; update it only after checking every changed ordinal, name, and kind against the new source revision.

The script uses only Python's standard library and fails on duplicate or missing IDs, invalid states, missing evidence paths or tokens, source-inventory extraction drift, and stale generated files. Its evidence check establishes traceability; semantic review remains a contributor responsibility.

## Remaining metadata work

- Check the 668 reference entries without Xenon source registrations against actual title imports, console kernel versions, and any dynamic paths. The reference is a pinned research table; an authoritative version-qualified export inventory and reserved-ordinal classification are still needed.
- Connect PPC catalog entries to per-opcode lifting, fallback, and semantic test metadata. Current decoder metadata does not identify stubs or tested execution paths.
- Give Xenos ALU and fetch opcodes named source metadata and explicit test coverage tags; current numeric ranges and HLSL branches make semantic auditing difficult.
- Distinguish PM4 packet parsing, normalized IR emission, backend execution, and tests per opcode. The type-3 enum and command classifier do not encode those layers.

# Phase 2 coverage metadata audit

The [dashboard](README.md) now extracts 254 statically recoverable `xboxkrnl` registrations from export specs, session variables and direct descriptors, I/O import specs, and the audio ordinal constants paired with their registration calls. Duplicate names with different ordinals, duplicate ordinals with different names, audio name/ordinal-symbol drift, and disagreements with the diagnostic RTL metadata sample fail generation. This is a Xenon **source-declared** inventory, not a complete Xbox 360 export table. Build-conditional registrations and titles' actual import use are separate questions.

## Corrected discrepancies

- The diagnostic metadata lookup normalized only the caller's module name and therefore failed for its own `xboxkrnl.exe` rows. It now normalizes both sides.
- The RTL sample contained shifted ordinals. The corrected ten entries agree with Xenon's registration specs and the [Xenia xboxkrnl export table](https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/xboxkrnl/xboxkrnl_table.inc). `RtlFillMemory` and `RtlZeroMemory` were replaced by the actual `RtlCompareMemoryUlong` and `RtlFillMemoryUlong` exports; the former names are not entries in that reference table.
- `KeDelayExecutionThread` previously lacked the registry's `partial` flag even though its implementation explicitly ignores alertable/APC behavior. The descriptor now carries that limitation. Its relative-delay behavior remains covered by a test.

The dashboard still treats unassessed entries as unknown. A registered handler, generated HLSL branch, or PM4 IR packet is not proof of complete guest-visible behavior.

## Remaining metadata gaps

- Reconcile the source-declared kernel list with a complete, versioned console export table, including exports Xenon does not register and reserved ordinals. This audit establishes no percentage against that external universe.
- Add machine-readable per-opcode lift, fallback, and behavior-test links for the PPC catalog. The decoder catalog alone has no execution-status field.
- Give shader ALU/fetch forms named metadata and tests that assert lowering behavior per form; numerical bounds only show which forms the frontend accepts.
- Track PM4 parsing, normalized IR, backend effect, and test evidence as separate stages. A type-3 enum member may only reach passthrough IR.

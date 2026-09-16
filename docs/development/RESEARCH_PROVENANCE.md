# CPU research provenance

Xenon CPU is an independent static-recompilation implementation. Reference projects and
public architectural documents were used to identify guest-visible behavior and validate
coverage; their implementation structures are not runtime dependencies of Xenon.

Primary research references used during CPU bring-up:

- IBM / PowerPC architectural documentation for fixed-point, floating-point, control,
  synchronization and vector semantics.
- AltiVec Technology Programming Environments Manual for VMX element, vector memory and
  vector arithmetic behavior.
- Xenia (`xenia-project/xenia`, BSD-licensed) as a broad Xbox 360 PPC/VMX128 research
  reference and opcode-coverage cross-check.
- ReXGlue / AC6_recomp (`sal063/AC6_recomp`) as a static-recompilation reference for the
  instruction families exercised by Ace Combat 6.
- Public VMX128 reverse-engineering documentation for Xbox-specific register encodings and
  instructions.

The checked-in `src/opcode_catalog.inc` is Xenon's declarative architecture metadata:
mnemonic, canonical bit pattern, encoding form and classification. No Xenia source file is
required to build or regenerate Xenon's CPU implementation.

Where a reference implementation differed from the architecture specification, Xenon uses
architectural semantics. One concrete example is `lvebx/lvehx/lvewx`: Xenon implements
AltiVec element-load semantics rather than Xenia's current full-vector-load shortcut.

## GPU Phase 1 public research

The Xenos command-stream layout, PM4 opcode identifiers, register-file extent,
shader command formats, shader 96-bit/48-bit packing and EDRAM geometry were
cross-checked against public Xenia GPU documentation/source and publicly cited
R400/R500-era research. Xenon implements its own types, command processor,
graphics IR, shader container and EDRAM model rather than copying a host GPU
backend.

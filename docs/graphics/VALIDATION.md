# Xenon GPU Phase 1 Validation — 2026-09-16

## Result

The CPU + memory + GPU Phase 1 source tree passes all current tests in three
independent build modes:

- GCC Release, warnings as errors: 13/13
- Clang Release, warnings as errors: 13/13
- Clang Debug, AddressSanitizer + UndefinedBehaviorSanitizer: 13/13

## GPU frontend coverage

`xenon_gpu_frontend_tests` currently validates:

- packet type 0/1/2/3 header decoding
- type-0 sequential register writes
- type-0 one-register writes
- type-1 dual register writes
- circular primary ring wraparound
- nested indirect buffers
- constant register block mappings
- constant context loads from shared physical RAM
- PM4 memory writes and Xenos endian conversion
- CPU readback of a GPU physical-memory write
- draw packet ordering in graphics IR
- pointer-based shader load
- immediate shader load
- stable matching shader hash across equivalent load forms
- 96-bit shader groups and 48-bit control-flow unpacking
- EDRAM size, tile wrap and byte-address wrap
- backend command consumption and one-shot IR draining
- malformed/truncated command rejection

All pre-existing CPU and memory tests remain part of the same CTest run, so GPU
changes are checked for regressions against the 455-opcode CPU baseline and the
production memory implementation.

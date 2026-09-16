# Xenon CPU validation baseline — 2026-09-16

## Scope

This baseline freezes the retail-title static-recompilation CPU layer required before the
real Xenon memory subsystem is implemented. It targets Linux/x86-64 first. ARM64 is
intentionally deferred until the AC6/x86-64 path is running.

Implemented pipeline:

```
Xbox 360 PPC / VMX128 machine code
        -> Xenon decoder
        -> guest semantic frontend
        -> architecture-neutral Xenon IR
        -> basic optimizer
        -> native C++ AOT backend
        -> host C++ compiler
        -> x86-64 native code on Linux
```

The C++ AOT backend is a native static-recompilation backend, not a PPC interpreter. Guest
opcodes are decoded and lowered at compile time. Generated code contains no guest-opcode
runtime dispatcher.

## Architectural state

- 32 x 64-bit GPRs
- 32 x 64-bit FPR bit containers
- 128 x 128-bit VMX128 registers
- full 32-bit CR
- full 32-bit XER, including CA / OV / SO and string byte count
- full 32-bit FPSCR
- full 32-bit VSCR
- LR, CTR, MSR, VRSAVE, PVR and guest time-base interface
- load-reserve/store-conditional reservation state

## Instruction coverage

- 455 / 455 canonical Xenon PPC/VMX/VMX128 opcode patterns decode
- 455 / 455 lift to Xenon IR
- 455 / 455 lower to native AOT source
- the generated 455-function corpus compiles and executes without an interpreter fallback

## CPU-side memory semantics already tested

Only the CPU-facing contract and a flat test backing are present. This is not the real RAM
implementation.

Covered CPU semantics include:

- 8/16/32/64-bit big-endian scalar loads/stores
- byte-reversed loads/stores
- signed extension and update forms
- floating-point loads/stores
- load/store multiple
- load/store string, including XER byte count behavior
- VMX full, element, left/right and shift-control memory operations
- 32/64-bit load-reserve/store-conditional
- cache-block zero (32 and 128 bytes)
- barriers and instruction-cache invalidation hooks

## Validation gates

All checks below passed on Linux x86-64.

### GCC strict Release

- GCC 14.2.0
- `-Wall -Wextra -Wpedantic -Werror`
- 10 / 10 CTest suites passed

### Clang strict Release

- Clang 17.0.0
- `-Wall -Wextra -Wpedantic -Werror`
- strict guest-FP contract (`-frounding-math -ffp-contract=off -fno-fast-math`)
- 10 / 10 CTest suites passed

### Sanitizers

- Clang Debug
- AddressSanitizer + UndefinedBehaviorSanitizer
- leak detection enabled
- 10 / 10 CTest suites passed

## Test suites

1. `xenon_cpu_tests` — state, decoder, randomized operand-bit decoding/lifting,
   FPSCR/XER/VSCR helpers, MSR and base vector semantics.
2. `xenon_cpu_native_smoke` — generated arithmetic, memory, FP, vector, reservation,
   branch, rotate/mask, OE/Rc and FP compare behavior.
3. `xenon_cpu_exhaustive_native` — executes the generated canonical 455-op corpus.
4. `xenon_cpu_function_flow` — multi-block static recompilation and local branch flow.
5. `xenon_cpu_branch_matrix` — 320 BO/BI/CR/CTR conditional-branch combinations.
6. `xenon_cpu_control_boundaries` — calls, traps/syscalls, SPR delegation and timebase.
7. `xenon_cpu_vector_memory` — VMX memory semantics across alignments, including
   element loads/stores.
8. `xenon_cpu_indirect_branch` — LR/CTR branch and link ordering/target behavior.
9. `xenon_cpu_scalar_memory` — scalar/floating/string/cache CPU-memory semantics.
10. `xenon_cpu_integer_edges` — carry, borrow, overflow, shifts and compare edge cases.

## Important boundary

This is the point at which Project Xenon can move to the real memory subsystem. The CPU
already describes what memory must provide through `MemoryPort`, but it does not yet own:

- the Xbox 360 virtual/physical address map
- 512 MiB unified backing RAM
- page allocation, commit or protection
- aliases/MMIO routing
- executable page invalidation policy
- CPU/GPU coherency and Xenos-visible mappings

Those belong to the next memory phase.

## Deferred work

- ARM64 native validation/backend tuning is deliberately deferred.
- Direct hand-emitted x86-64 machine-code/JIT emission is an optional performance backend;
  the current AOT path already produces native x86-64 code through the host compiler.
- OS/kernel exception delivery, scheduling and system services belong above the CPU core.

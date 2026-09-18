# Xenon CPU + Memory Validation — 2026-09-16

## Frozen target

Current validated host target: Linux x86-64.

ARM64 remains intentionally deferred until the AC6 x86-64 path is running.

## Test matrix

All 12 CTest suites pass in each configuration:

- GCC 14.2 Release, warnings as errors: **12/12**
- Clang 17 Release, warnings as errors: **12/12**
- Clang 17 Debug + AddressSanitizer + UndefinedBehaviorSanitizer: **12/12**

The first ten suites are the previously frozen CPU validation set. The two new suites are:

### `xenon_memory_tests`

Validates:

- official guest region boundaries and allocation page sizes
- low/null virtual guard behavior
- 4 KiB virtual commit and cross-page access
- big-endian and byte-reversed little-endian storage
- virtual-to-physical translation
- A-view physical alias coherence
- 0x7F GPU/writeback alias coherence
- E-view 4 KiB physical offset
- 0x800/0x900 XEX aliasing
- execute/read/write protection faults
- 128-byte physical reservation invalidation
- unrelated-granule reservation survival
- reservation token address/granule binding
- explicit physical allocation + virtual mapping
- virtual mapping lifetime independent of physical allocation lifetime
- 64 KiB top-down allocation/alignment
- DMA/external-write reservation invalidation
- physical write observer callbacks
- MMIO overlay routing
- instruction-cache invalidation callback routing
- decommit/release/query state transitions

### `xenon_memory_cpu_integration`

Uses generated native code from real PPC instructions and the production
`memory::AddressSpace` rather than `FlatMemory`.

Validates:

- PPC `stw` -> Xenon IR -> native code -> AddressSpace
- PPC `lwz` sees the same physical bytes
- writes through the physical alias are immediately visible to translated PPC
- `lwarx` reservations are invalidated by alias writes
- `stwcx.` success/failure and CR0 behavior remain correct through AddressSpace
- `dcbz` changes the shared physical backing
- `icbi` reaches the memory/code invalidation callback

## CPU regression status

The complete 455-canonical-opcode CPU corpus still decodes, lifts, generates and executes
through the same frozen CPU path. No PPC interpreter/runtime guest-opcode fallback was added
to implement memory.

## Boundary for the next phase

The memory layer is now sufficient to begin the Xenos/GPU phase. GPU work should consume
physical RAM and MMIO through these interfaces rather than creating a separate game-specific
memory store.

Kernel APIs such as `NtAllocateVirtualMemory`, `MmAllocatePhysicalMemory`, profiles, saves,
content storage and filesystem policy are later runtime/kernel consumers of this memory
module; they are not part of the physical RAM/address-space implementation itself.

## Memory V2 validation — 2026-09-18

The first Memory V2 implementation slice adds a compact hot translation table, generated `MemoryAccessContext` loads/stores, physical mapping refcounts/pending-free ownership, backend-neutral guest-memory coherency, range-oriented fill/copy behavior and a host-VM abstraction.

New or expanded regression coverage includes:

- direct fast RAM access and MMIO slow-path fallback;
- six concurrent normal-RAM access threads;
- physical alias lifetime and delayed anonymous-page reuse;
- range operations across page boundaries;
- range-level write notification across discontiguous physical mappings;
- coherency epochs and texture dirty consumption;
- generated PPC scalar/vector memory access through `MemoryAccessContext`.

The complete generic Linux x86-64 Release CTest matrix is run after each foundational change. Native Vulkan backend compilation requires a Vulkan SDK and D3D12/Windows host-VM validation requires a Windows build, so those platform-specific checks remain separate required validation rather than being inferred from generic Linux tests. See [`../MEMORY_V2.md`](../MEMORY_V2.md) for the live completion status and remaining hardening work.

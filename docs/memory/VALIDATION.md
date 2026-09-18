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
- physical write observer callbacks (historical V1 coverage; removed by Memory V2)
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
- read-side quiescence preventing a retired physical page from being recycled
  while an older fast `MemoryAccessContext` remains alive;
- range operations across page boundaries;
- range-level write notification across discontiguous physical mappings;
- coherency epochs and texture dirty consumption;
- controlled physical/DMA writes automatically invalidating reservations and
  publishing coherency state;
- scoped physical write spans publishing their complete declared range on
  destruction;
- production raw physical backing being read-only outside the memory layer;
- removal of the synchronous physical-write observer branch from generated
  scalar stores;
- generated PPC scalar/vector memory access through `MemoryAccessContext`.

After the controlled-write/observer-removal slice, the complete generic Linux
x86-64 Release matrix remains **24/24 passing**.

The complete generic Linux x86-64 Release CTest matrix is run after each foundational change. Native Vulkan backend compilation requires a Vulkan SDK and D3D12/Windows host-VM validation requires a Windows build, so those platform-specific checks remain separate required validation rather than being inferred from generic Linux tests. See [`../MEMORY_V2.md`](../MEMORY_V2.md) for the live completion status and remaining hardening work.

### Physical allocator slice

The third Memory V2 slice replaces page-by-page physical allocation search with
an ordered coalescing range allocator while retaining the per-page ownership and
mapping-reference arrays as the semantic source of truth.

Additional regression coverage validates:

- bottom-up allocation starts after the reserved 16 MiB system/GPU window;
- top-down allocation selects the highest valid physical page;
- arbitrary supported power-of-two physical alignment;
- adjacent freed runs coalesce and satisfy a larger contiguous allocation;
- mapping previously unowned physical RAM removes those frames from allocator
  availability until mapping/ownership release;
- retired physical pages are tracked as ranges rather than discovered by a full
  131,072-page scan on subsequent allocation;
- deterministic 2,000-operation fragmentation stress mixing allocation sizes,
  alignments, top-down/bottom-up placement and frees;
- complete coalescence back to the lowest allocatable physical page after the
  fragmentation workload is released.

Fresh Linux x86-64 Release validation after this slice: **24/24 CTest suites
passed**. Targeted Debug ASan+UBSan validation for memory, CPU→memory,
texture/coherency and GPU frontend integration: **4/4 passed**.


### Physical ownership / reverse-mapping slice

The fourth Memory V2 slice hardens physical ownership with a sparse cold reverse
map from physical pages to dynamic Virtual/XEX guest pages. Mapping refcounts and
reverse-map membership are updated together under the management lock, while the
normal generated RAM path remains unchanged.

Additional regression coverage validates:

- overlapping virtual views of the same explicit physical allocation;
- release in different alias-lifetime states without premature physical reuse;
- XEX 0x8/0x9 aliases appearing as two dynamic mappings of one physical page;
- fixed 0x7F/A/C/E physical aliases retaining their direct architectural semantics;
- explicit mapping state being cleared on decommit before anonymous recommit;
- `validate_invariants()` cross-checking refcounts, reverse mappings, page state,
  physical ownership and free/retired allocator indexes;
- a deterministic 2,000-operation randomized alias model over 32 virtual slots
  and 16 shared physical pages, including alias-data coherence checks.

Fresh Linux x86-64 Release validation after this slice: **24/24 CTest suites
passed**. Targeted Debug ASan+UBSan validation: **4/4 passed** for memory,
CPU→memory, texture/coherency and GPU frontend integration. Clang Release also
builds and passes `xenon_memory_tests`.

### PPC reservation-monitor slice

The fifth Memory V2 slice replaces the V1 4,194,304-entry reservation-generation
table (~16 MiB) with six live reservation slots and a 4,194,304-bit sticky
reservation-seen bitmap (~512 KiB). Exact live reservations remain physical and
128-byte-granule conflicts are preserved.

Additional regression coverage validates:

- monitor metadata remains below 600 KiB;
- 32-bit load-reserve/store-conditional success and failure;
- 64-bit load-reserve/store-conditional;
- a conditional store through a different guest alias of the same physical word;
- same-128-byte-granule conflicts and neighboring-granule survival;
- controlled DMA/external writes invalidating reservations;
- a generated second load-reserve replacing the earlier `CpuState` reservation;
- six simultaneously live reservations on independent granules;
- six hardware-thread-shaped contention repeatedly updating one word without
  losing successful stores;
- generated `stwcx.` no longer imposing guest-virtual-address equality when
  Xenon Memory resolves both aliases to the same physical storage.

The monitor uses a short commit gate only around conditional-store commit. Normal
RAM writes remain outside the global `AddressSpace` management mutex and use the
shared writer section to invalidate reservations before publishing bytes.

Validation for this slice:

- GCC Linux x86-64 Release full generic matrix: **24/24 passed**;
- Debug ASan+UBSan targeted memory/CPU→memory/texture/GPU-front-end matrix:
  **4/4 passed**;
- GCC TSan `xenon_memory_tests`: **passed with no reported data race**; GCC
  warns that `atomic_thread_fence` is not instrumented by TSan, which is tracked
  with the still-pending PPC ordering phase rather than counted as barrier proof;
- Clang Release memory + generated CPU→memory integration: **2/2 passed**.

### PPC/Xenon memory-ordering slice

The sixth Memory V2 slice replaces the generic fence placeholders with an explicit
PowerPC/Xenon ordering model and tuned host mappings.

Coverage now validates:

- `sync` orders all four data-access pairs;
- `lwsync` orders Load->Load, Load->Store and Store->Store, but intentionally not
  Store->Load;
- `eieio` is represented as a device/cache-inhibited/write-combined ordering
  primitive rather than a normal cached-RAM fence;
- `isync` is represented as an instruction-stream synchronization boundary;
- a two-thread message-passing litmus over actual relaxed Memory V2 RAM using
  `lwsync`;
- generated AOT execution of `sync`, `lwsync`, `eieio` and `isync`;
- x86-64 uses TSO-aware lowering while the ARM64 path uses dedicated DSB/DMB/ISB
  sequences and therefore does not depend on x86's stronger host memory model.

LR/SC remains atomic/reservation-based without inventing an implicit full fence.
Executable-generation synchronization and concrete memory-type policy remain owned
by their later Memory V2 phases rather than being hidden inside this barrier pass.

Phase 10 validation result:

- GCC Linux x86-64 Release full generic matrix: **24/24 passed**;
- Debug ASan+UBSan targeted memory/CPU->memory/texture/GPU-front-end matrix:
  **4/4 passed**;
- Clang Release memory + generated CPU->memory integration: **2/2 passed**;
- x86-64 assembly probe: `lwsync`/`isync` emit no hardware fence, while `sync`
  and conservative `eieio` emit `mfence`;
- Clang AArch64 assembly syntax probe accepts `dsb ish`, `dmb ishst`,
  `dmb ishld`, `dmb osh` and `isb`.

### Block/range completion slice

The seventh Memory V2 delivery closes brief Phase 11 rather than leaving another
foundation-only checkpoint. The production block path now includes page/range
`read_bytes`/`write_bytes`/`fill_bytes`, zero/fill, overlap-safe copy/move,
generated `dcbz`, PPC string transfers, VMX partial left/right transfers and
batched physical/external writes.

Additional regression coverage validates:

- 16-byte writes crossing two physical pages publish two page-range coherency
  updates rather than sixteen scalar updates;
- overlapping physically-linear RAM copies preserve `memmove` semantics through atomic word-burst moves;
- non-linear non-overlapping RAM copies use bounded scratch and preserve bytes;
- reversed-page physical alias overlap takes the snapshot slow path and preserves
  the complete logical source;
- MMIO range read/write/copy remains on the precise slow path;
- generated PPC string operations cross a 4 KiB page boundary through the real
  `AddressSpace`;
- generated VMX partial left/right operations use the real range path;
- multi-dword Xenos `PM4_MEM_WRITE` publishes one complete physical write span.

Validation for the completed Phase 11 slice:

- GCC Linux x86-64 Release full generic matrix: **24/24 passed**;
- Debug ASan+UBSan targeted memory/CPU→memory/scalar/vector/GPU-front-end matrix:
  **5/5 passed**;
- Clang Release memory/CPU→memory/scalar/vector matrix: **4/4 passed**;
- GCC TSan `xenon_memory_tests`: **passed with no reported data race**.

Native Vulkan and D3D12 remain platform/SDK validation items and are not inferred
from the generic Linux test matrix.

### Windows native Memory V2 integration slice

The current Windows x86-64 tree was configured and built from the real repository with
Visual Studio, Vulkan SDK, DXC and D3D12 enabled. This validates the platform-specific
host VM implementation and the native GPU consumers rather than inferring their behavior
from the generic Linux matrix.

Additional coverage in this slice includes:

- Windows reserve/commit/protect/decommit/recommit/discard/release and zero-fill;
- sticky executable physical-page generations for CPU, controlled external and `icbi`
  writes;
- a 65,536-entry lock-free exact-write journal with conservative page fallback;
- byte-precise CPU/GPU ownership when both sides modify one physical page;
- Vulkan and D3D12 controlled readback through `write_physical`;
- Xenos color/depth 40-sample-half aliasing across native backends;
- deterministic two-sample resolve rounding rather than driver-specific UNORM resolve
  rounding;
- correct D3D12 single-sample resolve-image alignment.

Validation results:

- Visual Studio Debug full native matrix: **27/27 passed**;
- Visual Studio Release full native matrix: **27/27 passed**;
- Vulkan backend: NVIDIA GeForce RTX 3050 Ti Laptop GPU;
- D3D12 backend: NVIDIA GeForce RTX 3050 Ti Laptop GPU;
- Release Memory V2 benchmark: completed, including scalar/vector, random translation,
  cross-page, range, MMIO, reservation, six-thread and dirty-range cases.


### Phase 4 concurrency completion slice

The eighth Memory V2 delivery closes brief Phase 4 rather than leaving the global-lock
removal as a generated-code-only property. Production direct `AddressSpace` scalar,
vector and range RAM accesses now probe the same compact hot translation before any
management lock is taken. Cold locks remain for MMIO, faults and mapping management.

Additional concurrency hardening validates:

- six direct `AddressSpace` CPU threads repeatedly reading/writing disjoint aligned
  64-bit RAM while mapping commit/release churn executes concurrently;
- a controlled physical/DMA writer racing a CPU 64-bit reader on the same aligned
  physical word without torn values or C++ data races;
- `PhysicalWriteSpan` exposes bounded atomic `write`/`fill` operations rather than a
  raw mutable physical span;
- controlled physical reads use atomic guest-memory loads;
- slow unaligned RAM fallback uses atomic byte/word operations;
- common linear copy/move retains overlap-safe memmove semantics with atomic word
  bursts instead of raw host `std::memmove`;
- physical-page retirement/read-side quiescence continues to prevent ABA reuse while
  mapping publication changes concurrently.

Validation for the completed Phase 4 slice:

- GCC Linux x86-64 Release full generic matrix: **24/24 passed**;
- Debug ASan+UBSan targeted memory/CPU→memory/texture/GPU-front-end matrix:
  **4/4 passed**;
- Clang Release memory + generated CPU→memory integration: **2/2 passed**;
- GCC TSan `xenon_memory_tests`: **passed with no reported data race**, including the
  new CPU + DMA + mapping-churn stress.

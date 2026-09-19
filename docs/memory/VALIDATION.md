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

### Host VM completion + direct guest aperture

The newer Memory V2 baseline closes brief phases 5 and 6 rather than leaving the
host-VM layer and direct-aperture work as research-only scaffolding.

Phase 5 validation now covers:

- native Linux reserve/commit/protect/decommit/recommit/discard/release lifecycle;
- opaque move-only shared-memory objects;
- two writable host views of the same shared bytes;
- shared-view unmap/remap behavior;
- capability-tested fixed shared mappings used only by the optional guest aperture;
- platform separation between POSIX, Windows and fallback host-VM implementations.

Phase 6 validation now covers:

- explicit `Compact`, `DirectAperture` and `Auto` translation strategies;
- fixed 0x7F/A/C/E architectural aliases using the same physical backing;
- dynamic virtual commit/decommit/remap through the aperture;
- paired XEX 0x8/0x9 aliases;
- withdrawal of the direct hot-entry bit plus read-side quiescence before a fixed
  host view is replaced, preventing stale host-pointer reuse;
- transparent fallback to compact translation when fixed shared mappings are not
  supported or cannot be transitioned safely;
- dedicated compact-vs-direct Release benchmark reporting.

Current Linux x86-64 paired measurements do not justify making the 4 GiB aperture the
qualified default, so `Auto` remains compact unless explicitly enabled at build time.
This is a performance selection only; both strategies retain the same Xbox-visible
mapping semantics.

### PPC/Xenon memory-ordering completion

The earlier Phase 10 foundation is now closed against the full brief requirements.
In addition to the existing canonical `sync`/`lwsync`/`eieio`/`isync` matrix and
x86-64/ARM64 host mappings, the production memory system now exposes its actual ordering
domain from mapping state:

- normal cached RAM;
- write-combined RAM;
- cache-inhibited / `NoCache` RAM;
- MMIO/device pages.

`MemoryAccessContext` reads those classifications directly from compact hot metadata
where possible and delegates only slow/MMIO pages to the cold `AddressSpace` query.
`eieio` remains architecturally ineffective for ordinary cached RAM while ordering the
three device-like domains.

Generated `isync` is now a native translation boundary rather than only a host/compiler
fence. AOT code performs the instruction barrier and returns to the dispatcher at the
next guest PC, allowing executable-page generation validation to take effect before
execution continues.

Additional completion coverage includes:

- real mapping-domain tests for Normal/WC/CI/MMIO;
- a dedicated lightweight-sync message-passing litmus;
- a heavyweight-sync Store-Buffering litmus, where both threads reading zero is
  forbidden by Store->Load ordering;
- generated AOT verification that `isync` returns `FlowReason::Branch` to the exact
  next guest instruction;
- a dedicated `xenon_memory_ordering_tests` target suitable for sanitizer/TSan runs;
- Clang AArch64 assembly acceptance of `dsb ish`, `dmb ishst`, `dmb ishld`, `dmb osh`
  and `isb`.

Validation on this completion checkpoint:

- GCC Linux x86-64 Release full matrix: **28/28 passed**;
- targeted Debug ASan+UBSan memory/ordering/CPU->memory/host-VM/texture/GPU-front-end
  coverage: all selected tests pass;
- Clang Release memory, ordering, host-VM and CPU->memory targets pass;
- GCC TSan dedicated `xenon_memory_ordering_tests`: **passed with no reported race**.

The larger all-in-one `xenon_memory_tests` TSan executable remains slow enough to exceed
this environment's command execution window; that timeout produced no TSan race report
and is not substituted for the dedicated ordering TSan result.


### Phase 14 shared CPU/GPU coherency completion

Brief Phase 14 is closed around a single backend-neutral ownership model in Xenon Memory.
Validation covers:

- exact CPU/DMA write epoch ingestion before upload and readback planning;
- per-range GPU generations so unrelated writes remain independently committable;
- newer overlapping GPU writes surviving an older readback commit;
- CPU writes between readback planning and physical commit winning deterministically via
  the short physical-write quiescence window;
- source-aware acknowledgement of the mirror's own readback publication without dropping
  unrelated CPU/DMA dirt;
- discrete `Copy` and shared-host-visible `VisibilityOnly` synchronization actions;
- Vulkan and D3D12 mirror code consuming the same shared upload/readback plans.

Completion validation:

- GCC Linux x86-64 Release full matrix: **29/29 passed**;
- Debug ASan+UBSan targeted matrix: **6/6 passed**;
- Clang Release targeted matrix: **3/3 passed**;
- GCC TSan `xenon_gpu_coherency_tests`: **passed with no reported race**;
- the broader TSan `xenon_memory_tests` exceeded the available execution window and is
  not counted as a completed TSan pass.


### Phase 15 lazy/range-driven GPU synchronization completion

Brief Phase 15 is closed around request-driven guest-memory synchronization. Validation
confirms:

- untouched device-invalid ranges initialize lazily on their exact first request;
- already device-valid clean ranges generate no repeat transfer;
- adjacent CPU dirty writes and page dirty ranges coalesce;
- requested ranges and page-aligned touched physical ranges are tracked independently of
  dirty ownership;
- production Vulkan/D3D12 source paths no longer synchronize the full physical aperture at
  submission start;
- active vertex buffers and statically known memexport targets request exact upload spans;
- textures, index data, resolve/command streams and presentation/readback request exact
  CPU-visible spans;
- dynamic-address memexport is the explicit full-range fallback;
- the shared planner exposes Texture, VertexBuffer, IndexBuffer, Shader, CommandData,
  MemoryExport, RenderReadback and Unrestricted request classes.

Completion validation:

- GCC Linux x86-64 Release full matrix: **29/29 passed**;
- Debug ASan+UBSan targeted GPU/coherency/resource/texture/CPU-memory matrix: **5/5
  passed**, plus standalone `xenon_memory_tests` pass;
- Clang Release selected matrix: **4/4 passed**;
- GCC TSan `xenon_gpu_coherency_tests` and `xenon_resource_ir_tests`: **2/2 passed**
  with no reported race.


### Phase 16 memory types

Phase 16 closes observable Xbox memory-type behavior in the Memory V2 layer. Validation
now covers canonical NormalCached / WriteCombined / CacheInhibited / Device classification,
mutually exclusive NoCache+WriteCombine rejection, query-visible protection flags, WC/CI
exclusion from the direct aperture, executable memory as an orthogonal property, MMIO as
Device memory, cache-domain publication through the CPU fast path, domain-preserving GPU
upload planning and rollback, conservative CacheInhibited fallback after exact-journal
overflow, and Device-domain controlled physical writes.

Validation results for the completion checkpoint:

- GCC Linux x86-64 Release full matrix: **30/30 passed**;
- Debug ASan+UBSan targeted memory/type/coherency/CPU-memory/host-VM matrix: **5/5 passed**;
- Clang Release memory/type/coherency/CPU-memory matrix: **4/4 passed**;
- GCC TSan `xenon_memory_type_tests` and `xenon_gpu_coherency_tests`: **2/2 passed** with no reported race.

Native host WC/uncached page attributes are intentionally not forced onto individual aliases
of Xenon's shared physical backing on general hosts. The tested policy is semantic
translation (ordering/coherency/access-path policy) until a host backend can provide an
alias-safe native cache-attribute mechanism.


### Phase 17 MMIO fast/slow split

Phase 17 closes the MMIO isolation requirements. Validation now covers hot-page slow-bit
publication, sorted non-overlapping interval registration, logarithmic cold device lookup,
partial-page overlays, adjacent sub-page devices, handler lifetime across catalogue clear,
and byte-visible range fallback without holding the AddressSpace management mutex across
device callbacks.

A dedicated concurrency regression deliberately blocks inside an MMIO callback while a
second thread commits a normal RAM page. The management operation completes before the
handler is released, demonstrating that arbitrary device code no longer executes under the
global memory-management lock. A separate self-clearing handler removes its own catalogue
during dispatch and safely returns through shared snapshot ownership.

Validation results for the completion checkpoint:

- GCC Linux x86-64 Release full matrix: **31/31 passed**;
- Debug ASan+UBSan targeted MMIO/type/coherency/ordering/CPU-memory matrix: **5/5 passed**;
- Clang Release MMIO/type/ordering matrix: **3/3 passed**;
- GCC TSan `xenon_mmio_tests` and `xenon_memory_type_tests`: **2/2 passed** with no
  reported race;
- Release microbenchmark: `load_store_32_be` ~327.6 ns/op and
  `normal_ram_with_256_mmio_devices` ~316.4 ns/op in the same run, while the final cold
  catalogue device dispatched at ~968.0 ns/op. The measurement is not treated as an
  absolute performance guarantee, but it confirms the device catalogue is absent from the
  ordinary RAM path.


### Phase 18 structured fault/protection completion

Phase 18 closes the exception-ready guest fault model. `MemoryFaultInfo` now records the
original request and exact faulting address, access width/kind/reason, region kind, page
state, mapped/committed status, allocation/current protection, physical backing, MMIO
classification and canonical memory type/host policy. Existing `MemoryFault` accessors remain
compatible while `info()` exposes the full structured payload.

New regressions cover free versus reserved/uncommitted pages, read/write/execute protection
faults, cross-page scalar failures preserving the original 4-byte request while naming the
second page that faulted, 32-bit guest-address wrap rejection before memory is touched,
dedicated MMIO without a handler, and partial MMIO scalar accesses producing `MmioWidth`
instead of falling through to RAM. Host page-protection implementation remains isolated in
the host-VM layer.

Validation results for the completion checkpoint:

- GCC Linux x86-64 Release full matrix: **32/32 passed**;
- Debug ASan+UBSan memory/fault/MMIO/CPU-memory matrix: **4/4 passed**;
- Clang Release memory/fault/MMIO/CPU-memory matrix: **4/4 passed**;
- GCC TSan `xenon_memory_fault_tests` and `xenon_mmio_tests`: **2/2 passed** with no
  reported race;
- `git diff --check`: clean.


### Phase 19 executable/self-modifying-code completion

Phase 19 closes native generated-code ownership around the executable-page generation
model. `ExecutableCodeCache` stores source snapshots containing physical-page identity and
executable generation for every page covered by a native translation. Dispatch lookup
returns a native entry only while every stamp still matches, and stale entries are lazily
evicted without adding synchronous callbacks to ordinary stores.

Regressions cover CPU self-modifying writes, physical aliases, XEX 0x8/0x9 aliases,
controlled DMA/GPU writes, `icbi`, Execute->NX->Execute transitions, decommit/recommit
and reset epoch boundaries, multi-page translations, race-safe dynamic recompilation
snapshot rejection, Execute-aware instruction fetch, explicit module-range invalidation
and six concurrent lookup threads racing repeated writes/registration.

Completion validation:

- GCC Linux x86-64 Release full matrix: **33/33 passed**;
- ASan+UBSan: `xenon_executable_code_cache_tests`, `xenon_memory_tests` and
  `xenon_memory_cpu_integration` passed in the Phase 19 hardening build;
- Clang Release: the same three Phase 19/memory integration suites passed;
- GCC TSan `xenon_executable_code_cache_tests`: **passed with no reported race**;
- `git diff --check`: clean at packaging.


## Memory V2 Phase 20/21 closure — 2026-09-19

The original Memory V2 roadmap is now closed at **21/21 phases**. Phase 20 extends
`xenon_memory_v2_benchmarks` with allocation/free, fragmentation, fixed physical aliases,
explicit block copy, CPU→GPU synchronization planning and discrete-mirror upload staging
bandwidth. Reports can be emitted as CSV, JSON (`xenon-memory-v2-benchmark-v1`) or a
human-readable table with platform/architecture/compiler/build/translation metadata. The
format is intentionally shared by Windows x86-64, Linux x86-64, Linux ARM64 and Android
ARM64.

Phase 21 adds `xenon_memory_hardening_tests`, a dedicated randomized/model-based harness
covering fixed A/C/E/7F aliases, dynamic physical ownership/refcounts/reverse mappings,
release/reuse reachability, protection transitions, XEX executable generations, dirty
journal exactness + wrap fallback, LR/SC invalidation, controlled external writes and
six-thread CPU + DMA concurrency. `XENON_MEMORY_FUZZ_SEED` makes failures reproducible;
`XENON_MEMORY_FUZZ_SCALE` allows expensive stress loops to be reduced for sanitizer jobs
without changing the test model.

Closure validation performed on Linux x86-64:

- GCC Release generic test matrix: **34/34 passed** across the initial 1-16 segment and
  resumed 17-34 segment;
- Phase 21 full-scale default seed passed, plus alternate seeds `0x12345678` and
  `0xCAFEBABE`;
- Debug GCC ASan+UBSan: core `xenon_memory_tests` passed full-scale; the hardening target
  passed at 10% sanitizer scale; executable-code-cache and GPU-coherency tests passed;
- Clang 17 Release: hardening, core memory, executable-code-cache and GPU-coherency tests
  passed, and the benchmark executable built and ran;
- GCC TSan: hardening passed at 10% sanitizer scale with `halt_on_error=1` and no reported
  race.

The portable benchmark's `gpu_upload_snapshot_bandwidth_4m` measures the canonical Xenon
Memory side of a discrete upload: exact-range planning plus the safe physical snapshot into
the backend upload staging buffer. Native Vulkan/D3D12 queue-copy timing remains a graphics
backend/hardware performance concern and does not alter Memory V2 semantics.


## Xbox/XDK compatibility closure — 2026-09-19

After the 21-phase Memory V2 roadmap was closed, the complete memory implementation was
audited again against current Xenia memory research, public NT/XDK-shaped memory semantics
and successful Xbox 360 recompilation runtimes. This pass did not replace the V2
architecture; it tightened observable allocation/query/protection behavior before more
runtime systems are built on top of it.

Regression coverage added by this closure verifies:

- the low 64 KiB virtual range is committed but no-access and faults as protection, not as
  an ordinary uncommitted allocation;
- the final 64 KiB of physical RAM can never be returned by the physical allocator and its
  direct alias is no-access;
- default 4 KiB physical allocations cannot escape the E-view into the `0xFFD00000+` MMIO
  window, while 64 KiB A-view allocations may use the higher valid physical range;
- 4 KiB, 64 KiB and 16 MiB physical page classes round and align allocations correctly;
- physical minimum/maximum address bounds are honored;
- physical allocation size, original protection and current protection are queryable;
- read-only/cache-inhibited and write-combined physical policy is observed consistently by
  the A/C/E aliases;
- physical protection changes invalidate executable generations across all aliases;
- freeing by the caller's original unrounded byte count releases the complete page-class
  allocation;
- reserve-only non-fixed virtual allocation can later be committed;
- MEM_NOZERO-style reuse preserves prior contents while ordinary allocation still zeroes;
- adjacent independent allocations with identical state/protection remain separate query
  regions;
- an unaligned two-byte decommit crossing a 4 KiB boundary decommits both touched pages.

The canonical range allocator, compact hot-page table, six-thread reservation monitor,
coherency service and generated PPC fast path remain unchanged in architectural role. The
new Xbox-facing policy is cold metadata and management logic.

Validation after the closure:

- GCC Linux x86-64 Release: complete repository build succeeded and **34/34 CTest suites passed**;
- GCC Debug ASan+UBSan: core memory, structured fault model, executable-code cache and
  GPU coherency targets passed **4/4** with leak detection and halt-on-error enabled;
- Clang Release: core memory, structured fault model, executable-code cache, host-VM and
  generated CPU->Memory integration passed **5/5**.

The optional direct-aperture host abstraction was also tightened in this closure. POSIX
reuses the existing fixed `mmap` implementation. Modern Windows now reserves the aperture
with `VirtualAlloc2(MEM_RESERVE_PLACEHOLDER)` and replaces exact page-sized placeholder
slices with `MapViewOfFile3(MEM_REPLACE_PLACEHOLDER)`, restoring them through
`UnmapViewOfFile2(MEM_PRESERVE_PLACEHOLDER)`. Permanent 0x7F/A/C/E physical aliases remain
on compact translation in Windows placeholder mode so the runtime does not create one
section view per 4 KiB alias page. The common host-VM regression uses the fixed-region API
and the Linux fixed-mapping path was rerun successfully. A native Windows runtime was not
available in this Linux validation environment, so the Windows-specific branch is source-
and API-validated here and is designed to fall back to compact translation automatically
if the placeholder API is unavailable or initialization fails.

## Final adversarial memory audit closure — 2026-09-19

A final comparison against current Xenia/ReXGlue behavior and the production graphics callers
found four additional edge cases. Regression coverage now verifies:

- a one-byte `protect` or `decommit` request in the 64 KiB virtual heap affects the complete
  architectural 64 KiB page, while the hot table remains internally 4 KiB;
- the 0x800 XEX view receives the same 64 KiB management normalization and its 0x900 backing alias
  observes the resulting state;
- protection cannot span adjacent independent virtual reservations;
- physical protection is normalized to the allocation's 4 KiB/64 KiB/16 MiB page class and cannot
  cross the allocation identity boundary;
- free-region queries report a non-zero run up to the next occupied page/heap boundary;
- `copy_physical_range` races aligned CPU stores under the Phase-21 hardening harness without a
  TSan report;
- common GPU helpers accept range snapshots with non-zero physical bases, so backends no longer
  need a direct 512 MiB physical pointer for resolve geometry, indices, textures or presentation.

Production `physical_data()` usage in `src/graphics` is now zero. The Xenos command processor,
indirect shader/constant loads, raw resolve RMW path, Vulkan backend and D3D12 backend all consume
atomic physical snapshots. `physical_data()` remains available only as a read-only diagnostic/
quiescent view for tests and tooling.

Validation after the final closure:

- GCC Linux x86-64 Release: **34/34 CTest suites passed**;
- GCC ASan+UBSan: core memory, hardening at sanitizer scale, fault model, executable cache, GPU
  frontend, resource IR, texture, primitive processing and presentation passed with halt-on-error;
- GCC TSan: Phase-21 hardening (including concurrent CPU store vs physical snapshot) and GPU
  frontend passed with halt-on-error;
- Clang 17 Release: the same focused memory/graphics set passed.

The 0x340000-byte no-access 64 KiB physical reservation still present in Xenia/ReXGlue was also
researched. Upstream continues to mark it as unknown (`// ?`), and no public Xbox/XDK contract was
found that establishes ownership or observable semantics. It is intentionally **not** copied into
Xenon without evidence.

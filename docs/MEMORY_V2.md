# Xenon Memory V2

Status: **in progress** — foundational fast-path, ownership, host-VM and shared CPU/GPU coherency work is active.

Memory V2 keeps the Xbox 360-visible memory model owned by Xenon while replacing baseline implementation mechanisms that are too expensive or too weak for a multi-game native recompilation runtime.

The design rule is:

```text
Xbox 360 memory semantics
          |
          v
canonical Xenon memory model
          |
          v
efficient native host representation
          |
          +--> Windows x86-64
          +--> Linux x86-64
          `--> future Linux/Android ARM64
```

Game modules do not own Xbox page tables, aliases, endian behaviour, reservations, allocation rules, protection semantics or CPU/GPU coherency.

## Specification progress checkpoint

This table tracks the original Memory V2 implementation brief against production
code rather than treating "old tests pass" as completion.

| Brief phase | Status | Current state |
| --- | --- | --- |
| 1. Audit and design | Substantially complete | V1 hot paths, metadata costs, ownership, generated access and GPU coherency boundaries are documented and representative generated AOT output was inspected. |
| 2. Hot/cold architecture | Implemented foundation | `AddressSpace` owns management; generated PPC uses concrete `MemoryAccessContext` for ordinary RAM. |
| 3. Compact page translation | Implemented foundation | Atomic 64-bit hot table covers the 32-bit guest page space; large management `Page` state remains cold. |
| 4. Remove global access serialization | Partial / strong | Common generated RAM is lock-free with respect to the global management mutex; six-thread stress exists; read-side reclamation now prevents physical-page ABA. Final PPC ordering/atomic semantics remain. |
| 5. Host VM backend | Implemented foundation | Windows, POSIX/Linux and fallback host-VM implementations are separated from Xbox policy. |
| 6. Direct guest aperture | Not started | Compact translation remains the portable baseline. |
| 7. Physical allocator | Not started | Allocation is still linear-scan; replacement and fragmentation tests are next. |
| 8. Physical ownership/aliases | Partial / strong | Mapping refcounts, pending-free ownership and retired-page quiescence are implemented; broader reverse-mapping/model tests remain. |
| 9. PPC reservation monitor | Not started final design | Correct existing 128-byte generation model retained pending exact six-thread LR/SC redesign. |
| 10. PPC memory ordering | Not started | Dedicated `sync`/`lwsync`/`eieio`/`isync` host model and litmus tests remain. |
| 11. Block/range access | Partial | `fill`, `zero`, `copy` and `dcbz` use range-oriented work; copy still uses a proportional temporary snapshot and string/vector-partial operations remain scalar. |
| 12. Dirty tracking/observers | Implemented foundation | Xenon-owned page epochs drive consumers; synchronous physical-write observers have been removed from the scalar path entirely. |
| 13. Safe external writes | Implemented foundation | Raw physical backing is read-only externally; `write_physical`, `fill_physical` and RAII `PhysicalWriteSpan` automatically publish reservation/coherency changes. |
| 14. Shared CPU/GPU coherency | Partial / strong | `GuestMemoryCoherency` owns dirty state for Vulkan, D3D12 and texture tracking; backend/platform validation remains. |
| 15. Lazy/range GPU sync | Partial / strong | Mirrors consume coalesced dirty ranges after initial synchronization; unrestricted-workload/future UMA policy remains. |
| 16. Memory types | Early foundation | No-cache/write-combine bits exist in hot metadata; observable ordering/mapping/device semantics remain. |
| 17. MMIO fast/slow split | Partial / strong | MMIO pages force slow dispatch and do not burden normal RAM; cold MMIO lookup/handler locking still needs refinement. |
| 18. Fault/protection model | Partial | `MemoryFault` carries address/width/access/reason; richer mapping/protection/page-state records and kernel-exception translation remain. |
| 19. Executable/SMC | Early foundation | Execute permission and `icbi` callback behavior exist; executable generations/native-code invalidation remain. |
| 20. Benchmarking | Not started | Dedicated Memory V2 Release benchmark suite remains. |
| 21. Fuzzing/hardening | Partial | Targeted ASan/UBSan passes and concurrency regressions exist; model fuzzing, TSan and wider compiler/platform matrix remain. |

## 1. V1 audit

The Memory V2 work started by auditing `AddressSpace`, generated PPC memory access, reservations, MMIO, XEX mappings, physical allocation, the GPU guest-memory mirrors and the existing CPU/memory/graphics tests.

The main V1 hot-path costs were:

- every `AddressSpace` scalar/vector read and write acquired the same `std::recursive_mutex`;
- generated AOT code issued ordinary loads/stores through virtual `MemoryPort` calls;
- virtual access depended on the large management `Page` representation rather than a dedicated compact translation entry;
- physical writes updated reservation generations and synchronously copied/executed arbitrary physical-write observer callbacks;
- Vulkan and D3D12 each maintained backend-owned guest-memory dirty tracking;
- texture invalidation was also fed by synchronous write callbacks;
- `fill` and `copy` were implemented as repeated scalar/byte operations;
- physical allocation used linear page scans;
- `free_physical` scanned the entire 32-bit guest page table to discover live mappings;
- host VM primitives (`mmap`/`VirtualAlloc` family) were embedded in the Xbox-facing address-space implementation;
- writable raw physical backing was exposed to production callers, making it possible to bypass reservation/coherency bookkeeping; this was removed in the second V2 slice.

The audit also confirmed that the existing Xbox address map, protection checks, XEX aliases, physical aliases and MMIO behaviour are valuable semantics and should be retained rather than replaced for architectural neatness.

### 1.1 Baseline metadata cost

With 4 KiB guest pages, the 32-bit address space contains 1,048,576 guest pages.

At the start of V2 the principal always-resident metadata was approximately:

| Structure | Representation | Approximate size |
| --- | --- | ---: |
| V1 guest `Page` metadata | 1,048,576 x 20 bytes | 20 MiB |
| reservation generations | 4,194,304 x `atomic<uint32_t>` | 16 MiB |
| physical-page ownership | 131,072 x byte | 128 KiB |

V2 currently adds the following dedicated runtime metadata while the remaining V1 structures are progressively replaced:

| Structure | Representation | Approximate size |
| --- | --- | ---: |
| hot translation table | 1,048,576 x `atomic<uint64_t>` | 8 MiB |
| physical dirty epochs | 131,072 x `atomic<uint64_t>` | 1 MiB |
| physical mapping refcounts | 131,072 x `uint32_t` | 512 KiB |

The 16 MiB reservation-generation table is intentionally listed as legacy V1 cost: its replacement requires a correctness-first Xenon/PPC reservation-monitor redesign rather than a blind size optimization.

## 2. Xbox address-space model

The existing public Xbox 360 ranges remain authoritative:

| Guest range | Allocation page | Xenon model |
| --- | ---: | --- |
| `00000000-3FFFFFFF` | 4 KiB | normal virtual |
| `40000000-7EFFFFFF` | 64 KiB | large-page virtual |
| `7F000000-7FFFFFFF` | 64 KiB | GPU writeback/XPS physical view |
| `80000000-8FFFFFFF` | 64 KiB | XEX view |
| `90000000-9FFFFFFF` | 4 KiB | alias of XEX view |
| `A0000000-BFFFFFFF` | 64 KiB | physical RAM view |
| `C0000000-DFFFFFFF` | 16 MiB | physical RAM view |
| `E0000000-FFCFFFFF` | 4 KiB | physical RAM view with 4 KiB offset |
| `FFD00000-FFFFFFFF` | device-defined | MMIO/device space |

The XEX `0x800...` and `0x900...` windows share physical backing. The `0x7F...`, `0xA...`, `0xC...` and `0xE...` views resolve into the same 512 MiB unified physical RAM where their ranges overlap.

Memory V2 does not move these semantics into game modules.

## 3. Hot/cold architecture

`memory::AddressSpace` remains the authority for cold management operations:

- reserve/commit/decommit/release;
- allocation and mapping creation;
- protection changes and queries;
- physical allocation ownership;
- XEX mappings;
- MMIO registration;
- range/region metadata.

Ordinary generated PPC memory operations now acquire a concrete `cpu::MemoryAccessContext` once on entry to a generated function/block and use its non-virtual load/store methods.

```text
RECOMPILED PPC
      |
      v
MemoryAccessContext
      |
      +--------------------------+
      |                          |
      v                          v
FAST RAM PATH               SLOW PATH
      |                          |
64-bit hot page entry       MemoryPort / AddressSpace
      |                      MMIO, faults, unusual cases
      v
physical backing
```

The generated-function ABI still accepts `MemoryPort&` for compatibility. This is deliberate: it avoids an unnecessary CPU ABI upheaval while still removing virtual dispatch from repeated ordinary loads/stores. A generated function obtains `memory.access_context()` once and performs common memory operations on the concrete context.

Reference/test `MemoryPort` implementations automatically receive a slow-only context unless they provide a fast view.

## 4. Compact page translation

Memory V2 maintains a dedicated atomic 64-bit hot entry for every 4 KiB guest page. The entry contains only access-critical state:

- mapped state;
- physical page index;
- read/write/execute bits;
- slow-path/MMIO flag;
- no-cache flag;
- write-combine flag.

The larger V1 `Page` object remains cold management/query metadata while migration is in progress.

For an ordinary same-page RAM access the intended path is now effectively:

```text
guest address
    |
    +--> page index = address >> 12
    |
    +--> atomic 64-bit hot entry
    |
    +--> physical page + page offset
    |
    `--> native host load/store + endian conversion
```

Normal RAM no longer scans MMIO collections or `RegionDescriptor` structures. Pages covered by MMIO are published with the slow flag, causing the exceptional access to fall back to the authoritative dispatcher.

Hot page entries are release-published after mapping/protection changes and acquire-read by fast accesses.

### 4.1 Mapping lifetime and read-side quiescence

Atomic publication makes page-table state race-free as data, but publication alone is not enough: a CPU thread can load a hot entry immediately before another thread unpublishes the mapping. Reusing that physical page immediately would create an ABA hazard where the old access could reach a different allocation.

`MemoryAccessContext` now carries a lightweight read-side lifetime guard. Creating a production fast context increments a shared active-reader counter and destruction decrements it. This happens once per generated context, not once per scalar load/store.

Physical pages released from virtual/explicit ownership enter a **retired** state instead of immediately returning to the allocator. Retired pages are not recycled while any pre-existing fast context is alive. The cold allocator reclaims retired pages only after the active-reader count reaches zero, at which point it discards the backing and publishes the corresponding reservation/coherency change before reuse.

Fast accesses still load the atomic hot page entry on every access, so the guard is a reclamation/lifetime mechanism rather than a cached-TLB mechanism. New contexts after an unpublish observe the new mapping state; the quarantine exists to protect the narrow load-entry-to-host-access race of older contexts.

## 5. Normal RAM fast path

For common aligned same-page RAM operations, `MemoryAccessContext` currently provides:

- 8/16/32/64-bit loads and stores;
- 128-bit/vector loads and stores;
- Xbox big-endian conversion;
- explicit little-endian/byte-reversed variants;
- access permission checks from the hot entry;
- range-level reservation/coherency bookkeeping on writes;
- direct fallback for MMIO, faults, cross-page and unusual accesses.

Naturally aligned scalar accesses use `std::atomic_ref` with relaxed ordering for the host memory operation. PPC ordering is supplied separately by the PPC memory-order model; it is not encoded by making every RAM access sequentially consistent.

The normal generated path has no:

- `recursive_mutex` acquisition;
- page/region scan;
- heap allocation;
- per-access virtual load/store dispatch;
- unconditional arbitrary callback execution.

The slow `AddressSpace` methods remain the reference/exception path and still use the existing locking model while cold-path migration continues.

## 6. Physical memory and aliases

Physical RAM remains a single 512 MiB backing store with 4 KiB physical pages.

Memory V2 now separates **physical ownership** from **mapping references**. Physical pages track ownership state plus mapping refcount instead of treating the mapping that created a page as proof that the page can be immediately returned to the allocator.

Ownership states currently distinguish:

- free;
- system/reserved;
- anonymous virtual backing;
- explicit physical allocation;
- anonymous backing whose owner has been released but which is still held alive by aliases.

This fixes a critical lifetime invariant:

> A physical page cannot return to the free allocator while any live guest mapping can still reach it.

If an anonymous allocation is released while another alias remains, the page enters pending-free ownership. The final alias release retires the physical page. Explicit physical allocations remain owned until `free_physical`, and `free_physical` refuses to release pages with non-zero mapping refs.

This removes the old full 1,048,576-page scan from `free_physical`.

The physical allocator itself is still the V1 linear scanner and remains scheduled for replacement with a range-oriented allocator.

## 7. Block/range operations

`fill` and `copy` no longer implement common RAM work as repeated scalar stores.

The current V2 implementation:

- translates/chunks by page boundary;
- uses `memset` for common RAM fill chunks;
- uses page-chunk `memcpy` for common RAM snapshots/writes;
- performs write bookkeeping once per translated range chunk;
- falls back to precise byte operations for MMIO/unusual pages.

`copy` currently snapshots the source before writing the destination so that existing arbitrary alias/overlap semantics are preserved. This still uses a temporary allocation proportional to the copy and is therefore not the final large-transfer implementation. The next block-operation iteration should use bounded chunking/scratch storage while preserving overlap and alias correctness.

`dcbz`/cache-block zero inherits the range-based fill path.

## 8. Reservations

The existing reservation implementation is retained for correctness during the first V2 slice:

- reservation identity is based on physical memory rather than guest virtual address;
- the current implementation uses 128-byte physical granules;
- writes through aliases invalidate matching granule generations;
- external writes routed through the memory notification path also invalidate reservations.

The existing generation table costs approximately 16 MiB and is not the desired final V2 design. Replacing it requires an exact Xenon/PowerPC LR/SC audit and tests for all six Xenon hardware threads, virtual aliases, DMA/GPU writes and neighboring granules.

Until that work is complete, the generation implementation is preferred over an unproven compact replacement.

## 9. PPC memory ordering

A dedicated V2 PPC ordering pass is still pending.

The final design must explicitly model the observable semantics of:

- `sync`;
- `lwsync`;
- `eieio`;
- `isync`;
- LR/SC operations;
- normal cached RAM;
- device/cache-inhibited/write-combined memory.

It must be correct on both x86-64 and weaker-memory ARM64 hosts. Memory V2 deliberately does not treat `seq_cst` on every ordinary load/store as a substitute for that model.

## 10. MMIO fast/slow split

MMIO ranges are represented in the hot translation table as slow pages. Ordinary RAM therefore performs one hot page lookup and never scans the registered MMIO range collection.

A slow access enters `AddressSpace`, where the existing width-aware MMIO callback behavior is preserved.

The slow MMIO dispatcher itself still uses the current range collection. A page-indexed/interval device dispatcher is a later cold-path optimization; it is no longer on the normal RAM path.

Arbitrary device handlers are not invoked by the new fast RAM path.

## 11. Shared CPU/GPU coherency

Memory V2 introduces `memory::GuestMemoryCoherency` as a backend-neutral Xenon service.

It owns per-physical-page write epochs and produces coalesced dirty physical ranges. Writers publish changes through Xenon Memory rather than maintaining Vulkan- and D3D12-specific CPU dirty semantics.

Current consumers:

- Vulkan guest-memory mirror;
- D3D12 guest-memory mirror;
- texture dirty tracking.

The mirrors now remember a synchronized epoch, ask Xenon Memory for coalesced changed ranges and upload only those physical ranges after initial synchronization. A first synchronization may legitimately cover the complete guest RAM because reset publishes the initial memory state as dirty; subsequent synchronization is range-driven.

The epoch snapshot has an active-writer stability boundary. A GPU consumer does not advance its synchronized epoch past a CPU writer whose dirty-page publication is incomplete. If a page receives an even newer write after the consumer's boundary, the page can be conservatively included now and remains logically dirty for the following epoch as well, preventing a later page-epoch overwrite from hiding an earlier unsynchronized write.

This moves Xbox guest-memory coherency policy out of the Vulkan/D3D12 implementations. Native backends execute synchronization/upload plans; they do not define independent Xbox memory semantics.

## 12. Write observers and dirty tracking

The legacy synchronous physical-write observer API has now been removed. There is no observer-active branch, callback thunk, callback-vector copy or arbitrary callback execution in the ordinary `MemoryAccessContext` store path.

CPU and controlled external writers publish dirty state directly into fixed Xenon-owned coherency metadata. Vulkan/D3D12 mirrors and texture tracking consume that state asynchronously/range-wise rather than being invoked by every scalar write.

Instruction-cache/native-code invalidation remains a separate semantic concern and is still pending the executable-page generation work described below; it must not reintroduce arbitrary per-store observer dispatch.

## 13. External/DMA writes

Production code no longer receives unrestricted mutable physical backing. `physical_data()` is read-only outside the memory implementation.

Memory V2 now provides controlled external-write operations:

- `write_physical` for copying a known byte range into physical RAM;
- `fill_physical` for bounded physical fills;
- move-only RAII `PhysicalWriteSpan` for native subsystems that genuinely need a direct bounded mutable span.

`PhysicalWriteSpan` publishes its complete declared range automatically when the scope ends. The current completion path invalidates LR/SC reservation generations and publishes CPU/GPU dirty/coherency state. The Xenos PM4 physical-write path and native Vulkan/D3D12 resolve writeback paths use these controlled APIs instead of mutating `physical_data()` and making a second notification call.

This closes the bookkeeping-bypass API for current production callers. Executable-page generation changes will be added to the same completion path when the executable/SMC subsystem lands, so external writers will not need a second contract later.

The remaining concurrency caveat is broader than this API: exact C++ memory-model behavior for simultaneous differently-sized CPU/DMA accesses is part of the pending multi-threaded memory-order/quiescence hardening and must be validated on x86-64 and ARM64.

## 14. Host virtual-memory abstraction

Host VM calls have been moved out of `AddressSpace` into `memory::host_vm`.

The current abstraction exposes:

- page size;
- allocation granularity;
- reserve;
- commit;
- decommit;
- protect;
- discard;
- release.

Implementations:

- Windows: `VirtualAlloc`, `VirtualFree`, `VirtualProtect`;
- POSIX/Linux: `mmap`, `mprotect`, `madvise`, `munmap`;
- fallback implementation for unsupported hosts.

`AddressSpace` now asks this abstraction for physical backing rather than containing Win32/POSIX calls itself. Android/Linux ARM64 can share the POSIX family while retaining Android-specific policy behind the same boundary if required later.

Host VM protection is not yet used as the authoritative Xbox protection mechanism; Xbox permissions remain explicit Xenon page metadata. This avoids accidentally turning a host OS mechanism into Xbox-visible policy.

## 15. Direct guest aperture

An optional direct guest virtual-address aperture remains research work.

Memory V2 does **not** depend on it. The compact translation table is the portable baseline and must continue to work on Windows, Linux and future ARM64/Android hosts.

If a direct aperture is later proven worthwhile, both modes must expose identical Xbox-visible semantics and the runtime/build should select the appropriate representation without leaking host policy into game modules.

## 16. Executable/self-modifying memory

Execute permission remains represented, and `fetch32_be`/`icbi` behavior is preserved, but executable-page generations are not yet a complete V2 subsystem.

Required follow-up includes:

- executable physical/virtual page generations;
- generation updates on every write-capable path;
- invalidation of generated/native code associated with changed pages;
- protection-transition handling;
- `icbi` integration;
- dynamic guest code translation without introducing an interpreter requirement.

## 17. Memory types

No-cache and write-combine flags are carried in the hot entry, preserving the information needed by the access layer.

Their full observable effects on ordering, host mapping policy and GPU/device visibility are not yet complete. Those semantics must be defined together with the PPC ordering and device-memory work rather than implemented as unrelated backend flags.

## 18. Fault model

The existing `MemoryFault` path remains the slow/reference fault mechanism. Fast accesses fall back rather than inventing a second fault policy.

A later hardening pass should expand this into a structured fault record suitable for eventual Xbox kernel exception translation, including guest address, width, access type, mapping/protection state and reason.

Any host SEH/signal/page-fault acceleration must remain contained inside the host VM/backend layer.

## 19. Validation added with the first V2 slice

The current tests include coverage for:

- direct `MemoryAccessContext` RAM reads/writes;
- fast-context fallback to MMIO;
- generated PPC load/store using the fast context;
- six concurrent normal-RAM threads using separate fast contexts;
- coherency epoch changes on writes;
- range fill across a page boundary;
- overlapping copy/snapshot behavior;
- range-level notifications across physically discontinuous page mappings;
- anonymous physical owner release while an alias remains live;
- prevention of physical-page reuse until the final alias is released;
- reuse after the final alias is released;
- retirement of unpublished physical pages while an older fast access context
  remains alive, followed by reuse only after read-side quiescence;
- coherency-driven texture dirty detection;
- controlled physical writes automatically invalidating reservations and publishing coherency state;
- scoped physical write spans automatically publishing their declared range at scope completion;
- rejection of out-of-range controlled physical writes;
- existing CPU/memory/graphics regression behavior.

The generated AOT output has also been inspected: representative `lwz`/`stw` paths acquire `memory.access_context()` and emit `memory_access.read32_be` / `memory_access.write32_be`, while reservations and other uncommon operations still use the slower `MemoryPort` operations.

## 20. Current validation environment

Current development validation is Linux x86-64 Release.

The generic CPU, memory and Xenos graphics suites build in this environment. The Vulkan native backend is disabled when the Vulkan SDK is unavailable, and D3D12 requires Windows; therefore native backend compilation must also be repeated on the relevant SDK/platform before this V2 slice is considered cross-platform validated.

Windows host-VM code likewise requires Windows compilation/runtime validation.

## 21. Remaining dependency order

The next Memory V2 work should proceed in this order unless a discovered correctness dependency changes it:

1. replace the linear physical allocator with a range-oriented allocator and add fragmentation/model tests;
2. complete reverse-mapping/ownership invariants where required by executable/coherency consumers;
3. research and implement the final six-thread Xenon/PPC reservation monitor;
4. implement/test the canonical PPC memory-order model for x86-64 and ARM64;
5. complete memory-type semantics;
6. add executable-page generations and self-modifying-code integration;
7. research the optional direct guest aperture;
8. add dedicated Release memory benchmarks;
9. add randomized/model-based invariant tests plus ASan/UBSan/TSan validation;
10. validate native Vulkan, D3D12, Windows VM and later Linux/ARM64 builds.

## 22. Definition-of-done status

The following V2 goals are already substantially represented in production code:

- compact dedicated hot translation metadata;
- non-virtual generated ordinary load/store path;
- no global `AddressSpace` mutex for common generated RAM accesses;
- no RegionDescriptor/MMIO scan for common RAM accesses;
- no heap allocation on scalar fast accesses;
- backend-neutral CPU/GPU dirty tracking;
- no synchronous physical-write observer/callback branch on scalar stores;
- controlled external/DMA/GPU write APIs with automatic reservation/coherency completion;
- read-only raw physical backing outside the memory implementation;
- read-side quiescence preventing unpublished physical pages from being recycled into ABA/stale translations;
- range-based common fill/copy operations;
- physical alias lifetime/refcount protection;
- host VM implementation separated from the canonical Xbox address-space model;
- six-thread normal-RAM stress coverage.

Memory V2 is **not complete**. In particular, the reservation monitor, PPC ordering, allocator replacement, executable generations, full memory-type behavior, benchmarking, fuzz/model testing, sanitizer matrix and native backend/platform validation remain required before this document can be marked complete.

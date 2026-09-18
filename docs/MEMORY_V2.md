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
code rather than treating "old tests pass" as completion. `Complete` is now
deliberately strict for every phase: every requirement in that phase must be
implemented, production callers migrated, regression/stress coverage added and the
result documented before work advances. Earlier foundation work remains explicitly
partial until its outstanding requirements are closed.

| Brief phase | Status | Current state |
| --- | --- | --- |
| 1. Audit and design | **Complete** | V1 hot paths, locks/dispatch/callbacks, metadata costs, raw writers, ownership, generated access, GPU coherency boundaries and representative generated AOT output were audited and documented before the production migration. |
| 2. Hot/cold architecture | **Complete** | `AddressSpace` owns cold management; generated PPC acquires concrete `MemoryAccessContext` once and ordinary RAM uses non-virtual/no-allocation fast access while exceptional cases fall back to `MemoryPort`. |
| 3. Compact page translation | **Complete** | Atomic 64-bit hot entries cover all 1,048,576 guest pages with physical-page identity, permissions, slow/MMIO and memory-type flags; large allocation/query metadata remains cold. |
| 4. Remove global access serialization | **Complete** | Common generated and direct `AddressSpace` RAM scalar/range accesses bypass the global management mutex; hot mappings are atomically published, retired physical pages use read-side quiescence, CPU accesses use atomic host operations, controlled DMA/GPU writes use atomic transfer helpers, and six-thread + mapping-churn + external-writer stress is TSan-clean. |
| 5. Host VM backend | Partial / strong | Windows, POSIX/Linux and fallback implementations are separated from Xbox policy; Windows reserve/commit/protect/decommit/discard/release lifecycle is now regression-tested from a clean native build. Equivalent current Linux lifecycle validation remains before strict closure. |
| 6. Direct guest aperture | Not started | Compact translation remains the portable baseline. |
| 7. Physical allocator | **Complete** | Coalescing free/retired ranges replace page-by-page search; 4 KiB granularity, contiguous aligned runs, top-down/bottom-up placement, deterministic reuse and randomized fragmentation/coalescence tests are implemented. |
| 8. Physical ownership/aliases | **Complete** | Mapping refcounts are cross-checked against sparse cold reverse mappings; pending-free/retired ownership, overlapping views, fixed/XEX/GPU-visible aliases, release-order/reallocation behavior and randomized alias-model tests are implemented without whole-space free scans. |
| 9. PPC reservation monitor | **Complete** | Six live reservation slots plus a ~512 KiB sticky granule bitmap replace the 16 MiB generation table; physical identity, 32/64-bit LR/SC, alias conflicts, DMA/external invalidation, neighboring granules, replacement and six-thread contention are covered. |
| 10. PPC memory ordering | Partial / strong | Canonical `sync`/`lwsync`/`eieio`/`isync` ordering, x86-64/ARM64 lowering and threaded litmus coverage exist. This phase remains open until device/write-combined/cache-inhibited behavior and instruction synchronization are integrated with phases 16/19 rather than merely represented. |
| 11. Block/range access | **Complete** | `read/write/fill`, zero, copy/move, `dcbz`, PPC string operations, VMX partial transfers and current external/DMA-style physical writers use page/range operations; common linear copies use overlap-safe atomic word bursts, nonlinear non-overlap uses bounded scratch, and only pathological MMIO/alias cases use snapshot slow paths. |
| 12. Dirty tracking/observers | **Complete** | Xenon-owned page epochs plus a bounded lock-free exact-range journal drive consumers; synchronous physical-write observers have been removed from the scalar path entirely. Journal wrap safely falls back to conservative page ranges. |
| 13. Safe external writes | **Complete** | Raw physical backing is read-only externally; `write_physical`, `fill_physical` and RAII `PhysicalWriteSpan` automatically publish reservation, coherency and executable-generation changes. Production Xenos/Vulkan/D3D12 writers use these controlled paths. |
| 14. Shared CPU/GPU coherency | Partial / strong | `GuestMemoryCoherency` owns dirty state for Vulkan, D3D12 and texture tracking. Windows Vulkan/D3D12 byte-ownership merging is validated; Linux backend and future UMA policy validation remain. |
| 15. Lazy/range GPU sync | Partial / strong | Mirrors consume exact dirty writes while journal history is available and conservatively coalesced pages after wrap. Initial/full-range and genuinely unrestricted fallback remain; requested-range/device-valid planning and future UMA policy remain. |
| 16. Memory types | Early foundation | No-cache/write-combine bits exist in hot metadata; observable ordering/mapping/device semantics remain. |
| 17. MMIO fast/slow split | Partial / strong | MMIO pages force slow dispatch and do not burden normal RAM; cold MMIO lookup/handler locking still needs refinement. |
| 18. Fault/protection model | Partial | `MemoryFault` carries address/width/access/reason; richer mapping/protection/page-state records and kernel-exception translation remain. |
| 19. Executable/SMC | Partial / strong | Sticky physical executable-page generations now advance on CPU stores, controlled external writes and `icbi`, including protection transitions. Native code-cache ownership/eviction still needs to consume the generation API. |
| 20. Benchmarking | Implemented foundation | A dedicated Release benchmark target covers scalar/vector fast access, endian conversion, random translation, cross-page fallback, range writes/zero, MMIO, reservations, six-thread contention and dirty-range coalescing. Allocation/fragmentation and hardware GPU upload reporting remain. |
| 21. Fuzzing/hardening | Partial / stronger | Targeted ASan/UBSan and TSan passes, concurrency regressions, physical-allocation fragmentation stress and randomized alias/ownership model testing exist; broader fuzzing and wider platform/compiler coverage remain. |

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
- physical allocation used linear page scans; this was replaced in the third V2 slice with a coalescing range index;
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
| reservation seen bitmap | 4,194,304 bits | 512 KiB |
| active reservation slots | 6 x `atomic<uint64_t>` | 48 bytes |

The V1 16 MiB reservation-generation table has now been removed. The V2 monitor uses an approximately 512 KiB sticky bitmap only as a hot-path hint plus six exact live reservation slots, reducing always-resident reservation metadata by roughly 32x while retaining 128-byte physical-granule invalidation.

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

### 4.2 Phase 4 concurrency completion

Phase 4 is now closed. Ordinary aligned RAM no longer depends on the global
`AddressSpace` management mutex whether it is reached from generated AOT code or
through the public `AddressSpace` `MemoryPort` surface. Direct scalar/vector and
range calls probe the atomic hot translation first and enter the cold locked resolver
only for MMIO, faults, protection failures, cross-page/unaligned exceptional cases or
other slow mappings. Common linear copy/move planning likewise uses hot physical
resolution rather than scanning cold mapping structures under the mutex.

Host accesses to guest RAM use `std::atomic_ref`-backed relaxed scalar/word operations.
The PPC ordering layer supplies ordering separately. Slow unaligned RAM accesses use
atomic byte operations rather than plain `memcpy`/dereference, and controlled
physical/DMA/GPU writes no longer expose a mutable raw span: `PhysicalWriteSpan`
performs bounded atomic `write`/`fill` operations and publishes reservation/coherency
completion once at scope exit. Physical range reads used by controlled DMA paths use
the matching atomic read helper.

Concurrency validation combines six direct `AddressSpace` CPU threads, a concurrent
controlled physical writer and reader on one aligned 64-bit physical word, and repeated
commit/release churn on an unrelated guest mapping. The same test runs under TSan.
Together with the reservation monitor, explicit PPC barrier model, atomic hot-page
publication and read-side reclamation, this satisfies the Phase 4 requirement without
moving memory-management locking onto the normal RAM path. Detailed device-memory
ordering and backend transfer policy remain owned by Phases 10/14/16 rather than
reopening Phase 4.

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

Direct `AddressSpace` scalar and range methods now also attempt the same hot translation path before entering the reference/exception resolver. The management mutex remains confined to cold mapping/MMIO/fault operations rather than ordinary aligned RAM.

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

### 6.1 Reverse mappings and ownership invariants

Memory V2 now keeps a **sparse cold reverse map** from physical pages to the dynamic guest pages currently mapped to them. The reverse map is deliberately not part of the hot translation entry and is never consulted by ordinary generated loads/stores. Heap-backed reverse-map maintenance occurs only on cold mapping-management operations.

This provides two independent ownership signals:

- `physical_mapping_refs_` remains the compact O(1) physical mapping count;
- the reverse map records exactly which virtual/XEX guest pages contribute those references.

Adding or removing a dynamic mapping updates both structures atomically under the management lock, and an inconsistency is treated as an internal logic error rather than silently tolerated. `dynamic_guest_aliases_for_physical` can enumerate aliases for a specific physical location without scanning the 1,048,576-entry guest page table. Fixed A/C/E/7F physical views are architectural aliases resolved directly by address formula; they are intentionally not ownership references and therefore are not inserted into the dynamic reverse map.

`validate_invariants()` provides an explicit cold-path model check used by stress tests. It verifies, among other things:

- physical mapping counts exactly equal reverse-map cardinality;
- every reverse-map entry points at a committed guest page with matching physical backing;
- every committed Virtual/XEX page has a corresponding reverse-map entry;
- free physical pages have no live mappings and are present in the free-range allocator;
- retired pages have no live mappings and are present only in the retired-range index;
- anonymous pending-free pages still have at least one live alias;
- XEX 0x8/0x9 window metadata stays symmetric;
- non-committed guest pages cannot retain stale physical backing or explicit-mapping state.

The reverse-map pass also exposed and fixed a stale-state bug in `decommit`: an explicitly mapped virtual page retained `explicit_physical_mapping` after its mapping had been removed. Recommitting that reserved VA with anonymous RAM could then make a later release skip anonymous backing reclamation. Decommit now clears mapping-specific ownership state before recommit.

### 6.2 Physical range allocator

Memory V2 now maintains a cold coalescing free-range index over the 4 KiB physical-page space. The byte-per-page ownership array remains the authoritative ownership model; the range allocator is deliberately only the efficient search/reuse structure beneath it.

The allocator supports:

- 4 KiB granularity;
- contiguous physical runs;
- arbitrary power-of-two page alignment accepted by the Xbox-facing allocation API;
- deterministic bottom-up and top-down selection;
- O(number-of-free-ranges) candidate search rather than O(number-of-physical-pages) scanning;
- O(log n) claim/release/coalescing through ordered range metadata;
- explicit claiming of previously free physical frames when `map_virtual_to_physical` establishes ownership;
- coalesced retired ranges used by the read-side quiescence mechanism.

The first 16 MiB GPU writeback/XPS reservation never enters the free-range index. Anonymous backing and explicit physical allocations consume from the same free-range authority, while ownership/refcount state continues to determine when a frame is legally releasable.

Retired pages are also range-indexed. Allocation no longer rescans all 131,072 physical pages merely to discover whether something can be reclaimed. Once fast readers quiesce, only recorded retired ranges are discarded, returned to the free index and coalesced.

Validation includes bottom-up/top-down placement, alignment, adjacent-hole coalescing, allocator exclusion for directly mapped free physical frames, and a deterministic 2,000-operation fragmentation stress that mixes allocation size, power-of-two alignment, allocation direction and frees.

A more complex buddy hybrid is not currently justified: the ordered range allocator gives the required Xbox semantics, deterministic contiguous allocation and fragmentation recovery while keeping allocator machinery entirely off the normal RAM access path. This decision can be revisited using the dedicated Memory V2 benchmark suite in Phase 20.

## 7. Block/range operations

Phase 11 is complete. Large guest operations no longer use repeated scalar memory
calls on ordinary RAM. `MemoryAccessContext` exposes `read_bytes`, `write_bytes`
and `fill_bytes`; each operation translates/chunks at 4 KiB guest-page boundaries,
uses native/word-burst host work inside the resolved chunk, and performs reservation
and coherency bookkeeping once per physical range chunk rather than once per byte.
MMIO, faults and other exceptional pages retain the precise slow/reference path.

`AddressSpace::zero` and `fill` use this range interface directly. `copy` and `move`
share an overlap-safe implementation with three deliberate tiers:

1. **Linear RAM source + destination:** one overlap-safe atomic word-burst move over
   physical backing with one destination range publication. This retains memmove
   semantics while avoiding C++ data races with concurrent guest CPU/DMA accesses.
2. **Non-linear RAM with no physical-page overlap:** page/range translation streams
   through a fixed 64 KiB stack scratch buffer, so temporary memory remains bounded
   regardless of transfer size.
3. **MMIO or pathological non-linear physical alias overlap:** a cold snapshot slow
   path preserves the pre-V2 all-reads-before-writes semantics and MMIO side effects.
   The proportional allocation is therefore isolated from ordinary RAM rather than
   paid by every block copy.

Generated `dcbz` now emits directly to `MemoryAccessContext::zero_cache_block`. PPC
string loads/stores (`lswi`/`lswx`/`stswi`/`stswx`) use fixed stack staging and range
reads/writes rather than scalar guest calls, including page-crossing cases. VMX
left/right partial loads and stores use the same range interface; vector element
operations intentionally remain scalar because they architecturally transfer only a
single 1/2/4-byte element.

The external/physical transfer side is range-based as well: controlled
`write_physical`/`fill_physical`/`PhysicalWriteSpan` operations use atomic bulk
copy/fill helpers while automatically batching reservation/coherency completion, Xenos
`PM4_MEM_WRITE` acquires one span for the complete packet payload, and resolve
writeback already commits through physical spans. Read-only DMA index consumption
operates directly on the physical-memory span and does not manufacture scalar guest
transactions. No currently implemented production bulk-transfer path requires one
reservation/coherency notification per byte.

Regression coverage includes linear overlapping memmove-style copies, discontiguous non-overlap
copy, reversed-page pathological alias overlap, MMIO range copy, page-crossing range
read/write/fill, generated PPC string transfers, generated VMX partial transfers and
a multi-dword Xenos memory write that publishes one physical range transaction.

## 8. Reservations

Memory V2 now uses a compact Xenon reservation monitor rather than the V1 per-granule generation table. The observable model remains physical:

- reservation identity is keyed by the resolved physical word/doubleword, not its guest virtual alias;
- Xenon uses a 128-byte physical reservation granule for conflict invalidation;
- a store anywhere in that granule invalidates other active reservations on it;
- `lwarx`/`ldarx` retain the exact resolved physical address and access width so `stwcx.`/`stdcx.` validate the reserved storage while still allowing another guest alias of that same physical storage;
- a new generated load-reserve explicitly cancels the prior reservation held in that `CpuState`;
- every conditional-store attempt consumes or loses its reservation;
- controlled DMA/GPU/external writes invalidate reservations before mutable bytes are exposed.

The monitor consists of six atomic 64-bit live slots, matching Xenon's maximum six simultaneously executing hardware threads, plus a sticky bit for each 128-byte physical granule. The bitmap is a performance hint only: an ordinary write to a granule whose bit has never been set skips the six-slot scan. Once a granule has hosted a reservation the bit remains set, avoiding bitmap clear races; exact live state always remains in the six slots.

Each slot records physical address, 32/64-bit width, state (`Active`/`Committing`) and a generation used by an opaque token. Generated PPC holds that token in `CpuState::reservation`. The slots are intentionally not a second page table and are not attached to hot translation entries.

### Conditional-store race prevention

The V1 implementation could check a generation and then perform the store while a normal fast writer raced between those operations. V2 closes that window with a very short reservation commit gate used only by uncommon conditional stores. Normal RAM writes remain concurrent: they enter the existing coherency writer section, recheck the gate, invalidate matching active slots and perform the native store. A conditional store closes the gate, waits for already-entered writers/reservation setup operations to drain, atomically claims its slot as `Committing`, performs the store, publishes coherency and reopens the gate.

This gate is not taken by ordinary reads and is not a global management mutex on normal stores. The hot write cost for a granule never used by LR/SC is a bitmap bit test in addition to the coherency bookkeeping that already existed.

### Reservation setup

Load-reserve setup participates in the same gate handshake. It snapshots the shared write epoch around slot publication and the atomic load; if an overlapping writer is detected during that narrow setup interval, the reservation is conservatively lost. PowerPC permits reservation loss, so this favors correctness over manufacturing a successful reservation through a race.

Regression coverage includes 32-bit and 64-bit reservations, same-physical-word access through different guest aliases, same-granule conflicts, neighboring-granule survival, controlled external/DMA invalidation, replacement of an older reservation by a newer load-reserve, six simultaneously live reservations and six-thread contended LR/SC retry loops.

## 9. PPC memory ordering

Memory V2 now has an explicit canonical ordering model in
`xenon/cpu/memory_ordering.hpp`. Guest semantics are kept separate from the host
instruction selected to implement them. Ordinary RAM loads/stores remain relaxed
atomic operations; PPC barriers establish the required ordering between them.

The canonical data-order matrix is:

| Barrier | Load -> Load | Load -> Store | Store -> Load | Store -> Store | Scope |
| --- | --- | --- | --- | --- | --- |
| `sync` | yes | yes | yes | yes | general |
| `lwsync` | yes | yes | **no** | yes | general |
| `eieio` | yes | yes | yes | yes | device/cache-inhibited/WC only |
| `isync` | no data pair | no data pair | no data pair | no data pair | instruction stream |

The important optimization is that `lwsync` is no longer silently strengthened to
a universal sequentially-consistent fence. Xenon preserves the PowerPC property
that Store->Load is not ordered by `lwsync`.

Current host lowering is:

- **x86-64 `sync`**: `MFENCE` plus a compiler barrier. x86 TSO already supplies
  LL/LS/SS ordering, but heavyweight `sync` must also close Store->Load.
- **x86-64 `lwsync`**: compiler barrier only. TSO already provides exactly the
  required LL/LS/SS data ordering and naturally leaves Store->Load relaxed.
- **x86-64 `eieio`**: `MFENCE` conservatively covers future host MMIO/WC mappings.
- **x86-64 `isync`**: guest instruction/execution boundary plus compiler barrier;
  generated guest code is not fetched directly from guest RAM.
- **ARM64 `sync`**: `DSB ISH`, preserving the heavyweight completion semantics.
- **ARM64 `lwsync`**: `DMB ISHST` followed by `DMB ISHLD`, providing SS plus
  LL/LS without deliberately introducing Store->Load ordering.
- **ARM64 `eieio`**: `DMB OSH` for device/cache-inhibited ordering outside the
  inner-shareable domain.
- **ARM64 `isync`**: `ISB`.
- other hosts use conservative standard-C++ fence fallbacks until tuned.

`lwarx`/`ldarx` and `stwcx.`/`stdcx.` remain reservation/atomic primitives rather
than being incorrectly promoted to implicit full fences. Guest software supplies
its architecturally required barrier pattern around those operations. The existing
reservation commit gate guarantees the conditional write itself is atomic with
respect to Xenon writers; it is not used as a replacement for PPC ordering.

Validation now includes compile-time checks of the canonical ordering matrix, a
two-thread message-passing litmus using real Memory V2 relaxed RAM accesses and
`lwsync`, plus generated AOT execution of `sync`, `lwsync`, `eieio` and `isync`.

Two boundaries remain intentionally separate:

- phase 16 still needs the hot-page memory type to drive concrete MMIO,
  cache-inhibited and write-combined access policy rather than only barrier policy;
- phase 19 will connect `icbi`/`isync` to executable-page generations and dynamic
  native-code invalidation.

## 10. MMIO fast/slow split

MMIO ranges are represented in the hot translation table as slow pages. Ordinary RAM therefore performs one hot page lookup and never scans the registered MMIO range collection.

A slow access enters `AddressSpace`, where the existing width-aware MMIO callback behavior is preserved.

The slow MMIO dispatcher itself still uses the current range collection. A page-indexed/interval device dispatcher is a later cold-path optimization; it is no longer on the normal RAM path.

Arbitrary device handlers are not invoked by the new fast RAM path.

## 11. Shared CPU/GPU coherency

Memory V2 introduces `memory::GuestMemoryCoherency` as a backend-neutral Xenon service.

It owns per-physical-page write epochs and a bounded lock-free exact-write journal. Writers publish changes through Xenon Memory rather than maintaining Vulkan- and D3D12-specific CPU dirty semantics. Page epochs are the durable conservative record; the journal lets current consumers retain byte ownership when CPU and GPU update different bytes of the same page. If a consumer falls more than 65,536 write epochs behind, journal collection reports overflow and the consumer safely falls back to dirty page ranges.

Current consumers:

- Vulkan guest-memory mirror;
- D3D12 guest-memory mirror;
- texture dirty tracking.

The mirrors now remember a synchronized epoch, ask Xenon Memory first for exact changed ranges and upload only those physical ranges after initial synchronization. Their backend-neutral byte-range ownership tracker subtracts only overlapping GPU-authored bytes, so a one-byte CPU write cannot destroy neighboring memexport/GPU data. A first synchronization may legitimately cover the complete guest RAM because mirror initialization starts CPU-dirty; subsequent synchronization is range-driven. Journal wrap uses coalesced dirty pages as the correctness fallback.

The epoch snapshot has an active-writer stability boundary. A GPU consumer does not advance its synchronized epoch past a CPU writer whose dirty-page publication is incomplete. If a page receives an even newer write after the consumer's boundary, the page can be conservatively included now and remains logically dirty for the following epoch as well, preventing a later page-epoch overwrite from hiding an earlier unsynchronized write.

This moves Xbox guest-memory coherency policy out of the Vulkan/D3D12 implementations. Native backends execute synchronization/upload plans; they do not define independent Xbox memory semantics.

## 12. Write observers and dirty tracking

The legacy synchronous physical-write observer API has now been removed. There is no observer-active branch, callback thunk, callback-vector copy or arbitrary callback execution in the ordinary `MemoryAccessContext` store path.

CPU and controlled external writers publish dirty state directly into fixed Xenon-owned coherency metadata. Vulkan/D3D12 mirrors and texture tracking consume that state asynchronously/range-wise rather than being invoked by every scalar write.

Instruction-cache/native-code invalidation is represented separately by sticky physical executable-page generations. CPU stores, controlled external writes and `icbi` advance those generations without reintroducing arbitrary per-store observer dispatch; native code-cache eviction still needs to consume the API.

## 13. External/DMA writes

Production code no longer receives unrestricted mutable physical backing. `physical_data()` is read-only outside the memory implementation.

Memory V2 now provides controlled external-write operations:

- `write_physical` for copying a known byte range into physical RAM;
- `fill_physical` for bounded physical fills;
- move-only RAII `PhysicalWriteSpan` for native subsystems that genuinely need a direct bounded mutable span.

`PhysicalWriteSpan` publishes its complete declared range automatically when the scope ends. The current completion path invalidates LR/SC reservation generations and publishes CPU/GPU dirty/coherency state. The Xenos PM4 physical-write path and native Vulkan/D3D12 resolve writeback paths use these controlled APIs instead of mutating `physical_data()` and making a second notification call.

This closes the bookkeeping-bypass API for current production callers. Executable-page generation changes use the same completion path, so external writers do not need a second contract.

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

Execute permission is represented in the compact hot entry. Each physical page that
has hosted executable guest code receives a sticky nonzero generation. Ordinary CPU
stores and controlled external/DMA/GPU-style writes increment that generation only
when the page is executable, avoiding callback dispatch on scalar writes. `icbi`
advances the same generation, and write/execute protection transitions publish the
page into generation tracking.

`AddressSpace::executable_generation` exposes the current physical generation through
a guest executable mapping so a native code cache can validate a compiled block
without owning Xbox page semantics.

Required follow-up includes:

- invalidation of generated/native code associated with changed pages;
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
- range fill/read/write across page boundaries with page-level rather than byte-level publication;
- overlap-safe linear atomic move behavior;
- bounded-scratch copy across physically discontinuous non-overlapping mappings;
- pathological reversed-page alias overlap retaining snapshot semantics;
- MMIO range read/write/copy through the slow path;
- generated PPC string and VMX partial transfers through production `AddressSpace`;
- batched multi-dword Xenos `MEM_WRITE` publication;
- range-level notifications across physically discontinuous page mappings;
- anonymous physical owner release while an alias remains live;
- prevention of physical-page reuse until the final alias is released;
- reuse after the final alias is released;
- retirement of unpublished physical pages while an older fast access context
  remains alive, followed by reuse only after read-side quiescence;
- coherency-driven texture dirty detection;
- controlled physical writes automatically invalidating reservations and publishing coherency state;
- V2 six-slot reservation monitor behavior across physical aliases, 32/64-bit operations, neighboring granules and six-thread contention;
- generated load-reserve replacement and physical-alias conditional-store behavior;
- scoped physical write spans automatically publishing their declared range at scope completion;
- rejection of out-of-range controlled physical writes;
- Windows host-VM reserve/commit/protect/decommit/recommit/discard/release lifecycle and zero-fill behavior;
- executable generation changes after CPU writes, controlled external writes and `icbi`;
- existing CPU/memory/graphics regression behavior.

The generated AOT output has also been inspected: representative `lwz`/`stw` paths acquire `memory.access_context()` and emit `memory_access.read32_be` / `memory_access.write32_be`. Reservations remain uncommon `MemoryPort` operations, but generated load-reserve now cancels a prior `CpuState` reservation and generated store-conditional delegates physical identity to Xenon Memory rather than incorrectly requiring the same guest virtual address.

## 20. Current validation environment

Current development validation is Linux x86-64. After closing Phase 4 concurrency hardening:

- GCC Release full generic matrix: **24/24 CTest suites passed**;
- Debug ASan+UBSan targeted memory/CPU→memory/texture/GPU-front-end matrix:
  **4/4 passed**;
- Clang Release memory + generated CPU→memory integration: **2/2 passed**;
- GCC TSan `xenon_memory_tests`: **passed with no reported data race**.

Windows x86-64 validation on the current host additionally includes clean Visual Studio
Debug and Release builds with the real `host_vm_windows.cpp` source. The complete native
matrix passes **27/27** in both configurations, including CPU, Memory V2, Vulkan, D3D12,
presentation, EDRAM ownership and Vulkan↔D3D12 canonical handoff tests. The Release
benchmark target was also rebuilt and run after exact-range coherency publication was
added. Current Linux host-VM lifecycle revalidation is still required before Phase 5 is
marked strictly complete.

The September 2026 Windows Release baseline records approximately 24–25 ns for aligned
scalar load/store pairs, 32 ns for 128-bit access, 35 ns for random translation, 2.7–2.9
µs for 64 KiB fill/write, 101 ns for reservation operations, 93 ns per operation under
six-thread contention and 88 µs to coalesce a dirty 2 MiB region. These values are a
comparison baseline, not pass/fail thresholds.

## 21. Remaining dependency order

The project now closes unfinished phases in numerical order before advancing. Phase 4
is closed in this slice. The next strict completion sequence is:

1. **Phase 5** — finish host-VM abstraction and platform separation, including the
   remaining Windows/Linux validation and lifecycle semantics;
2. **Phase 6** — research and, if valid, implement the optional direct guest aperture
   while retaining compact translation as the portable fallback;
3. Phases **7-9** are already complete;
4. **Phase 10** — close the remaining device/write-combined/cache-inhibited and
   instruction-synchronization ordering integration;
5. Phase **11** is already complete;
6. Phases **12-13** are already complete;
7. then close **14, 15, 16, 17, 18, 19, 20 and 21** one at a time under the same
   strict completion rule.

A later phase may still supply a dependency required by an earlier one, but the earlier
phase will not be marked complete until that dependency is implemented and tested.

## 22. Definition-of-done status

The following V2 goals are already substantially represented in production code:

- compact dedicated hot translation metadata;
- non-virtual generated ordinary load/store path;
- no global `AddressSpace` mutex for common generated or direct normal-RAM accesses;
- no RegionDescriptor/MMIO scan for common RAM accesses;
- no heap allocation on scalar fast accesses;
- backend-neutral CPU/GPU dirty tracking;
- no synchronous physical-write observer/callback branch on scalar stores;
- controlled external/DMA/GPU write APIs with automatic reservation/coherency completion;
- read-only raw physical backing outside the memory implementation;
- read-side quiescence preventing unpublished physical pages from being recycled into ABA/stale translations;
- complete Phase 11 range/block path covering fill/zero/copy/move/dcbz/string/vector-partial/current external bulk transfers;
- physical alias lifetime/refcount protection with sparse dynamic reverse mappings and explicit invariant validation;
- coalescing physical range allocation with aligned top-down/bottom-up runs and retired-range reclamation;
- host VM implementation separated from the canonical Xbox address-space model;
- six-thread normal-RAM stress coverage;
- compact six-slot PPC reservation monitor with physical-alias and six-thread contention coverage.

Memory V2 is **not complete**. In strict terms, **10 of the 21 brief phases are closed** (1, 2, 3, 4, 7, 8, 9, 11, 12 and 13), with strong production foundations in phases 5, 10 and 14–21. Remaining closure work includes Linux host-VM/backend validation, optional direct-aperture research, observable memory-type/device ordering, requested-range/device-valid and future UMA synchronization planning, native code-cache consumption of executable generations, fault/MMIO hardening, allocation/GPU-upload benchmark coverage, broader fuzz/model testing and sanitizer runs on supported toolchains.

# Xenon Memory V2

Status: **Memory V2 roadmap complete (21/21)** — the native fast path, ownership/coherency architecture, benchmark closure and model-based hardening defined by the original Memory V2 brief are implemented. Future game bring-up may still expose bugs, but there is no remaining planned Memory V2 phase.

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
| 5. Host VM backend | **Complete** | Windows, POSIX/Linux and fallback implementations are separated from Xbox policy and expose reserve/commit/decommit/release/protect plus opaque shared-memory map/unmap primitives. POSIX uses fixed `mmap`; modern Windows additionally exposes `VirtualAlloc2` placeholder reservation plus `MapViewOfFile3`/`UnmapViewOfFile2` replacement while retaining compact fallback when those APIs are unavailable. |
| 6. Direct guest aperture | **Complete** | A 4 GiB optional shared guest aperture is implemented where the host can safely replace fixed mappings. POSIX can map the permanent physical aliases as large fixed views; Windows placeholder mode uses page-granular dynamic Virtual/XEX aliases while deliberately leaving permanent 0x7F/A/C/E windows on compact translation to avoid hundreds of thousands of section views. Safe quiescent transitions and compact fallback remain mandatory; Auto/default stays compact on the qualified Linux x86-64 host. |
| 7. Physical allocator | **Complete** | Coalescing free/retired ranges replace page-by-page search; 4 KiB granularity, contiguous aligned runs, deterministic reuse and randomized fragmentation/coalescence are implemented. The Xbox-facing policy now models 4 KiB/64 KiB/16 MiB physical page classes, physical min/max bounds, top-down placement, cache/protection metadata, the first-16-MiB system window, the final-64-KiB reservation and the 4 KiB E-view/MMIO addressability ceiling. |
| 8. Physical ownership/aliases | **Complete** | Mapping refcounts are cross-checked against sparse cold reverse mappings; pending-free/retired ownership, overlapping views, fixed/XEX/GPU-visible aliases, release-order/reallocation behavior and randomized alias-model tests are implemented without whole-space free scans. |
| 9. PPC reservation monitor | **Complete** | Six live reservation slots plus a ~512 KiB sticky granule bitmap replace the 16 MiB generation table; physical identity, 32/64-bit LR/SC, alias conflicts, DMA/external invalidation, neighboring granules, replacement and six-thread contention are covered. |
| 10. PPC memory ordering | **Complete** | Canonical `sync`/`lwsync`/`eieio`/`isync` semantics, x86-64/ARM64 lowering, real Normal/WC/CI/MMIO domain classification, LR/SC separation, message-passing/store-buffering litmus tests and an AOT `isync` redispatch/refetch boundary are implemented. Full memory-type host policy and native code-cache eviction remain phases 16/19, not missing ordering semantics. |
| 11. Block/range access | **Complete** | `read/write/fill`, zero, copy/move, `dcbz`, PPC string operations, VMX partial transfers and current external/DMA-style physical writers use page/range operations; common linear copies use overlap-safe atomic word bursts, nonlinear non-overlap uses bounded scratch, and only pathological MMIO/alias cases use snapshot slow paths. |
| 12. Dirty tracking/observers | **Complete** | Xenon-owned page epochs plus a bounded lock-free exact-range journal drive consumers; synchronous physical-write observers have been removed from the scalar path entirely. Journal wrap safely falls back to conservative page ranges. |
| 13. Safe external writes | **Complete** | Raw physical backing is read-only externally; `write_physical`, `fill_physical` and RAII `PhysicalWriteSpan` automatically publish reservation, coherency and executable-generation changes. Production Xenos/Vulkan/D3D12 writers use these controlled paths. |
| 14. Shared CPU/GPU coherency | **Complete** | `GuestMemoryGpuCoherency` is the backend-neutral ownership authority for CPU/DMA publications, GPU-authored ranges, upload/readback planning, per-range GPU generations, device validity and discrete-vs-shared topology. Vulkan/D3D12 execute its plans; GPU readback uses a physical-write quiescence window so concurrent CPU writes cannot be overwritten, self-publications are source-acknowledged, and future UMA paths use visibility-only actions without changing Xbox semantics. |
| 15. Lazy/range GPU sync | **Complete** | GPU synchronization is request-driven: touched physical pages, CPU/GPU dirty ranges, exact requested ranges and device-valid ranges are tracked/coalesced. Device-invalid requested bytes are initialized lazily even when untouched, repeated valid requests produce no copy, active vertex/memexport/readback/texture/index/command-data paths request bounded ranges, and only genuinely dynamic/unrestricted memexport uses the explicit full-512-MiB fallback. |
| 16. Memory types | **Complete** | Normal cached, write-combined, cache-inhibited and device mappings now affect fast-path eligibility, ordering/coherency publication, GPU upload planning and MMIO policy. Physical-allocation cache/protection policy is shared by the A/C/E architectural aliases instead of those aliases silently becoming generic cached RW memory. WC/CI stay off the direct aperture unless an alias-safe native backend exists. |
| 17. MMIO fast/slow split | **Complete** | MMIO pages are marked slow in hot metadata; ordinary RAM never searches the device catalogue. Registered devices use sorted O(log N) interval lookup, handler snapshots outlive catalogue mutation, and arbitrary callbacks run outside the AddressSpace management mutex. |
| 18. Fault/protection model | **Complete** | `MemoryFaultInfo` captures the original request, exact faulting address, width/access/reason, region kind, mapped/committed/page state, allocation/current protection, physical backing, MMIO/device state and memory type. The low 64 KiB is represented as a committed no-access guard, query regions stop at original allocation boundaries, and byte-range decommit covers every touched page. Cross-page and wrapping accesses preserve precise fault context, while host fault mechanisms remain isolated below Xenon semantics. |
| 19. Executable/SMC | **Complete** | Sticky physical executable generations are consumed by a thread-safe native `ExecutableCodeCache`. Translations snapshot physical-page identity + generation across every source page; CPU/alias/DMA/GPU writes, `icbi`, execute-protection transitions, remap/reuse and reset make stale native code fail validation and be lazily evicted. Dynamic recompilation revalidates source snapshots before installation, and instruction fetch uses the Execute-aware `MemoryPort::fetch32_be` path without introducing an interpreter. |
| 20. Benchmarking | **Complete** | The Release benchmark now covers the full brief: scalar/vector/endian/sequential/random/cross-page access, compact/direct translation, fixed physical aliases, reservations, six-thread contention, block zero/copy, MMIO, allocation/free, mixed fragmentation, dirty coalescing, CPU→GPU synchronization planning and the production discrete-mirror physical snapshot/upload-staging path. CSV, JSON and human reports include platform/architecture/compiler/build/translation metadata and are portable to Windows x86-64, Linux x86-64, Linux ARM64 and Android ARM64. |
| 21. Fuzzing/hardening | **Complete** | A dedicated deterministic model/fuzz target assaults A/C/E/7F aliases, dynamic ownership/refcounts/reverse mappings, release/reuse, XEX coherence, protection + executable generations, dirty-journal wrap/fallback, LR/SC invalidation, controlled external writes and six-thread CPU+DMA concurrency. Seeds and workload scale are externally controllable for reproduction/sanitizer runs; GCC Release, ASan+UBSan, Clang Release and GCC TSan validation are clean on Linux x86-64. |

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

### 6.3 Xbox/XDK compatibility closure

A post-roadmap comparison against current Xenia memory research, successful Xbox 360
recompilation runtimes and the public NT/XDK-shaped memory contract identified several
observable policies that were not represented strongly enough by the original Memory V2
API. They are now modelled in the canonical memory layer rather than being deferred to a
game module or future kernel shim.

The compatibility closure adds:

- a permanent committed/no-access low 64 KiB user guard;
- a permanently unavailable final 64 KiB of physical RAM (`0x1FFF0000-0x1FFFFFFF`);
- Xbox-facing physical allocation options for 4 KiB, 64 KiB and 16 MiB page classes,
  physical minimum/maximum bounds, alignment, top-down placement, zero/no-zero policy,
  protection and cache policy;
- page-class-aware placement: 4 KiB allocations are limited to physical addresses that
  can be represented by the E-view before the `0xFFD00000` MMIO window, while 64 KiB
  A-view and 16 MiB C-view allocations retain the wider physical range;
- cold per-physical-page allocation metadata so `MmQueryAllocationSize`-,
  `MmQueryAddressProtect`- and `MmSetAddressProtect`-style kernel services can be thin
  consumers rather than reimplementing memory semantics;
- propagation of physical allocation protection/cache state into every A/C/E fixed alias,
  including executable-generation invalidation when physical Execute permission changes;
- non-fixed reserve-only virtual allocation and MEM_NOZERO-style commit/physical allocation
  policy without a parallel allocator;
- query-region termination at original allocation identity even when adjacent allocations
  have identical state/protection, plus non-zero free-region sizing up to the next occupied
  page/heap boundary;
- byte-granular decommit range handling so every architectural page containing a requested
  byte is decommitted; 64 KiB virtual/XEX heaps therefore fan one management operation out
  over their sixteen internal 4 KiB hot entries;
- architectural-page protection: 64 KiB virtual/XEX pages and 4 KiB/64 KiB/16 MiB physical
  allocation classes cannot be split into smaller externally visible protection states;
- protection requests may not span independent virtual or physical allocations, matching
  the allocation-identity boundary expected by the Xbox/NT-style management contract.

These additions are deliberately **cold policy/metadata changes**. The generated PPC fast
path is still a compact hot-page lookup followed by native RAM access; normal loads/stores
do not consult physical-allocation objects, allocation ranges or kernel-style query state.

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
`lwsync`, a Store-Buffering litmus proving heavyweight `sync` closes Store->Load,
plus generated AOT execution of `sync`, `lwsync`, `eieio` and `isync`.

The ordering domain is now derived from the production mapping rather than being a
documentation-only enum. `MemoryAccessContext::ordering_domain()` classifies normal
cached RAM, hot-entry `WriteCombine`, hot-entry `NoCache`/cache-inhibited mappings,
and MMIO/device pages. `eieio` is architecturally meaningful for the latter three
domains and not for normal cached RAM. The host fence is allowed to be stronger than
the minimum guest guarantee, but the canonical model does not falsely claim that
normal cached accesses are ordered by `eieio`.

`isync` is also a real recompilation boundary now. Generated AOT code performs the
host instruction-synchronization primitive and returns to the dispatcher at the next
guest PC. This discards the current native translation's already-fetched continuation,
allowing executable-page generation validation / later code-cache invalidation to take
effect before the next guest instruction executes. `icbi` continues to advance the
physical executable generation.

Phase 16 now closes the broader memory-type policy as well: guest cache type affects
host representation eligibility, dirty-publication identity and GPU visibility planning.
Phase 19 now closes native code-cache ownership and eviction. The ordering model therefore has an actual dispatch-side consumer for executable generations: `isync` returns to dispatch, and the next native lookup validates the source-page physical identity and generation before execution.

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

Instruction-cache/native-code invalidation is represented separately by sticky physical executable-page generations. CPU stores, controlled external writes and `icbi` advance those generations without reintroducing arbitrary per-store observer dispatch. Phase 19's `ExecutableCodeCache` consumes physical identity + generation snapshots at dispatch and lazily evicts stale native translations.

## 13. External/DMA writes

Production code no longer receives unrestricted mutable physical backing. `physical_data()` is
read-only outside the memory implementation and is now treated as a diagnostic/quiescent-view
API, not the production concurrent-read contract. All graphics production callers were migrated
away from direct backing dereferences.

Concurrent GPU/APU/command-processor readers use `copy_physical_range()`. It performs bounded
atomic word/byte snapshots without the global AddressSpace management mutex, so a CPU-side
Xenon store and a GPU-side CPU planner can legally overlap under the C++ memory model. PM4 ring
reads, indirect shader microcode, constant loads, resolve vertices, DMA index buffers, texture
decode inputs, presentation inputs and raw-resolve read/modify/write blocks all use snapshots.
Snapshot-aware common GPU helpers accept a physical base so callers copy only the exact resource
range instead of a synthetic 512 MiB mirror.

Memory V2 now provides controlled external-write operations:

- `write_physical` for copying a known byte range into physical RAM;
- `fill_physical` for bounded physical fills;
- move-only RAII `PhysicalWriteSpan` for native subsystems that genuinely need a direct bounded mutable span.

`PhysicalWriteSpan` publishes its complete declared range automatically when the scope ends. The current completion path invalidates LR/SC reservation generations and publishes CPU/GPU dirty/coherency state. The Xenos PM4 physical-write path and native Vulkan/D3D12 resolve writeback paths use these controlled APIs instead of mutating `physical_data()` and making a second notification call.

This closes the bookkeeping-bypass API for current production callers. Executable-page generation
changes use the same completion path, so external writers do not need a second contract. The read
side is covered by the atomic snapshot contract above; the Phase-21 hardening target now races
aligned CPU stores against repeated physical snapshots under TSan.

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
- release;
- opaque move-only shared-memory creation;
- shared map/unmap;
- capability-tested fixed shared mapping and restoration of an inaccessible
  reservation for the optional direct aperture.

Implementations:

- Windows: `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, pagefile-backed
  `CreateFileMapping`/`MapViewOfFile` shared views, plus capability-loaded
  `VirtualAlloc2` placeholders and `MapViewOfFile3`/`UnmapViewOfFile2` for
  page-granular fixed aperture replacement on Windows 10 1803+;
- POSIX/Linux: `mmap`, `mprotect`, `madvise`, `munmap`, `memfd_create` with an
  immediately-unlinked `shm_open` fallback;
- fallback implementation for unsupported hosts.

`AddressSpace` now asks this abstraction for physical backing rather than containing Win32/POSIX calls itself. Android/Linux ARM64 can share the POSIX family while retaining Android-specific policy behind the same boundary if required later.

Host VM protection is not the authoritative Xbox protection mechanism; Xbox
permissions remain explicit Xenon page metadata. This avoids accidentally turning a
host OS mechanism into Xbox-visible policy. Shared-native handles are opaque to the
Xbox-facing layer and to game modules.

## 15. Direct guest aperture

Phase 6 research resulted in an optional 4 GiB guest virtual-address aperture built on
the host-VM shared-mapping primitives. It is an acceleration representation only; the
compact translation table remains authoritative for mapping state, permissions, MMIO,
physical identity, reservations and coherency.

Two production strategies therefore exist:

- `GuestTranslationMode::Compact` — compact hot entry -> shared physical backing;
- `GuestTranslationMode::DirectAperture` — the same hot entry additionally marks a
  fixed guest-VA host alias, allowing the host pointer to be `aperture + guest VA`.

`GuestTranslationMode::Auto` currently selects compact translation unless a platform
is explicitly qualified at build time with `XENON_MEMORY_DEFAULT_DIRECT_APERTURE`.
Repeated paired Release measurements on the current Linux x86-64 environment found
the direct path within noise or slower than compact translation, with the cleaner
three-way runs around 87-93 ns/op for compact random translation versus roughly
99-102 ns/op for the direct aperture. Spending 4 GiB of host VA is therefore not the
default merely because the mechanism exists.

The aperture always uses the same shared 512 MiB physical backing, but the host
representation is deliberately backend-specific. POSIX can map the permanent 0x7F/A/C/E
aliases as a few large fixed views and can also replace dynamic Virtual/XEX ranges. Modern
Windows reserves the 4 GiB aperture with `MEM_RESERVE_PLACEHOLDER`; an exact 4 KiB
placeholder slice is isolated and replaced with `MapViewOfFile3(MEM_REPLACE_PLACEHOLDER)`.
Because later guest remaps must be independently replaceable, Windows uses page-sized
section views for dynamic Virtual/XEX mappings. The permanent physical alias windows stay
on compact translation on that backend instead of manufacturing hundreds of thousands of
4 KiB section views merely to mirror A/C/E/7F.

Mapping transitions first withdraw the aperture bit from the published hot entry and wait
for old read-side contexts to quiesce before replacing or restoring a host view. Windows
restores mapped slices with `UnmapViewOfFile2(MEM_PRESERVE_PLACEHOLDER)` and coalesces the
placeholder reservation on aperture teardown. If a safe transition cannot be performed,
that page stays on compact translation rather than risking stale host pointers. Older
Windows versions and hosts without reliable fixed shared mappings simply keep the compact
path.

This leaves ARM64/Android free to use compact translation indefinitely or qualify a
direct aperture later without changing Xbox-visible semantics.

## 16. Executable/self-modifying memory

Execute permission is represented in the compact hot entry. Each physical page that
has hosted executable guest code receives a sticky nonzero generation. Ordinary CPU
stores and controlled external/DMA/GPU-style writes advance that generation when the
physical page participates in executable tracking, avoiding callback dispatch on the
scalar store path. `icbi`, execute-protection transitions, decommit/recommit, release/
reuse and reset also advance the executable epoch so old native translations cannot
resurrect through an ABA-style lifetime transition.

`MemoryPort::executable_page_stamp` exposes a physical-page identity + generation stamp
through a guest executable mapping. `ExecutableCodeCache` owns native translations and
snapshots every executable page covered by a translation. A dispatch lookup returns the
native entry only if all source stamps still match; otherwise the entry is lazily evicted.
This makes aliases naturally coherent because different guest views of the same physical
code page compare against the same physical identity/generation.

Static recompilation modules register their native functions against the current source
snapshot. Dynamic guest code can be recompiled through an optional compiler callback;
the translator captures the executable source snapshot it decoded, uses the Execute-aware
`MemoryPort::fetch32_be` path, and installation revalidates the snapshot so a concurrent
self-modifying write cannot bless stale native code. A miss returns to the native
recompilation/dispatch architecture; no interpreter fallback is introduced. Explicit
range invalidation remains available for module unload/tooling but is not required for
ordinary SMC writes.

## 17. Memory types

No-cache and write-combine flags are carried in the hot entry, preserving the information needed by the access layer.

Their full observable effects on ordering, host mapping policy and GPU/device visibility are not yet complete. Those semantics must be defined together with the PPC ordering and device-memory work rather than implemented as unrelated backend flags.

## 18. Fault model

`MemoryFault` remains the single slow/reference guest-fault mechanism; fast accesses fall back instead of inventing a second policy. Phase 18 replaces the old four-field exception payload with a trivially-copyable `MemoryFaultInfo` record suitable for a later Xbox kernel exception translator.

The record captures:

- original request address and width;
- exact faulting address for cross-page operations;
- read/write/execute access kind and canonical `FaultReason`;
- whether an address-space region exists;
- region kind and base page size;
- free/reserved/committed page state;
- mapped/committed state;
- allocation and current Xbox protection;
- physical address where live backing exists;
- MMIO/device state; and
- canonical memory type / host mapping policy.

Cross-page scalar/range fallbacks now retain the original request width while identifying the exact later page that failed. Multi-byte accesses that would wrap the 32-bit guest address space fail as `OutOfRange` before touching wrapped memory. Scalar accesses that partially overlap an MMIO registration fail as `MmioWidth` rather than silently reading or writing the underlying RAM page.

The pre-existing `MemoryFault::address()`, `width()`, `access()` and `reason()` accessors remain available for tooling compatibility, with `info()` exposing the complete record.

Host SEH/signal/page-fault optimization is deliberately not part of guest exception policy. All native reserve/commit/protect machinery remains encapsulated in `src/memory/mapping/`; a future host-fault accelerator must translate back into the same `MemoryFaultInfo` contract rather than exposing platform-specific state to Xenon or game modules.

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

## 20.1 Phase 14 shared CPU/GPU coherency completion

Phase 14 is strictly complete. Xenon Memory, not Vulkan or D3D12, owns the CPU/GPU
coherency state machine through `GuestMemoryGpuCoherency`. The shared service tracks
CPU/DMA write epochs, GPU-authored byte ranges with per-range generations, device-valid
ranges, and backend-neutral upload/readback plans. Native graphics backends execute those
plans but do not redefine Xbox-visible ownership semantics.

GPU-to-CPU reconciliation is race-safe: before readback planning, newer CPU publications
remove overlapping GPU ownership; after the native copy completes, an AddressSpace
physical-write quiescence window blocks concurrent CPU writers while the tracker rechecks
ownership and commits only bytes still owned by the planned GPU generation. The exact
publication epoch produced by that commit is acknowledged as the mirror's own write so it
is not echoed back as CPU dirt, while unrelated CPU/DMA writes remain dirty. Newer
overlapping GPU generations survive older readback completion, and unrelated GPU writes
do not invalidate independent plans.

The same ownership model represents discrete mirrors and future UMA/mobile memory.
Discrete Vulkan/D3D12 plans request `Copy`; shared-host-visible topology requests
`VisibilityOnly`, preserving one Xbox semantic model while allowing future Android/ARM64
backends to avoid unnecessary copies.

Completion validation on Linux x86-64:

- GCC Release full matrix: **29/29 passed**;
- targeted Debug ASan+UBSan matrix: **6/6 passed**;
- targeted Clang Release matrix: **3/3 passed**;
- GCC TSan dedicated `xenon_gpu_coherency_tests`: **passed with no reported race**;
- the broader TSan `xenon_memory_tests` exceeded the environment execution window and
  is therefore not claimed as a completed TSan pass;
- native Vulkan/D3D12 runtime validation remains platform-dependent, while their source
  paths consume the same backend-neutral Xenon plans.

## 20.2 Phase 15 lazy/range-driven GPU synchronization completion

Phase 15 is strictly complete. Guest-memory mirrors no longer perform an unconditional
512 MiB upload at submission start. `GuestMemoryGpuCoherency` tracks exact requested
ranges and page-aligned touched ranges in addition to CPU/GPU dirty ownership and
device-valid coverage. Upload planning includes only bytes that are CPU-new or requested
but not yet device-valid, so untouched zero-filled RAM is initialized lazily on first use
instead of being globally marked dirty. Adjacent dirty/requested ranges coalesce and
transfer-size limits split only the resulting native work items.

Current production requests are range-specific where the backend can know the address:

- active Xenos vertex fetch buffers request their exact physical spans before shader use;
- sampled textures request exact GPU-to-CPU subresource visibility before CPU-side decode;
- index buffers request exact GPU-to-CPU visibility before primitive processing;
- resolve/command-data streams request only their referenced dwords;
- statically planned memexport targets request their exact ranges before writable shader
  execution and publish the same exact GPU-owned ranges afterward;
- presentation/render readback requests exact texture subresources;
- shader microcode is already decoded by the CPU command front-end before native backend
  consumption, so it does not require a redundant mirror upload; the shared range API
  nevertheless carries a `Shader` usage class for backends that consume guest shader bytes
  directly.

Only memexport whose address cannot be bounded by static analysis invokes
`GuestMemoryMirror::synchronize()` and marks the complete physical aperture GPU-owned.
That is the deliberate full-range fallback for a genuinely unrestricted workload, not the
normal submission path. The same request planner supports discrete `Copy` and future
shared-host-visible `VisibilityOnly` execution.

Completion validation on Linux x86-64:

- GCC Release full matrix: **29/29 passed**;
- targeted Debug ASan+UBSan GPU/coherency/resource/texture/CPU-memory tests: **5/5
  passed**, with `xenon_memory_tests` also passing separately under ASan+UBSan;
- Clang Release memory/coherency/resource/CPU-memory matrix: **4/4 passed**;
- GCC TSan `xenon_gpu_coherency_tests` + `xenon_resource_ir_tests`: **2/2 passed**
  with no reported race;
- source audit confirms the only backend full-mirror synchronization calls remaining are
  the dynamic/unrestricted memexport fallbacks.

## 20.3 Phase 16 memory-type completion

Phase 16 is strictly complete. Xbox cache/protection attributes are now represented as
canonical `MemoryType` state rather than query-only flags. `AddressSpace::memory_type_info`
exposes NormalCached, WriteCombined, CacheInhibited and Device mappings together with the
host representation policy, executable status, direct-aperture eligibility and explicit
device-visibility requirement. `NoCache` and `WriteCombine` are mutually exclusive at all
allocation/protect/mapping entry points.

The shared 512 MiB physical backing deliberately remains a normal cached host mapping on
general desktop hosts. Xenon has many simultaneous aliases to the same physical bytes, and
creating conflicting native cache attributes for individual aliases is not a portable or
safe userspace contract. Instead, WC/CI semantics are translated through the compact hot
path: these mappings are excluded from the direct guest aperture, retain their distinct PPC
ordering domains, and require explicit device visibility/coherency handling. A future host
VM backend may use native WC/uncached mappings only when it can prove alias-safe support
without changing Xbox-visible semantics.

Cache identity is preserved beyond translation. Every CPU write publication carries its
`MemoryOrderingDomain` in the bounded exact dirty journal. `GuestMemoryGpuCoherency` tracks
that identity byte-precisely and places the strongest source domain on `GpuUploadPlan`, so a
future shared-host-visible/UMA backend can select appropriate cache maintenance or visibility
work while current discrete mirrors continue to copy the exact requested range. Upload
rollback preserves the original cache domain. If exact journal history has wrapped, the
page-level fallback is intentionally classified as CacheInhibited so loss of metadata can
never weaken visibility guarantees. Controlled physical DMA/GPU writes publish as Device
domain writes.

MMIO remains Device memory and is isolated to the slow dispatcher. Executability is
orthogonal to cache type: executable RAM continues to use executable-page generations and
recompilation invalidation rather than native execution from guest bytes. Existing `eieio`
semantics from Phase 10 consume the same WC/CI/Device ordering-domain classification.

Completion validation on Linux x86-64:

- GCC Release full matrix: **30/30 passed**;
- targeted Debug ASan+UBSan memory/type/coherency/CPU-memory/host-VM matrix: **5/5 passed**;
- Clang Release memory/type/coherency/CPU-memory matrix: **4/4 passed**;
- GCC TSan `xenon_memory_type_tests` + `xenon_gpu_coherency_tests`: **2/2 passed** with
  no reported race;
- direct-aperture tests verify cached RAM remains eligible while WC/CI pages never receive
  direct host aliases;
- exact-journal and forced-overflow tests verify WC/CI/Device source domains are never
  silently weakened.

## 20.4 Phase 17 MMIO fast/slow split completion

Phase 17 is strictly complete. MMIO pages are identified by the compact hot translation
entry, so ordinary RAM continues to resolve directly through the fast page table and never
scans device ranges. Actual device dispatch is cold: registered MMIO intervals are kept
sorted and non-overlapping, and lookup uses binary interval search rather than a linear
walk. Hot-page publication also uses logarithmic interval lookup when deciding whether a
page must carry the slow bit.

MMIO handler lifetime and locking are now explicit. The dispatcher snapshots a shared
handler reference while holding the AddressSpace management mutex, releases the mutex, and
only then invokes arbitrary device code. A handler may block, re-enter memory management or
clear the catalogue containing itself without holding or invalidating the global memory
lock. Range operations and 128-bit fallback paths use the same rule, so recursive outer
locks no longer accidentally extend across byte-visible MMIO callbacks.

A page containing a narrow MMIO overlay is conservatively marked slow as a whole. Accesses
outside the device interval on that page still resolve to the underlying RAM through the
cold resolver, preserving existing Xbox-visible overlay semantics. MMIO registration is
kept as a cold operation; overlapping intervals are rejected and adjacent intervals, even
within one 4 KiB page, remain independently dispatchable.

Completion validation on Linux x86-64:

- GCC Release full matrix: **31/31 passed**;
- targeted Debug ASan+UBSan MMIO/type/coherency/ordering/CPU-memory matrix: **5/5 passed**;
- Clang Release MMIO/type/ordering matrix: **3/3 passed**;
- GCC TSan `xenon_mmio_tests` + `xenon_memory_type_tests`: **2/2 passed** with no
  reported race;
- a dedicated concurrency regression holds an MMIO callback open while another thread
  commits guest RAM and verifies that management progress is not blocked by the handler;
- a self-clearing handler proves shared snapshot lifetime is safe when the active device
  catalogue is replaced during dispatch;
- the Release benchmark measured ordinary 32-bit RAM at about **316 ns/op with 256 MMIO
  devices registered versus 328 ns/op before that catalogue in the same run**, confirming
  that catalogue size is not on the ordinary RAM path; the final device in that catalogue
  dispatched at about **968 ns/op** through the cold path.

## 21. Phase 20 benchmark closure

Phase 20 is strictly complete. `xenon_memory_v2_benchmarks` now records all benchmark
categories required by the original Memory V2 brief. In addition to the existing scalar,
vector, endian, random-translation, cross-page, MMIO, reservation, six-thread and dirty
range cases, it measures:

- sequential RAM separately from random RAM;
- compact and direct-aperture guest→host translation independently;
- access through the fixed Xbox physical aliases;
- explicit 64 KiB block copy as well as block write/zero;
- repeated 4 KiB physical allocate/free;
- randomized mixed-size/alignment top-down/bottom-up fragmentation churn;
- backend-neutral CPU→GPU upload planning over exact requested ranges;
- upload-staging/snapshot bandwidth through `AddressSpace::copy_physical_range`, the same
  safe physical snapshot primitive used by the discrete Vulkan and D3D12 guest-memory
  mirrors before their native queue copy.

The benchmark executable no longer emits only an ad-hoc stream. It supports `csv`, `json`
and `human` output plus an optional output file and iteration override. Every report carries
platform, architecture, compiler, CMake build configuration, active guest translation mode,
pointer width and fixed-shared-mapping capability. The JSON schema is
`xenon-memory-v2-benchmark-v1`, which gives Windows x86-64, Linux x86-64, Linux ARM64 and
Android ARM64 runs one directly comparable results format without requiring all four hosts
to be physically present during implementation.

Native Vulkan/D3D12 queue/device throughput is intentionally not folded into canonical
Memory V2 semantics. The Memory benchmark measures the common upload planning + staging
boundary owned by Xenon Memory; graphics-backend performance tooling may add the
hardware-specific queue-copy portion without changing this schema or the Xbox model.

## 22. Phase 21 invariant/model-fuzz hardening closure

Phase 21 is strictly complete. `xenon_memory_hardening_tests` is a dedicated model-based
harness rather than more one-off assertions inside the ordinary memory test. Its default
run is deterministic, while `XENON_MEMORY_FUZZ_SEED` selects a reproducible alternate
random stream and `XENON_MEMORY_FUZZ_SCALE` scales expensive loops for sanitizer/CI runs.
The harness continuously checks production invariants while randomized operations execute.

Coverage includes:

- randomized fixed A/C/E/7F physical-alias coherence, including the E-view 4 KiB offset;
- a 64-page physical ownership model with 48 independently churning guest mappings,
  reverse-map enumeration, protection transitions and periodic `validate_invariants()`;
- refusal to free owned physical memory while a live guest alias remains;
- anonymous commit/release/recommit churn with stale-address reachability checks;
- XEX 0x8/0x9 alias identity combined with CPU and controlled physical writes,
  execute-protection transitions and native `ExecutableCodeCache` generation invalidation;
- exact dirty-journal visibility followed by forced journal wrap and conservative page-level
  fallback, proving written pages are never lost;
- LR/SC invalidation by overlapping fixed aliases and controlled DMA writes, plus survival
  for neighboring reservation granules;
- six independent guest-thread-shaped RAM workers racing a controlled physical DMA writer,
  followed by invariant validation.

Completion validation on Linux x86-64:

- GCC Release generic matrix: **34/34 tests passed** (1-16 completed before the command
  window expired; the resumed 17-34 segment passed 18/18);
- the hardening target also passed with alternate seeds `0x12345678` and `0xCAFEBABE`;
- Debug GCC ASan+UBSan: `xenon_memory_tests` passed at full scale before the wider command
  window expired, then the Phase 21 hardening target (10% sanitizer scale), executable-code
  cache tests and GPU-coherency tests all passed with leak detection and UBSan stack traces
  enabled;
- Clang 17 Release: full-scale Phase 21 hardening, core memory, executable-code cache and
  GPU-coherency tests passed; the benchmark target also built and ran successfully;
- GCC TSan: the Phase 21 hardening target passed at 10% sanitizer scale with
  `halt_on_error=1` and no reported race.

## 22.4 Final adversarial Xenia/recomp audit closure

A final post-compatibility scan specifically looked for behavior that could still leak through the
4 KiB implementation granularity or through read-only raw backing pointers. It found and closed
four narrow issues without changing the Memory V2 architecture:

- virtual/XEX `protect` and `decommit` normalize to the selected heap's architectural page size;
- `protect` rejects ranges crossing independent reservations, and `protect_physical` does the same
  while normalizing to the allocation's 4 KiB/64 KiB/16 MiB page class;
- free-memory queries now return the remaining free run rather than a zero-length region;
- all production Xenos CPU-side readers use atomic physical snapshots rather than `physical_data()`.

The audit also rechecked the unexplained 0x340000-byte 64 KiB physical reservation present in
Xenia/ReXGlue initialization. Current upstream still labels it `// ?`, and no public XDK/SDK
contract or hardware ownership rationale was found. Xenon therefore deliberately does **not** add
that unexplained reservation. If a title or kernel service later demonstrates a concrete Xbox
requirement, it should be added with that evidence and a regression test rather than cargo-culted
from an emulator implementation detail.

Final Linux x86-64 validation after these changes: full GCC Release CTest **34/34**, GCC
ASan+UBSan on memory/hardening/fault/SMC plus the affected Xenos common/frontend tests, GCC TSan
on the hardening snapshot race and GPU frontend, and Clang 17 Release on the same focused set.

## 23. Definition-of-done status

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
- Xbox/XDK-shaped physical page classes, bounds, cache/protection/query policy and fixed-alias propagation;
- reserve-only/no-zero virtual allocation policy plus allocation-identity query and byte-range decommit semantics;
- permanent low-64-KiB no-access and final-64-KiB physical reservations;
- host VM implementation separated from the canonical Xbox address-space model;
- six-thread normal-RAM stress coverage;
- compact six-slot PPC reservation monitor with physical-alias and six-thread contention coverage.

Memory V2 is **complete at the original definition-of-done checkpoint: 21/21 brief phases are closed**. The benchmark and invariant/fuzz harnesses are now part of the normal repository rather than external completion notes. This does not mean future game bring-up cannot reveal defects; any defect that violates one of these invariants should be fixed in Xenon Memory and accompanied by a regression test rather than creating game-specific memory behavior.

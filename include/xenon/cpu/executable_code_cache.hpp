#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "xenon/cpu/runtime.hpp"

namespace xenon::cpu {

using NativeGuestFunction =
    ExecutionResult (*)(CpuState&, MemoryPort&, RuntimeServices&);

struct ExecutableSourceSnapshot {
  GuestAddress base{};
  std::uint32_t size{};
  std::vector<ExecutablePageStamp> pages{};

  [[nodiscard]] bool valid() const noexcept {
    return size != 0u && !pages.empty();
  }
};

struct CompiledTranslation {
  GuestAddress entry{};
  NativeGuestFunction function{};
  ExecutableSourceSnapshot source{};
};

struct CompiledTranslationV2 {
  GuestAddress entry{};
  NativeCompiledEntry function{};
  ExecutableSourceSnapshot source{};
  bool direct_call_safe{};
};

// Native translation ownership for executable guest code.
//
// Normal RAM stores never call into this class. Instead, Xenon Memory advances
// sticky physical executable-page generations. A lookup compares the stored
// physical-page identity + generation snapshot with the current memory view and
// lazily evicts stale translations. This keeps the scalar write path callback-
// free while still making self-modifying code observable before dispatch.
//
// Dynamic translation is optional and callback-driven. The callback must return
// a source snapshot captured before/while decoding. Registration revalidates it
// after compilation, so a write racing compilation cannot bless stale native
// code. This cache owns native translations only; the Gen 7 dynamic PPC safety
// net is a separate ExecutionContext callback used after native lookup misses.
class ExecutableCodeCache {
 public:
  using CompileCallback =
      std::function<std::optional<CompiledTranslation>(GuestAddress target,
                                                       MemoryPort& memory)>;

  explicit ExecutableCodeCache(CompileCallback compiler = {});

  [[nodiscard]] static std::optional<ExecutableSourceSnapshot> capture_source(
      MemoryPort& memory, GuestAddress base, std::uint32_t size);

  // Register a precompiled/native translation against the executable bytes
  // currently mapped at [source_base, source_base + source_size). This is the
  // intended path for static recompilation modules during module activation.
  [[nodiscard]] bool register_current(
      MemoryPort& memory, GuestAddress entry, GuestAddress source_base,
      std::uint32_t source_size, NativeGuestFunction function);

  // CPU V2 native-entry registration. Unlike the compatibility entry above,
  // these functions reuse the caller's ExecutionContext/MemoryAccessContext
  // across indirect control transfers.
  [[nodiscard]] bool register_current_v2(
      MemoryPort& memory, GuestAddress entry, GuestAddress source_base,
      std::uint32_t source_size, NativeCompiledEntry function,
      bool direct_call_safe = false);

  // Register a translation produced from an explicit source snapshot. This is
  // the safe dynamic-code path: if the source changed during translation, the
  // registration is rejected and the caller may compile again.
  [[nodiscard]] bool register_compiled(MemoryPort& memory,
                                       const CompiledTranslation& translation);
  [[nodiscard]] bool register_compiled_v2(
      MemoryPort& memory, const CompiledTranslationV2& translation);

  // Return the native entry only if every source page still has the exact
  // physical identity and executable generation captured at registration.
  // Stale entries are removed before returning null.
  [[nodiscard]] NativeGuestFunction lookup(MemoryPort& memory,
                                           GuestAddress entry);

  // Lookup, then invoke the optional compiler on a miss/stale translation. A
  // compiler race is handled by snapshot revalidation rather than by freezing
  // normal memory writes.
  [[nodiscard]] NativeGuestFunction resolve(MemoryPort& memory,
                                            GuestAddress entry);

  // Fast CPU V2 lookup. Single-page translations are published into a small
  // lock-free direct-mapped cache. The current executable physical-page stamp
  // is still validated through Memory V2 before returning the native entry.
  // Misses/collisions/multi-page translations fall back to the authoritative
  // registry under the shared mutex.
  [[nodiscard]] NativeCompiledEntry lookup_v2(ExecutionContext& context,
                                              GuestAddress entry,
                                              CompiledLookupKind kind =
                                                  CompiledLookupKind::Branch);

  // Install this cache as the compiled-code registry used by generated CPU V2
  // code. The callback is non-virtual and reuses the active Memory V2 context.
  void bind(ExecutionContext& context) noexcept;

  // Convenience for a dispatcher/runtime implementation. std::nullopt means
  // no current native translation could be resolved. Callers may then invoke
  // the separately bound Gen 7 dynamic fallback through ExecutionContext.
  [[nodiscard]] std::optional<ExecutionResult> execute(
      GuestAddress entry, CpuState& state, MemoryPort& memory,
      RuntimeServices& runtime);
  [[nodiscard]] std::optional<ExecutionResult> execute_v2(
      GuestAddress entry, ExecutionContext& context);

  // Explicit invalidation remains useful for module unload/tooling. SMC writes
  // and icbi do not need it because generation mismatch invalidates lazily.
  void invalidate_guest_range(GuestAddress base, std::uint32_t size);
  void clear();

  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::uint64_t stale_evictions() const noexcept {
    return stale_evictions_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t compilation_rejections() const noexcept {
    return compilation_rejections_.load(std::memory_order_relaxed);
  }
  // Part 14 of the AC6 Runtime Readiness pass ("Runtime Fallback
  // Accounting"): the AOT-side half of the AOT-vs-fallback ratio. Counted on
  // every successful lookup()/lookup_v2() hit (the lock-free hot path
  // included) - a single relaxed atomic increment, so this stays cheap on
  // the hot compiled-code dispatch path.
  [[nodiscard]] std::uint64_t aot_lookup_hits() const noexcept {
    return aot_lookup_hits_.load(std::memory_order_relaxed);
  }

 private:
  struct Entry {
    GuestAddress entry{};
    NativeGuestFunction function{};
    NativeCompiledEntry function_v2{};
    ExecutableSourceSnapshot source{};
    bool direct_call_safe{};
    std::uint64_t serial{};
  };

  struct HotEntry {
    // guest_key is the publication word. Writers clear it before modifying
    // the remaining atomics and publish it last with release semantics.
    std::atomic<std::uint64_t> guest_key{};
    std::atomic<NativeCompiledEntry> function{};
    std::atomic<std::uint64_t> page_stamp{};
    std::atomic<std::uint32_t> flags{};
  };

  static constexpr std::size_t kHotEntryCount = 256u;

  [[nodiscard]] static bool snapshot_matches(
      MemoryPort& memory, const ExecutableSourceSnapshot& snapshot) noexcept;
  [[nodiscard]] static bool snapshot_matches(
      MemoryAccessContext& memory,
      const ExecutableSourceSnapshot& snapshot) noexcept;
  [[nodiscard]] static NativeCompiledEntry lookup_callback(
      void* registry, ExecutionContext& context, GuestAddress target,
      CompiledLookupKind kind);
  [[nodiscard]] static std::uint64_t encode_stamp(
      const ExecutablePageStamp& stamp) noexcept;
  [[nodiscard]] static ExecutablePageStamp decode_stamp(
      std::uint64_t stamp) noexcept;
  [[nodiscard]] static std::size_t hot_index(GuestAddress entry) noexcept;
  void publish_hot(const Entry& entry) noexcept;
  void clear_hot(GuestAddress entry) noexcept;
  void clear_hot_all() noexcept;
  [[nodiscard]] static bool ranges_overlap(GuestAddress a,
                                           std::uint32_t a_size,
                                           GuestAddress b,
                                           std::uint32_t b_size) noexcept;

  mutable std::shared_mutex mutex_{};
  std::unordered_map<GuestAddress, Entry> entries_{};
  std::array<HotEntry, kHotEntryCount> hot_entries_{};
  CompileCallback compiler_{};
  std::uint64_t next_serial_{1u};
  std::atomic<std::uint64_t> stale_evictions_{0u};
  std::atomic<std::uint64_t> compilation_rejections_{0u};
  std::atomic<std::uint64_t> aot_lookup_hits_{0u};
};

}  // namespace xenon::cpu

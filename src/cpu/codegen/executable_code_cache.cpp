#include "xenon/cpu/executable_code_cache.hpp"

#include <algorithm>
#include <limits>
#include <mutex>

namespace xenon::cpu {
namespace {
constexpr std::uint32_t kGuestPageSize = 0x1000u;
constexpr std::uint32_t kGuestPageMask = kGuestPageSize - 1u;

[[nodiscard]] bool valid_range(GuestAddress base, std::uint32_t size) noexcept {
  if (!size) return false;
  return std::uint64_t{base} + size <= (std::uint64_t{1} << 32u);
}
}  // namespace

ExecutableCodeCache::ExecutableCodeCache(CompileCallback compiler)
    : compiler_(std::move(compiler)) {}

std::optional<ExecutableSourceSnapshot> ExecutableCodeCache::capture_source(
    MemoryPort& memory, GuestAddress base, std::uint32_t size) {
  if (!valid_range(base, size)) return std::nullopt;

  const auto first_page = base & ~kGuestPageMask;
  const auto last_address = static_cast<GuestAddress>(
      std::uint64_t{base} + size - 1u);
  const auto last_page = last_address & ~kGuestPageMask;
  const auto page_count =
      ((last_page - first_page) / kGuestPageSize) + 1u;

  ExecutableSourceSnapshot snapshot{};
  snapshot.base = base;
  snapshot.size = size;
  snapshot.pages.reserve(page_count);
  for (std::uint32_t i = 0; i < page_count; ++i) {
    const auto page = first_page + i * kGuestPageSize;
    const auto stamp = memory.executable_page_stamp(page);
    if (!stamp.executable()) return std::nullopt;
    snapshot.pages.push_back(stamp);
  }
  return snapshot;
}

bool ExecutableCodeCache::snapshot_matches(
    MemoryPort& memory, const ExecutableSourceSnapshot& snapshot) noexcept {
  if (!snapshot.valid() || !valid_range(snapshot.base, snapshot.size)) {
    return false;
  }
  const auto first_page = snapshot.base & ~kGuestPageMask;
  const auto last_address = static_cast<GuestAddress>(
      std::uint64_t{snapshot.base} + snapshot.size - 1u);
  const auto last_page = last_address & ~kGuestPageMask;
  const auto page_count =
      ((last_page - first_page) / kGuestPageSize) + 1u;
  if (snapshot.pages.size() != page_count) return false;

  for (std::uint32_t i = 0; i < page_count; ++i) {
    const auto page = first_page + i * kGuestPageSize;
    if (memory.executable_page_stamp(page) != snapshot.pages[i]) return false;
  }
  return true;
}

bool ExecutableCodeCache::snapshot_matches(
    MemoryAccessContext& memory,
    const ExecutableSourceSnapshot& snapshot) noexcept {
  if (!snapshot.valid() || !valid_range(snapshot.base, snapshot.size)) {
    return false;
  }
  const auto first_page = snapshot.base & ~kGuestPageMask;
  const auto last_address = static_cast<GuestAddress>(
      std::uint64_t{snapshot.base} + snapshot.size - 1u);
  const auto last_page = last_address & ~kGuestPageMask;
  const auto page_count =
      ((last_page - first_page) / kGuestPageSize) + 1u;
  if (snapshot.pages.size() != page_count) return false;

  for (std::uint32_t i = 0; i < page_count; ++i) {
    const auto page = first_page + i * kGuestPageSize;
    if (memory.executable_page_stamp(page) != snapshot.pages[i]) return false;
  }
  return true;
}

std::uint64_t ExecutableCodeCache::encode_stamp(
    const ExecutablePageStamp& stamp) noexcept {
  return (std::uint64_t{stamp.physical_page} << 32u) |
         std::uint64_t{stamp.generation};
}

ExecutablePageStamp ExecutableCodeCache::decode_stamp(
    std::uint64_t stamp) noexcept {
  return {static_cast<std::uint32_t>(stamp >> 32u),
          static_cast<std::uint32_t>(stamp)};
}

std::size_t ExecutableCodeCache::hot_index(GuestAddress entry) noexcept {
  // Code is four-byte aligned; ignore those guaranteed-zero bits and mix a
  // little of the page number so adjacent function tables do not all alias.
  const auto value = (entry >> 2u) ^ (entry >> 12u) ^ (entry >> 20u);
  return static_cast<std::size_t>(value) & (kHotEntryCount - 1u);
}

void ExecutableCodeCache::publish_hot(const Entry& entry) noexcept {
  if (!entry.function_v2 || entry.source.pages.size() != 1u) return;
  auto& hot = hot_entries_[hot_index(entry.entry)];
  hot.guest_key.store(0u, std::memory_order_release);
  hot.function.store(entry.function_v2, std::memory_order_relaxed);
  hot.page_stamp.store(encode_stamp(entry.source.pages.front()),
                       std::memory_order_relaxed);
  hot.flags.store(entry.direct_call_safe ? 1u : 0u,
                  std::memory_order_relaxed);
  // +1 reserves zero as an unpublished slot while supporting guest address 0.
  hot.guest_key.store(std::uint64_t{entry.entry} + 1u,
                      std::memory_order_release);
}

void ExecutableCodeCache::clear_hot(GuestAddress entry) noexcept {
  auto& hot = hot_entries_[hot_index(entry)];
  const auto key = hot.guest_key.load(std::memory_order_acquire);
  if (key == std::uint64_t{entry} + 1u)
    hot.guest_key.store(0u, std::memory_order_release);
}

void ExecutableCodeCache::clear_hot_all() noexcept {
  for (auto& hot : hot_entries_)
    hot.guest_key.store(0u, std::memory_order_release);
}

bool ExecutableCodeCache::register_current(
    MemoryPort& memory, GuestAddress entry, GuestAddress source_base,
    std::uint32_t source_size, NativeGuestFunction function) {
  if (!function) return false;
  auto snapshot = capture_source(memory, source_base, source_size);
  if (!snapshot) return false;
  return register_compiled(
      memory, CompiledTranslation{entry, function, std::move(*snapshot)});
}

bool ExecutableCodeCache::register_current_v2(
    MemoryPort& memory, GuestAddress entry, GuestAddress source_base,
    std::uint32_t source_size, NativeCompiledEntry function,
    bool direct_call_safe) {
  if (!function) return false;
  auto snapshot = capture_source(memory, source_base, source_size);
  if (!snapshot) return false;
  return register_compiled_v2(
      memory, CompiledTranslationV2{entry, function, std::move(*snapshot),
                                    direct_call_safe});
}

bool ExecutableCodeCache::register_compiled(
    MemoryPort& memory, const CompiledTranslation& translation) {
  if (!translation.function || !translation.source.valid() ||
      !snapshot_matches(memory, translation.source)) {
    compilation_rejections_.fetch_add(1u, std::memory_order_relaxed);
    return false;
  }
  const auto source_end =
      std::uint64_t{translation.source.base} + translation.source.size;
  if (translation.entry < translation.source.base ||
      std::uint64_t{translation.entry} >= source_end) {
    compilation_rejections_.fetch_add(1u, std::memory_order_relaxed);
    return false;
  }

  std::unique_lock lock(mutex_);
  auto serial = next_serial_++;
  if (!serial) serial = next_serial_++;
  entries_.insert_or_assign(
      translation.entry,
      Entry{translation.entry, translation.function, nullptr,
            translation.source, false,
            serial});
  clear_hot(translation.entry);
  return true;
}

bool ExecutableCodeCache::register_compiled_v2(
    MemoryPort& memory, const CompiledTranslationV2& translation) {
  if (!translation.function || !translation.source.valid() ||
      !snapshot_matches(memory, translation.source)) {
    compilation_rejections_.fetch_add(1u, std::memory_order_relaxed);
    return false;
  }
  const auto source_end =
      std::uint64_t{translation.source.base} + translation.source.size;
  if (translation.entry < translation.source.base ||
      std::uint64_t{translation.entry} >= source_end) {
    compilation_rejections_.fetch_add(1u, std::memory_order_relaxed);
    return false;
  }

  std::unique_lock lock(mutex_);
  auto serial = next_serial_++;
  if (!serial) serial = next_serial_++;
  auto [it, inserted] = entries_.insert_or_assign(
      translation.entry,
      Entry{translation.entry, nullptr, translation.function,
            translation.source, translation.direct_call_safe, serial});
  static_cast<void>(inserted);
  publish_hot(it->second);
  return true;
}

NativeGuestFunction ExecutableCodeCache::lookup(MemoryPort& memory,
                                                 GuestAddress entry) {
  std::uint64_t serial = 0u;
  {
    std::shared_lock lock(mutex_);
    const auto it = entries_.find(entry);
    if (it == entries_.end()) return nullptr;
    if (snapshot_matches(memory, it->second.source)) {
      return it->second.function;
    }
    serial = it->second.serial;
  }

  std::unique_lock lock(mutex_);
  const auto it = entries_.find(entry);
  if (it != entries_.end() && it->second.serial == serial &&
      !snapshot_matches(memory, it->second.source)) {
    entries_.erase(it);
    stale_evictions_.fetch_add(1u, std::memory_order_relaxed);
  }
  return nullptr;
}

NativeGuestFunction ExecutableCodeCache::resolve(MemoryPort& memory,
                                                  GuestAddress entry) {
  if (auto* function = lookup(memory, entry)) return function;
  if (!compiler_) return nullptr;

  // A compiler may race SMC. It must capture the source snapshot used for
  // decoding; register_compiled rejects the result if any source page changed.
  auto compiled = compiler_(entry, memory);
  if (!compiled || compiled->entry != entry) return nullptr;
  if (!register_compiled(memory, *compiled)) return nullptr;
  return lookup(memory, entry);
}

NativeCompiledEntry ExecutableCodeCache::lookup_v2(ExecutionContext& context,
                                                    GuestAddress entry,
                                                    CompiledLookupKind kind) {
  // Lock-free direct-mapped fast path. Only single-page translations are
  // published here, so one executable-page stamp proves the complete source
  // snapshot. Re-read the key after the payload to reject concurrent publish.
  auto& hot = hot_entries_[hot_index(entry)];
  const auto wanted_key = std::uint64_t{entry} + 1u;
  const auto key_before = hot.guest_key.load(std::memory_order_acquire);
  if (key_before == wanted_key) {
    const auto function = hot.function.load(std::memory_order_relaxed);
    const auto stamp_bits = hot.page_stamp.load(std::memory_order_relaxed);
    const auto flags = hot.flags.load(std::memory_order_relaxed);
    const auto key_after = hot.guest_key.load(std::memory_order_acquire);
    const auto call_allowed =
        kind != CompiledLookupKind::Call || (flags & 1u) != 0u;
    if (key_after == key_before && function && call_allowed) {
      const auto current = context.memory_access.executable_page_stamp(entry);
      if (current == decode_stamp(stamp_bits)) {
        return function;
      }
    }
  }

  std::uint64_t serial = 0u;
  {
    std::shared_lock lock(mutex_);
    const auto it = entries_.find(entry);
    if (it == entries_.end() || !it->second.function_v2) return nullptr;
    if (kind == CompiledLookupKind::Call && !it->second.direct_call_safe)
      return nullptr;
    if (snapshot_matches(context.memory_access, it->second.source)) {
      publish_hot(it->second);
      return it->second.function_v2;
    }
    serial = it->second.serial;
  }

  std::unique_lock lock(mutex_);
  const auto it = entries_.find(entry);
  if (it != entries_.end() && it->second.serial == serial &&
      !snapshot_matches(context.memory_access, it->second.source)) {
    clear_hot(entry);
    entries_.erase(it);
    stale_evictions_.fetch_add(1u, std::memory_order_relaxed);
  }
  return nullptr;
}

NativeCompiledEntry ExecutableCodeCache::lookup_callback(
    void* registry, ExecutionContext& context, GuestAddress target,
    CompiledLookupKind kind) {
  if (!registry) return nullptr;
  return static_cast<ExecutableCodeCache*>(registry)->lookup_v2(context,
                                                                target, kind);
}

void ExecutableCodeCache::bind(ExecutionContext& context) noexcept {
  context.compiled_registry = this;
  context.compiled_lookup = &ExecutableCodeCache::lookup_callback;
}

std::optional<ExecutionResult> ExecutableCodeCache::execute(
    GuestAddress entry, CpuState& state, MemoryPort& memory,
    RuntimeServices& runtime) {
  auto* function = resolve(memory, entry);
  if (!function) return std::nullopt;
  return function(state, memory, runtime);
}

std::optional<ExecutionResult> ExecutableCodeCache::execute_v2(
    GuestAddress entry, ExecutionContext& context) {
  auto* function = lookup_v2(context, entry);
  if (!function) return std::nullopt;
  return function(context);
}

bool ExecutableCodeCache::ranges_overlap(GuestAddress a,
                                         std::uint32_t a_size,
                                         GuestAddress b,
                                         std::uint32_t b_size) noexcept {
  if (!a_size || !b_size) return false;
  const auto a_begin = std::uint64_t{a};
  const auto b_begin = std::uint64_t{b};
  const auto a_end = std::min<std::uint64_t>(a_begin + a_size,
                                              std::uint64_t{1} << 32u);
  const auto b_end = std::min<std::uint64_t>(b_begin + b_size,
                                              std::uint64_t{1} << 32u);
  return a_begin < b_end && b_begin < a_end;
}

void ExecutableCodeCache::invalidate_guest_range(GuestAddress base,
                                                  std::uint32_t size) {
  if (!size) return;
  std::unique_lock lock(mutex_);
  std::erase_if(entries_, [&](const auto& pair) {
    const auto erase = ranges_overlap(pair.second.source.base,
                                      pair.second.source.size, base, size);
    if (erase) clear_hot(pair.first);
    return erase;
  });
}

void ExecutableCodeCache::clear() {
  std::unique_lock lock(mutex_);
  entries_.clear();
  clear_hot_all();
}

std::size_t ExecutableCodeCache::size() const {
  std::shared_lock lock(mutex_);
  return entries_.size();
}

}  // namespace xenon::cpu

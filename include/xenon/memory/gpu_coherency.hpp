#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <vector>

#include "xenon/memory/coherency.hpp"

namespace xenon::memory {

struct GpuCoherencyRange {
  std::uint32_t address{};
  std::uint32_t size{};

  [[nodiscard]] std::uint64_t end() const noexcept {
    return std::uint64_t{address} + size;
  }
};

struct GpuCpuVisibilityRange {
  std::uint32_t address{};
  std::uint32_t size{};
  xenon::cpu::MemoryOrderingDomain ordering_domain{
      xenon::cpu::MemoryOrderingDomain::Normal};

  [[nodiscard]] std::uint64_t end() const noexcept {
    return std::uint64_t{address} + size;
  }
};

// A GPU-authored range carries the generation of the write that owns it.
// Generations are range-local once stored here: an unrelated later GPU write
// must not invalidate a readback plan for an older, non-overlapping range.
struct GpuReadbackRange {
  std::uint32_t address{};
  std::uint32_t size{};
  std::uint64_t generation{};

  [[nodiscard]] std::uint64_t end() const noexcept {
    return std::uint64_t{address} + size;
  }
};

// Describes how the host GPU reaches Xbox physical memory. Current desktop
// Vulkan/D3D12 backends use a device-local mirror. A future UMA/mobile backend
// may map Xenon's shared physical backing directly and execute visibility/cache
// operations without redundant CPU<->GPU copies.
enum class GpuMemoryTopology : std::uint8_t {
  DiscreteMirror,
  SharedHostVisible,
};

enum class GpuSynchronizationAction : std::uint8_t {
  Copy,
  VisibilityOnly,
};

enum class GpuRangeUsage : std::uint8_t {
  Generic,
  Texture,
  VertexBuffer,
  IndexBuffer,
  Shader,
  CommandData,
  MemoryExport,
  RenderReadback,
  Unrestricted,
};

struct GpuUploadPlan {
  std::uint64_t through_epoch{};
  bool exact_history{};
  GpuSynchronizationAction action{GpuSynchronizationAction::Copy};
  GpuRangeUsage usage{GpuRangeUsage::Generic};
  // Strongest guest cache/order domain that produced bytes in this upload.
  // Discrete mirrors still copy normally; shared-host-visible backends may use
  // this to choose the appropriate cache maintenance / visibility primitive.
  xenon::cpu::MemoryOrderingDomain source_domain{
      xenon::cpu::MemoryOrderingDomain::Normal};
  std::vector<GpuCoherencyRange> ranges{};
};

struct GpuReadbackPlan {
  // CPU/DMA publications through this epoch were reconciled before the GPU
  // ranges were selected. A readback commit reconciles any newer publications
  // again while AddressSpace holds its short physical-write quiescence window.
  std::uint64_t through_epoch{};
  bool exact_history{};
  GpuSynchronizationAction action{GpuSynchronizationAction::Copy};
  GpuRangeUsage usage{GpuRangeUsage::RenderReadback};
  std::vector<GpuReadbackRange> ranges{};
};

// Backend-neutral ownership and synchronization planning for Xbox physical
// memory visible to a GPU. Native APIs execute copies/barriers/cache operations;
// Xenon Memory owns which bytes are newer, which ranges are device-valid, CPU
// publication epochs and GPU generations.
class GuestMemoryGpuCoherency {
 public:
  void reset(std::uint32_t size, bool cpu_initially_dirty = true,
             GpuMemoryTopology topology = GpuMemoryTopology::DiscreteMirror) {
    std::lock_guard lock(mutex_);
    size_ = size;
    topology_ = topology;
    synchronized_epoch_ = 0;
    gpu_generation_ = 0;
    cpu_dirty_.clear();
    cpu_visibility_.clear();
    gpu_dirty_.clear();
    device_valid_.clear();
    requested_.clear();
    touched_pages_.clear();
    if (cpu_initially_dirty && size_) {
      cpu_dirty_.push_back({0, size_});
      cpu_visibility_.push_back(
          {0, size_, xenon::cpu::MemoryOrderingDomain::Normal});
    }
  }

  void clear() noexcept {
    std::lock_guard lock(mutex_);
    size_ = 0;
    topology_ = GpuMemoryTopology::DiscreteMirror;
    synchronized_epoch_ = 0;
    gpu_generation_ = 0;
    cpu_dirty_.clear();
    cpu_visibility_.clear();
    gpu_dirty_.clear();
    device_valid_.clear();
    requested_.clear();
    touched_pages_.clear();
  }

  [[nodiscard]] GpuUploadPlan plan_upload(
      const GuestMemoryCoherency& memory, std::uint32_t address = 0,
      std::uint32_t size = (std::numeric_limits<std::uint32_t>::max)(),
      std::uint32_t max_range_bytes = 0,
      GpuRangeUsage usage = GpuRangeUsage::Generic) {
    GpuUploadPlan plan{};
    plan.through_epoch = memory.current_epoch();
    plan.usage = usage;

    std::lock_guard lock(mutex_);
    plan.action = synchronization_action_unlocked();
    plan.exact_history = ingest_cpu_writes_unlocked(
        memory, plan.through_epoch, 0u, max_range_bytes);
    const auto request = clamp(address, size);
    if (!request.size) return plan;
    add(requested_, request);
    touch_pages_unlocked(request);

    // Upload only CPU-new bytes plus parts of the requested range that have
    // never been made valid on the device. This lazily initializes untouched
    // RAM (normally zero-filled by Xenon) without treating all 512 MiB as dirty.
    plan.ranges = intersections(cpu_dirty_, request);
    add_all(plan.ranges, uncovered_ranges(device_valid_, request));
    plan.source_domain = strongest_visibility_domain(cpu_visibility_, plan.ranges);
    if (max_range_bytes) split_ranges(plan.ranges, max_range_bytes);
    return plan;
  }

  void commit_cpu_upload(std::uint32_t address, std::uint32_t size) {
    std::lock_guard lock(mutex_);
    const auto range = clamp(address, size);
    touch_pages_unlocked(range);
    subtract(cpu_dirty_, range);
    subtract(cpu_visibility_, range);
    add(device_valid_, range);
  }

  void rollback_cpu_upload(std::uint32_t address, std::uint32_t size) {
    std::lock_guard lock(mutex_);
    const auto range = clamp(address, size);
    if (!range.size) return;
    touch_pages_unlocked(range);
    // The upload never became device-valid. Preserve the cache/order domain
    // already attached to the CPU publication rather than reclassifying it as
    // ordinary cached RAM.
    subtract(device_valid_, range);
    add(cpu_dirty_, range);
  }

  void mark_cpu_write(
      std::uint32_t address, std::uint32_t size,
      xenon::cpu::MemoryOrderingDomain ordering_domain =
          xenon::cpu::MemoryOrderingDomain::Normal) {
    std::lock_guard lock(mutex_);
    mark_cpu_write_unlocked(address, size, ordering_domain);
  }

  // Returns the generation assigned to this GPU-authored ownership change.
  // Overlapping old GPU generations are replaced byte-precisely while
  // non-overlapping generations remain independently committable.
  std::uint64_t mark_gpu_write(std::uint32_t address, std::uint32_t size) {
    std::lock_guard lock(mutex_);
    const auto range = clamp(address, size);
    if (!range.size) return gpu_generation_;
    touch_pages_unlocked(range);
    ++gpu_generation_;
    if (!gpu_generation_) ++gpu_generation_;
    subtract(cpu_dirty_, range);
    subtract(cpu_visibility_, range);
    assign_gpu_range(gpu_dirty_,
                     {range.address, range.size, gpu_generation_});
    add(device_valid_, range);
    return gpu_generation_;
  }

  // Reconcile CPU/DMA writes before selecting GPU-owned bytes. This prevents a
  // CPU write that happened after mark_gpu_write but before readback planning
  // from being overwritten by stale device data.
  [[nodiscard]] GpuReadbackPlan plan_readback(
      const GuestMemoryCoherency& memory, std::uint32_t address,
      std::uint32_t size, std::uint32_t max_range_bytes = 0,
      GpuRangeUsage usage = GpuRangeUsage::RenderReadback) {
    GpuReadbackPlan plan{};
    plan.through_epoch = memory.current_epoch();
    plan.usage = usage;

    std::lock_guard lock(mutex_);
    plan.action = synchronization_action_unlocked();
    plan.exact_history = ingest_cpu_writes_unlocked(
        memory, plan.through_epoch, 0u, max_range_bytes);
    const auto request = clamp(address, size);
    if (!request.size) return plan;
    add(requested_, request);
    touch_pages_unlocked(request);
    plan.ranges = intersections(gpu_dirty_, request);
    if (max_range_bytes) split_ranges(plan.ranges, max_range_bytes);
    return plan;
  }

  // Called after the native backend has copied a planned GPU range into a
  // staging/readback buffer and AddressSpace has acquired its short exclusive
  // physical-write window. CPU writes that occurred since planning are ingested
  // here and removed from GPU ownership. Only bytes still owned by the exact
  // planned GPU generation may be written into physical RAM.
  [[nodiscard]] std::vector<GpuReadbackRange> prepare_gpu_download(
      const GuestMemoryCoherency& memory, const GpuReadbackRange& planned,
      std::uint32_t max_range_bytes = 0) {
    const auto through_epoch = memory.current_epoch();
    std::lock_guard lock(mutex_);
    (void)ingest_cpu_writes_unlocked(memory, through_epoch, 0u,
                                     max_range_bytes);
    auto ranges = intersections_generation(
        gpu_dirty_, {planned.address, planned.size}, planned.generation);
    if (max_range_bytes) split_ranges(ranges, max_range_bytes);
    return ranges;
  }

  // Completes one safe GPU->CPU visibility subrange. publication_epoch is the
  // exact GuestMemoryCoherency epoch produced when a discrete mirror copied the
  // bytes into Xenon physical RAM. That self-publication is ignored by this
  // mirror while unrelated CPU/DMA writes in the same interval are retained.
  //
  // The old GPU generation is cleared only where it still owns the bytes. A
  // newer overlapping GPU write therefore survives an older readback commit.
  [[nodiscard]] bool commit_gpu_download(
      const GuestMemoryCoherency& memory, const GpuReadbackRange& planned,
      std::uint32_t address, std::uint32_t size,
      std::uint64_t publication_epoch = 0) {
    std::lock_guard lock(mutex_);
    const auto range = clamp(address, size);
    if (!range.size) return true;

    bool exact_acknowledgement = true;
    if (publication_epoch) {
      exact_acknowledgement = ingest_cpu_writes_unlocked(
          memory, publication_epoch, publication_epoch, 0u);
    }

    const auto removed = subtract_generation(gpu_dirty_, range,
                                             planned.generation);
    add(device_valid_, range);
    for (const auto& cpu_range : intersections(cpu_dirty_, range)) {
      subtract(device_valid_, cpu_range);
    }

    return exact_acknowledgement && removed == range.size;
  }

  [[nodiscard]] bool has_gpu_dirty(std::uint32_t address,
                                   std::uint32_t size) const {
    std::lock_guard lock(mutex_);
    return overlaps(gpu_dirty_, clamp(address, size));
  }

  [[nodiscard]] bool device_range_valid(std::uint32_t address,
                                        std::uint32_t size) const {
    std::lock_guard lock(mutex_);
    const auto query = clamp(address, size);
    if (!query.size) return false;
    const auto covered = intersections(device_valid_, query);
    return covered.size() == 1u && covered[0].address == query.address &&
           covered[0].size == query.size;
  }

  [[nodiscard]] std::vector<GpuCoherencyRange> requested_ranges() const {
    std::lock_guard lock(mutex_);
    return requested_;
  }

  [[nodiscard]] std::vector<GpuCoherencyRange> touched_page_ranges() const {
    std::lock_guard lock(mutex_);
    return touched_pages_;
  }

  [[nodiscard]] std::uint64_t synchronized_epoch() const {
    std::lock_guard lock(mutex_);
    return synchronized_epoch_;
  }

  [[nodiscard]] std::uint64_t gpu_generation() const {
    std::lock_guard lock(mutex_);
    return gpu_generation_;
  }

  [[nodiscard]] GpuMemoryTopology topology() const {
    std::lock_guard lock(mutex_);
    return topology_;
  }

  // Compatibility helpers retained for tests/tooling and non-native consumers.
  void mark_cpu_uploaded(std::uint32_t address, std::uint32_t size) {
    commit_cpu_upload(address, size);
  }
  void mark_gpu_downloaded(std::uint32_t address, std::uint32_t size) {
    std::lock_guard lock(mutex_);
    const auto range = clamp(address, size);
    subtract(gpu_dirty_, range);
    add(device_valid_, range);
    for (const auto& cpu_range : intersections(cpu_dirty_, range)) {
      subtract(device_valid_, cpu_range);
    }
  }
  [[nodiscard]] std::vector<GpuCoherencyRange> cpu_dirty_ranges(
      std::uint32_t address = 0,
      std::uint32_t size = (std::numeric_limits<std::uint32_t>::max)()) const {
    std::lock_guard lock(mutex_);
    return intersections(cpu_dirty_, clamp(address, size));
  }
  [[nodiscard]] std::vector<GpuCoherencyRange> gpu_dirty_ranges(
      std::uint32_t address = 0,
      std::uint32_t size = (std::numeric_limits<std::uint32_t>::max)()) const {
    std::lock_guard lock(mutex_);
    const auto owned = intersections(gpu_dirty_, clamp(address, size));
    std::vector<GpuCoherencyRange> result;
    result.reserve(owned.size());
    for (const auto& range : owned) result.push_back({range.address, range.size});
    return result;
  }

 private:
  [[nodiscard]] GpuSynchronizationAction synchronization_action_unlocked() const
      noexcept {
    return topology_ == GpuMemoryTopology::SharedHostVisible
               ? GpuSynchronizationAction::VisibilityOnly
               : GpuSynchronizationAction::Copy;
  }

  // Pulls CPU/DMA publications from the canonical Xenon journal through a
  // stable epoch. ignored_epoch is used only for a mirror's own CPU writeback.
  // Returns false when byte-exact journal history was unavailable and a
  // conservative page-level fallback had to be used.
  bool ingest_cpu_writes_unlocked(const GuestMemoryCoherency& memory,
                                  std::uint64_t through_epoch,
                                  std::uint64_t ignored_epoch,
                                  std::uint32_t max_range_bytes) {
    if (through_epoch <= synchronized_epoch_) return true;

    std::vector<DirtyPhysicalWrite> exact_writes;
    const bool exact = memory.collect_exact_dirty_writes(
        synchronized_epoch_, through_epoch, exact_writes);
    if (exact) {
      if (ignored_epoch) {
        std::erase_if(exact_writes, [&](const DirtyPhysicalWrite& write) {
          return write.epoch == ignored_epoch;
        });
      }
      // A frame of guest stores is tens of thousands of small writes, and
      // applying them one at a time rebuilt (and re-sorted) every set per
      // write: AC6's mix of write-combined vertex data and cached stores
      // spent half the GPU thread there. The set updates commute; only each
      // byte's ordering domain depends on order, and the last write wins.
      mark_cpu_writes_unlocked(last_writer_runs(exact_writes));
    } else {
      std::vector<DirtyPhysicalRange> dirty_pages;
      memory.collect_dirty_ranges(synchronized_epoch_, through_epoch,
                                  max_range_bytes, dirty_pages);
      // Exact cache-type identity is unavailable after journal overflow.
      // Preserve correctness by treating the fallback as cache-inhibited,
      // the strongest portable guest visibility domain. The pages come back
      // sorted and disjoint, so they apply in one merge pass.
      std::vector<GpuCpuVisibilityRange> runs;
      runs.reserve(dirty_pages.size());
      for (const auto& write : dirty_pages)
        runs.push_back({write.address, write.size,
                        xenon::cpu::MemoryOrderingDomain::CacheInhibited});
      mark_cpu_writes_unlocked(runs);
    }
    synchronized_epoch_ = through_epoch;
    return exact;
  }

  // The writes, given in publication order, as address-sorted disjoint runs
  // in which every byte carries the ordering domain of the last write to it:
  // the state applying them one at a time would leave.
  [[nodiscard]] static std::vector<GpuCpuVisibilityRange> last_writer_runs(
      const std::vector<DirtyPhysicalWrite>& writes) {
    std::vector<GpuCpuVisibilityRange> pieces;
    if (writes.empty()) return pieces;
    const bool one_domain = std::all_of(
        writes.begin(), writes.end(), [&](const DirtyPhysicalWrite& write) {
          return write.ordering_domain == writes.front().ordering_domain;
        });
    if (one_domain) {
      pieces.reserve(writes.size());
      for (const auto& write : writes)
        if (write.size) pieces.push_back({write.address, write.size, write.ordering_domain});
      std::sort(pieces.begin(), pieces.end(),
                [](const auto& a, const auto& b) { return a.address < b.address; });
    } else {
      // Newest first: a byte belongs to the first write that reaches it.
      std::map<std::uint64_t, std::uint64_t> covered;  // start -> end, disjoint
      for (auto write = writes.rbegin(); write != writes.rend(); ++write) {
        const std::uint64_t start = write->address;
        const std::uint64_t end = start + write->size;
        if (start == end) continue;
        auto next = covered.upper_bound(start);
        if (next != covered.begin() && std::prev(next)->second >= start) --next;
        std::uint64_t cursor = start, merged_start = start, merged_end = end;
        while (next != covered.end() && next->first <= end) {
          if (next->first > cursor)
            pieces.push_back({static_cast<std::uint32_t>(cursor),
                              static_cast<std::uint32_t>(next->first - cursor),
                              write->ordering_domain});
          cursor = (std::max)(cursor, next->second);
          merged_start = (std::min)(merged_start, next->first);
          merged_end = (std::max)(merged_end, next->second);
          next = covered.erase(next);
        }
        if (cursor < end)
          pieces.push_back({static_cast<std::uint32_t>(cursor),
                            static_cast<std::uint32_t>(end - cursor),
                            write->ordering_domain});
        covered.emplace(merged_start, merged_end);
      }
      std::sort(pieces.begin(), pieces.end(),
                [](const auto& a, const auto& b) { return a.address < b.address; });
    }
    // Coalesce overlapping (one domain) and touching same-domain pieces.
    std::vector<GpuCpuVisibilityRange> runs;
    runs.reserve(pieces.size());
    for (const auto& piece : pieces) {
      if (!runs.empty() && piece.address <= runs.back().end() &&
          (piece.address < runs.back().end() ||
           piece.ordering_domain == runs.back().ordering_domain)) {
        runs.back().size = static_cast<std::uint32_t>(
            (std::max)(runs.back().end(), piece.end()) - runs.back().address);
      } else {
        runs.push_back(piece);
      }
    }
    return runs;
  }

  [[nodiscard]] GpuCoherencyRange clamp(std::uint32_t address,
                                        std::uint32_t size) const noexcept {
    if (!size || address >= size_) return {};
    const auto end = (std::min)(std::uint64_t{address} + size,
                                std::uint64_t{size_});
    return {address, static_cast<std::uint32_t>(end - address)};
  }

  void mark_cpu_write_unlocked(
      std::uint32_t address, std::uint32_t size,
      xenon::cpu::MemoryOrderingDomain ordering_domain) {
    const auto range = clamp(address, size);
    if (!range.size) return;
    touch_pages_unlocked(range);
    subtract(gpu_dirty_, range);
    subtract(device_valid_, range);
    add(cpu_dirty_, range);
    assign_cpu_visibility_range(
        cpu_visibility_, {range.address, range.size, ordering_domain});
  }

  // mark_cpu_write_unlocked() for many writes at once: address-sorted,
  // disjoint runs, each with the ordering domain it ends up in. Each set is
  // rebuilt in one merge pass instead of once per write: a frame of guest
  // stores across all of memory is thousands of runs against sets thousands
  // of ranges long, and per-write rebuilding made planning one 84-byte upload
  // take tens of milliseconds.
  void mark_cpu_writes_unlocked(const std::vector<GpuCpuVisibilityRange>& sorted_runs) {
    std::vector<GpuCpuVisibilityRange> domains;  // clamped, with their domains
    std::vector<GpuCoherencyRange> runs;         // the same bytes, coalesced
    std::vector<GpuCoherencyRange> pages;
    domains.reserve(sorted_runs.size());
    runs.reserve(sorted_runs.size());
    pages.reserve(sorted_runs.size());
    for (const auto& run : sorted_runs) {
      const auto range = clamp(run.address, run.size);
      if (!range.size) continue;
      domains.push_back({range.address, range.size, run.ordering_domain});
      if (!runs.empty() && runs.back().end() >= range.address) {
        runs.back().size = static_cast<std::uint32_t>(
            (std::max)(runs.back().end(), range.end()) - runs.back().address);
      } else {
        runs.push_back(range);
      }
      const auto first = std::uint64_t{range.address & ~(kBasePageSize - 1u)};
      const auto end = (std::min)(std::uint64_t{size_},
                                  (range.end() + kBasePageSize - 1u) &
                                      ~std::uint64_t{kBasePageSize - 1u});
      if (!pages.empty() && pages.back().end() >= first) {
        pages.back().size = static_cast<std::uint32_t>(
            (std::max)(pages.back().end(), end) - pages.back().address);
      } else if (end > first) {
        pages.push_back({static_cast<std::uint32_t>(first),
                         static_cast<std::uint32_t>(end - first)});
      }
    }
    if (runs.empty()) return;
    add_all(touched_pages_, pages);
    subtract_all(gpu_dirty_, runs);
    subtract_all(device_valid_, runs);
    add_all(cpu_dirty_, runs);
    subtract_all(cpu_visibility_, runs);
    std::vector<GpuCpuVisibilityRange> visibility;
    visibility.reserve(cpu_visibility_.size() + domains.size());
    std::size_t next = 0;
    const auto push = [&](GpuCpuVisibilityRange range) {
      if (!visibility.empty() && visibility.back().end() == range.address &&
          visibility.back().ordering_domain == range.ordering_domain) {
        visibility.back().size += range.size;
      } else {
        visibility.push_back(range);
      }
    };
    for (const auto& range : cpu_visibility_) {
      while (next < domains.size() && domains[next].address < range.address)
        push(domains[next++]);
      push(range);
    }
    for (; next < domains.size(); ++next) push(domains[next]);
    cpu_visibility_ = std::move(visibility);
  }

  // Union of a sorted set and sorted, disjoint insertions, in one pass.
  static void add_all(std::vector<GpuCoherencyRange>& ranges,
                      const std::vector<GpuCoherencyRange>& inserted) {
    if (inserted.empty()) return;
    std::vector<GpuCoherencyRange> merged;
    merged.reserve(ranges.size() + inserted.size());
    const auto push = [&](GpuCoherencyRange range) {
      if (!merged.empty() && merged.back().end() >= range.address) {
        merged.back().size = static_cast<std::uint32_t>(
            (std::max)(merged.back().end(), range.end()) - merged.back().address);
      } else {
        merged.push_back(range);
      }
    };
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < ranges.size() || j < inserted.size()) {
      if (j >= inserted.size() ||
          (i < ranges.size() && ranges[i].address <= inserted[j].address)) {
        push(ranges[i++]);
      } else {
        push(inserted[j++]);
      }
    }
    ranges = std::move(merged);
  }

  // A sorted set minus sorted, disjoint removals, in one pass; pieces keep
  // their other fields (generation, ordering domain).
  template <typename Range>
  static void subtract_all(std::vector<Range>& ranges,
                           const std::vector<GpuCoherencyRange>& removed) {
    if (ranges.empty() || removed.empty()) return;
    std::vector<Range> result;
    result.reserve(ranges.size() + removed.size());
    std::size_t first = 0;
    for (const auto& range : ranges) {
      std::uint64_t cursor = range.address;
      const std::uint64_t end = range.end();
      while (first < removed.size() && removed[first].end() <= cursor) ++first;
      for (std::size_t k = first; cursor < end; ++k) {
        if (k >= removed.size() || removed[k].address >= end) {
          auto piece = range;
          piece.address = static_cast<std::uint32_t>(cursor);
          piece.size = static_cast<std::uint32_t>(end - cursor);
          result.push_back(piece);
          break;
        }
        if (removed[k].address > cursor) {
          auto piece = range;
          piece.address = static_cast<std::uint32_t>(cursor);
          piece.size = static_cast<std::uint32_t>(removed[k].address - cursor);
          result.push_back(piece);
        }
        cursor = (std::max)(cursor, removed[k].end());
      }
    }
    ranges = std::move(result);
  }

  void touch_pages_unlocked(GpuCoherencyRange range) {
    if (!range.size) return;
    const auto first = range.address & ~(kBasePageSize - 1u);
    const auto end = (std::min)(
        std::uint64_t{size_},
        (range.end() + kBasePageSize - 1u) &
            ~std::uint64_t{kBasePageSize - 1u});
    if (end > first) {
      add(touched_pages_,
          {first, static_cast<std::uint32_t>(end - first)});
    }
  }

  static std::vector<GpuCoherencyRange> uncovered_ranges(
      const std::vector<GpuCoherencyRange>& covered,
      GpuCoherencyRange query) {
    // The gaps between the sorted covered ranges, in one pass.
    std::vector<GpuCoherencyRange> result;
    if (!query.size) return result;
    std::uint64_t cursor = query.address;
    const auto end = query.end();
    const auto first = std::lower_bound(
        covered.begin(), covered.end(), query.address,
        [](const GpuCoherencyRange& range, std::uint32_t address) { return range.end() <= address; });
    for (auto it = first; it != covered.end() && it->address < end; ++it) {
      if (it->address > cursor)
        result.push_back({static_cast<std::uint32_t>(cursor),
                          static_cast<std::uint32_t>(it->address - cursor)});
      cursor = (std::max)(cursor, it->end());
      if (cursor >= end) break;
    }
    if (cursor < end)
      result.push_back({static_cast<std::uint32_t>(cursor), static_cast<std::uint32_t>(end - cursor)});
    return result;
  }

  static bool overlaps(const std::vector<GpuCoherencyRange>& ranges,
                       GpuCoherencyRange query) {
    if (!query.size) return false;
    for (const auto& range : ranges) {
      if (range.end() <= query.address) continue;
      if (range.address >= query.end()) break;
      return true;
    }
    return false;
  }

  static bool overlaps(const std::vector<GpuReadbackRange>& ranges,
                       GpuCoherencyRange query) {
    if (!query.size) return false;
    for (const auto& range : ranges) {
      if (range.end() <= query.address) continue;
      if (range.address >= query.end()) break;
      return true;
    }
    return false;
  }

  static void add(std::vector<GpuCoherencyRange>& ranges,
                  GpuCoherencyRange inserted) {
    if (!inserted.size) return;
    std::vector<GpuCoherencyRange> merged;
    merged.reserve(ranges.size() + 1u);
    auto start = std::uint64_t{inserted.address};
    auto end = inserted.end();
    bool emitted = false;
    for (const auto& range : ranges) {
      if (range.end() < start) {
        merged.push_back(range);
      } else if (end < range.address) {
        if (!emitted) {
          merged.push_back({static_cast<std::uint32_t>(start),
                            static_cast<std::uint32_t>(end - start)});
          emitted = true;
        }
        merged.push_back(range);
      } else {
        start = (std::min)(start, std::uint64_t{range.address});
        end = (std::max)(end, range.end());
      }
    }
    if (!emitted)
      merged.push_back({static_cast<std::uint32_t>(start),
                        static_cast<std::uint32_t>(end - start)});
    ranges = std::move(merged);
  }

  static void subtract(std::vector<GpuCoherencyRange>& ranges,
                       GpuCoherencyRange removed) {
    if (!removed.size || ranges.empty()) return;
    std::vector<GpuCoherencyRange> result;
    result.reserve(ranges.size() + 1u);
    for (const auto& range : ranges) {
      if (range.end() <= removed.address || range.address >= removed.end()) {
        result.push_back(range);
        continue;
      }
      if (range.address < removed.address)
        result.push_back({range.address, removed.address - range.address});
      if (range.end() > removed.end())
        result.push_back({static_cast<std::uint32_t>(removed.end()),
                          static_cast<std::uint32_t>(range.end() - removed.end())});
    }
    ranges = std::move(result);
  }

  static void subtract(std::vector<GpuReadbackRange>& ranges,
                       GpuCoherencyRange removed) {
    if (!removed.size || ranges.empty()) return;
    std::vector<GpuReadbackRange> result;
    result.reserve(ranges.size() + 1u);
    for (const auto& range : ranges) {
      if (range.end() <= removed.address || range.address >= removed.end()) {
        result.push_back(range);
        continue;
      }
      if (range.address < removed.address) {
        result.push_back({range.address, removed.address - range.address,
                          range.generation});
      }
      if (range.end() > removed.end()) {
        result.push_back({static_cast<std::uint32_t>(removed.end()),
                          static_cast<std::uint32_t>(range.end() - removed.end()),
                          range.generation});
      }
    }
    ranges = std::move(result);
  }

  static void subtract(std::vector<GpuCpuVisibilityRange>& ranges,
                       GpuCoherencyRange removed) {
    if (!removed.size || ranges.empty()) return;
    std::vector<GpuCpuVisibilityRange> result;
    result.reserve(ranges.size() + 1u);
    for (const auto& range : ranges) {
      if (range.end() <= removed.address || range.address >= removed.end()) {
        result.push_back(range);
        continue;
      }
      if (range.address < removed.address) {
        result.push_back({range.address, removed.address - range.address,
                          range.ordering_domain});
      }
      if (range.end() > removed.end()) {
        result.push_back({static_cast<std::uint32_t>(removed.end()),
                          static_cast<std::uint32_t>(range.end() - removed.end()),
                          range.ordering_domain});
      }
    }
    ranges = std::move(result);
  }

  static void assign_cpu_visibility_range(
      std::vector<GpuCpuVisibilityRange>& ranges,
      GpuCpuVisibilityRange inserted) {
    if (!inserted.size) return;
    subtract(ranges, {inserted.address, inserted.size});
    ranges.insert(std::lower_bound(ranges.begin(), ranges.end(), inserted.address,
                                   [](const auto& range, std::uint32_t address) {
                                     return range.address < address;
                                   }),
                  inserted);
    std::vector<GpuCpuVisibilityRange> merged;
    merged.reserve(ranges.size());
    for (const auto& range : ranges) {
      if (!merged.empty() && merged.back().end() == range.address &&
          merged.back().ordering_domain == range.ordering_domain) {
        merged.back().size += range.size;
      } else {
        merged.push_back(range);
      }
    }
    ranges = std::move(merged);
  }

  [[nodiscard]] static unsigned domain_strength(
      xenon::cpu::MemoryOrderingDomain domain) noexcept {
    switch (domain) {
      case xenon::cpu::MemoryOrderingDomain::Normal: return 0u;
      case xenon::cpu::MemoryOrderingDomain::WriteCombined: return 1u;
      case xenon::cpu::MemoryOrderingDomain::CacheInhibited: return 2u;
      case xenon::cpu::MemoryOrderingDomain::Device: return 3u;
    }
    return 0u;
  }

  [[nodiscard]] static xenon::cpu::MemoryOrderingDomain strongest_visibility_domain(
      const std::vector<GpuCpuVisibilityRange>& visibility,
      const std::vector<GpuCoherencyRange>& upload_ranges) noexcept {
    // Both lists are sorted and disjoint: one forward pass. Rescanning the
    // visibility list from its start for every upload piece made planning
    // AC6's 692 KB vertex ring take up to a second.
    auto strongest = xenon::cpu::MemoryOrderingDomain::Normal;
    std::size_t first = 0;
    for (const auto& upload : upload_ranges) {
      while (first < visibility.size() && visibility[first].end() <= upload.address) ++first;
      for (auto i = first; i < visibility.size() && visibility[i].address < upload.end(); ++i) {
        if (domain_strength(visibility[i].ordering_domain) > domain_strength(strongest))
          strongest = visibility[i].ordering_domain;
      }
    }
    return strongest;
  }

  static void assign_gpu_range(std::vector<GpuReadbackRange>& ranges,
                               GpuReadbackRange inserted) {
    if (!inserted.size) return;
    subtract(ranges, {inserted.address, inserted.size});
    ranges.push_back(inserted);
    std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) {
      return a.address < b.address;
    });
    std::vector<GpuReadbackRange> merged;
    merged.reserve(ranges.size());
    for (const auto& range : ranges) {
      if (!merged.empty() && merged.back().end() == range.address &&
          merged.back().generation == range.generation) {
        merged.back().size += range.size;
      } else {
        merged.push_back(range);
      }
    }
    ranges = std::move(merged);
  }

  // Remove only bytes still carrying generation. Returns the number of bytes
  // actually removed so callers can detect a newer overlapping GPU write.
  static std::uint32_t subtract_generation(
      std::vector<GpuReadbackRange>& ranges, GpuCoherencyRange removed,
      std::uint64_t generation) {
    if (!removed.size || ranges.empty()) return 0u;
    std::uint64_t removed_bytes = 0u;
    std::vector<GpuReadbackRange> result;
    result.reserve(ranges.size() + 1u);
    for (const auto& range : ranges) {
      if (range.generation != generation || range.end() <= removed.address ||
          range.address >= removed.end()) {
        result.push_back(range);
        continue;
      }
      const auto overlap_start = (std::max)(std::uint64_t{range.address},
                                            std::uint64_t{removed.address});
      const auto overlap_end = (std::min)(range.end(), removed.end());
      removed_bytes += overlap_end - overlap_start;
      if (range.address < overlap_start) {
        result.push_back({range.address,
                          static_cast<std::uint32_t>(overlap_start - range.address),
                          range.generation});
      }
      if (range.end() > overlap_end) {
        result.push_back({static_cast<std::uint32_t>(overlap_end),
                          static_cast<std::uint32_t>(range.end() - overlap_end),
                          range.generation});
      }
    }
    ranges = std::move(result);
    return static_cast<std::uint32_t>(removed_bytes);
  }

  static std::vector<GpuCoherencyRange> intersections(
      const std::vector<GpuCoherencyRange>& ranges, GpuCoherencyRange query) {
    std::vector<GpuCoherencyRange> result;
    if (!query.size) return result;
    const auto first = std::lower_bound(
        ranges.begin(), ranges.end(), query.address,
        [](const auto& range, std::uint32_t address) { return range.end() <= address; });
    for (auto it = first; it != ranges.end(); ++it) {
      const auto& range = *it;
      if (range.address >= query.end()) break;
      const auto start = (std::max)(std::uint64_t{range.address},
                                    std::uint64_t{query.address});
      const auto end = (std::min)(range.end(), query.end());
      if (end > start)
        result.push_back({static_cast<std::uint32_t>(start),
                          static_cast<std::uint32_t>(end - start)});
    }
    return result;
  }

  static std::vector<GpuReadbackRange> intersections(
      const std::vector<GpuReadbackRange>& ranges, GpuCoherencyRange query) {
    std::vector<GpuReadbackRange> result;
    if (!query.size) return result;
    for (const auto& range : ranges) {
      if (range.end() <= query.address) continue;
      if (range.address >= query.end()) break;
      const auto start = (std::max)(std::uint64_t{range.address},
                                    std::uint64_t{query.address});
      const auto end = (std::min)(range.end(), query.end());
      if (end > start) {
        result.push_back({static_cast<std::uint32_t>(start),
                          static_cast<std::uint32_t>(end - start),
                          range.generation});
      }
    }
    return result;
  }

  static std::vector<GpuReadbackRange> intersections_generation(
      const std::vector<GpuReadbackRange>& ranges, GpuCoherencyRange query,
      std::uint64_t generation) {
    auto result = intersections(ranges, query);
    result.erase(std::remove_if(result.begin(), result.end(),
                                [generation](const auto& range) {
                                  return range.generation != generation;
                                }),
                 result.end());
    return result;
  }

  static void split_ranges(std::vector<GpuCoherencyRange>& ranges,
                           std::uint32_t maximum) {
    if (!maximum) return;
    std::vector<GpuCoherencyRange> split;
    for (const auto& range : ranges) {
      for (std::uint64_t offset = 0; offset < range.size; offset += maximum) {
        split.push_back({static_cast<std::uint32_t>(range.address + offset),
                         static_cast<std::uint32_t>((std::min)(
                             std::uint64_t{maximum},
                             std::uint64_t{range.size} - offset))});
      }
    }
    ranges = std::move(split);
  }

  static void split_ranges(std::vector<GpuReadbackRange>& ranges,
                           std::uint32_t maximum) {
    if (!maximum) return;
    std::vector<GpuReadbackRange> split;
    for (const auto& range : ranges) {
      for (std::uint64_t offset = 0; offset < range.size; offset += maximum) {
        split.push_back({static_cast<std::uint32_t>(range.address + offset),
                         static_cast<std::uint32_t>((std::min)(
                             std::uint64_t{maximum},
                             std::uint64_t{range.size} - offset)),
                         range.generation});
      }
    }
    ranges = std::move(split);
  }

  mutable std::mutex mutex_{};
  std::uint32_t size_{};
  GpuMemoryTopology topology_{GpuMemoryTopology::DiscreteMirror};
  std::uint64_t synchronized_epoch_{};
  std::uint64_t gpu_generation_{};
  std::vector<GpuCoherencyRange> cpu_dirty_{};
  std::vector<GpuCpuVisibilityRange> cpu_visibility_{};
  std::vector<GpuReadbackRange> gpu_dirty_{};
  std::vector<GpuCoherencyRange> device_valid_{};
  std::vector<GpuCoherencyRange> requested_{};
  std::vector<GpuCoherencyRange> touched_pages_{};
};

}  // namespace xenon::memory

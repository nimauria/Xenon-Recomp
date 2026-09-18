#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
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

struct GpuUploadPlan {
  std::uint64_t through_epoch{};
  bool exact_history{};
  GpuSynchronizationAction action{GpuSynchronizationAction::Copy};
  std::vector<GpuCoherencyRange> ranges{};
};

struct GpuReadbackPlan {
  std::uint64_t gpu_generation{};
  GpuSynchronizationAction action{GpuSynchronizationAction::Copy};
  std::vector<GpuCoherencyRange> ranges{};
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
    gpu_dirty_.clear();
    device_valid_.clear();
    if (cpu_initially_dirty && size_) cpu_dirty_.push_back({0, size_});
  }

  void clear() noexcept {
    std::lock_guard lock(mutex_);
    size_ = 0;
    topology_ = GpuMemoryTopology::DiscreteMirror;
    synchronized_epoch_ = 0;
    gpu_generation_ = 0;
    cpu_dirty_.clear();
    gpu_dirty_.clear();
    device_valid_.clear();
  }

  [[nodiscard]] GpuUploadPlan plan_upload(
      const GuestMemoryCoherency& memory, std::uint32_t address = 0,
      std::uint32_t size = (std::numeric_limits<std::uint32_t>::max)(),
      std::uint32_t max_range_bytes = 0) {
    GpuUploadPlan plan{};
    plan.through_epoch = memory.current_epoch();
    plan.action = synchronization_action();

    std::lock_guard lock(mutex_);
    plan.exact_history = ingest_cpu_writes_unlocked(
        memory, plan.through_epoch, 0u, max_range_bytes);
    plan.ranges = intersections(cpu_dirty_, clamp(address, size));
    if (max_range_bytes) split_ranges(plan.ranges, max_range_bytes);
    return plan;
  }

  void commit_cpu_upload(std::uint32_t address, std::uint32_t size) {
    std::lock_guard lock(mutex_);
    const auto range = clamp(address, size);
    subtract(cpu_dirty_, range);
    add(device_valid_, range);
  }

  void rollback_cpu_upload(std::uint32_t address, std::uint32_t size) {
    mark_cpu_write(address, size);
  }

  void mark_cpu_write(std::uint32_t address, std::uint32_t size) {
    std::lock_guard lock(mutex_);
    mark_cpu_write_unlocked(address, size);
  }

  // Returns the generation assigned to this GPU-authored ownership change.
  std::uint64_t mark_gpu_write(std::uint32_t address,
                               std::uint32_t size) {
    std::lock_guard lock(mutex_);
    const auto range = clamp(address, size);
    if (!range.size) return gpu_generation_;
    ++gpu_generation_;
    if (!gpu_generation_) ++gpu_generation_;
    subtract(cpu_dirty_, range);
    add(gpu_dirty_, range);
    add(device_valid_, range);
    return gpu_generation_;
  }

  [[nodiscard]] GpuReadbackPlan plan_readback(
      std::uint32_t address, std::uint32_t size,
      std::uint32_t max_range_bytes = 0) const {
    std::lock_guard lock(mutex_);
    GpuReadbackPlan plan{};
    plan.gpu_generation = gpu_generation_;
    plan.action = synchronization_action_unlocked();
    plan.ranges = intersections(gpu_dirty_, clamp(address, size));
    if (max_range_bytes) split_ranges(plan.ranges, max_range_bytes);
    return plan;
  }

  // Completes a GPU->CPU visibility operation. publication_epoch is the exact
  // GuestMemoryCoherency epoch produced when a discrete mirror copied the
  // downloaded bytes into Xenon physical RAM. That epoch is source-aware: it
  // is acknowledged as this consumer's own write while all unrelated CPU/DMA
  // writes in the same interval are retained as CPU-dirty.
  //
  // If exact journal history has wrapped, the function deliberately retains
  // conservative CPU dirt rather than risking suppression of a real writer.
  // If another GPU write was published after the plan was made, GPU dirt is
  // also retained conservatively instead of clearing a newer generation.
  [[nodiscard]] bool commit_gpu_download(
      const GuestMemoryCoherency& memory, const GpuReadbackPlan& plan,
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

    const bool generation_stable = gpu_generation_ == plan.gpu_generation;
    if (generation_stable) subtract(gpu_dirty_, range);

    // The device copy itself is current for this range, but any CPU write we
    // observed while acknowledging the readback makes the overlapping bytes
    // device-stale again. On journal overflow we conservatively leave CPU dirt
    // in place, so this same rule remains safe.
    add(device_valid_, range);
    for (const auto& cpu_range : intersections(cpu_dirty_, range)) {
      subtract(device_valid_, cpu_range);
    }

    return exact_acknowledgement && generation_stable;
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
    return plan_readback(address, size).ranges;
  }

 private:
  [[nodiscard]] GpuSynchronizationAction synchronization_action() const {
    std::lock_guard lock(mutex_);
    return synchronization_action_unlocked();
  }

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
      for (const auto& write : exact_writes) {
        if (ignored_epoch && write.epoch == ignored_epoch) continue;
        mark_cpu_write_unlocked(write.address, write.size);
      }
    } else {
      std::vector<DirtyPhysicalRange> dirty_pages;
      memory.collect_dirty_ranges(synchronized_epoch_, through_epoch,
                                  max_range_bytes, dirty_pages);
      for (const auto& write : dirty_pages) {
        mark_cpu_write_unlocked(write.address, write.size);
      }
    }
    synchronized_epoch_ = through_epoch;
    return exact;
  }

  [[nodiscard]] GpuCoherencyRange clamp(std::uint32_t address,
                                        std::uint32_t size) const noexcept {
    if (!size || address >= size_) return {};
    const auto end = (std::min)(std::uint64_t{address} + size,
                                std::uint64_t{size_});
    return {address, static_cast<std::uint32_t>(end - address)};
  }

  void mark_cpu_write_unlocked(std::uint32_t address, std::uint32_t size) {
    const auto range = clamp(address, size);
    if (!range.size) return;
    subtract(gpu_dirty_, range);
    subtract(device_valid_, range);
    add(cpu_dirty_, range);
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

  static std::vector<GpuCoherencyRange> intersections(
      const std::vector<GpuCoherencyRange>& ranges, GpuCoherencyRange query) {
    std::vector<GpuCoherencyRange> result;
    if (!query.size) return result;
    for (const auto& range : ranges) {
      if (range.end() <= query.address) continue;
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

  mutable std::mutex mutex_{};
  std::uint32_t size_{};
  GpuMemoryTopology topology_{GpuMemoryTopology::DiscreteMirror};
  std::uint64_t synchronized_epoch_{};
  std::uint64_t gpu_generation_{};
  std::vector<GpuCoherencyRange> cpu_dirty_{};
  std::vector<GpuCoherencyRange> gpu_dirty_{};
  std::vector<GpuCoherencyRange> device_valid_{};
};

}  // namespace xenon::memory

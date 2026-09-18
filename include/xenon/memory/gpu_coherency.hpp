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

struct GpuUploadPlan {
  std::uint64_t through_epoch{};
  bool exact_history{};
  std::vector<GpuCoherencyRange> ranges{};
};

// Backend-neutral ownership and synchronization planning for a device-side
// Xbox physical-memory mirror. Native APIs execute copies and barriers; Xenon
// Memory owns which bytes are newer and which ranges are device-valid.
class GuestMemoryGpuCoherency {
 public:
  void reset(std::uint32_t size, bool cpu_initially_dirty = true) {
    std::lock_guard lock(mutex_);
    size_ = size;
    synchronized_epoch_ = 0;
    cpu_dirty_.clear();
    gpu_dirty_.clear();
    device_valid_.clear();
    if (cpu_initially_dirty && size_) cpu_dirty_.push_back({0, size_});
  }

  void clear() noexcept {
    std::lock_guard lock(mutex_);
    size_ = 0;
    synchronized_epoch_ = 0;
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
    std::vector<DirtyPhysicalRange> writes;

    std::lock_guard lock(mutex_);
    plan.exact_history = memory.collect_exact_dirty_ranges(
        synchronized_epoch_, plan.through_epoch, writes);
    if (!plan.exact_history) {
      memory.collect_dirty_ranges(synchronized_epoch_, plan.through_epoch,
                                  max_range_bytes, writes);
    }
    for (const auto& write : writes)
      mark_cpu_write_unlocked(write.address, write.size);
    synchronized_epoch_ = plan.through_epoch;

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

  void mark_gpu_write(std::uint32_t address, std::uint32_t size) {
    std::lock_guard lock(mutex_);
    const auto range = clamp(address, size);
    if (!range.size) return;
    subtract(cpu_dirty_, range);
    add(gpu_dirty_, range);
    add(device_valid_, range);
  }

  [[nodiscard]] std::vector<GpuCoherencyRange> plan_readback(
      std::uint32_t address, std::uint32_t size,
      std::uint32_t max_range_bytes = 0) const {
    std::lock_guard lock(mutex_);
    auto result = intersections(gpu_dirty_, clamp(address, size));
    if (max_range_bytes) split_ranges(result, max_range_bytes);
    return result;
  }

  void commit_gpu_download(std::uint32_t address, std::uint32_t size) {
    std::lock_guard lock(mutex_);
    const auto range = clamp(address, size);
    subtract(gpu_dirty_, range);
    subtract(cpu_dirty_, range);
    add(device_valid_, range);
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

  // Compatibility API retained while all consumers move to explicit plans.
  void mark_cpu_uploaded(std::uint32_t address, std::uint32_t size) {
    commit_cpu_upload(address, size);
  }
  void mark_gpu_downloaded(std::uint32_t address, std::uint32_t size) {
    commit_gpu_download(address, size);
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
    return plan_readback(address, size);
  }

 private:
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
  std::uint64_t synchronized_epoch_{};
  std::vector<GpuCoherencyRange> cpu_dirty_{};
  std::vector<GpuCoherencyRange> gpu_dirty_{};
  std::vector<GpuCoherencyRange> device_valid_{};
};

}  // namespace xenon::memory

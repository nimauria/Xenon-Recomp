#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

#include "xenon/memory/types.hpp"

namespace xenon::memory {

struct DirtyPhysicalRange {
  std::uint32_t address{};
  std::uint32_t size{};
};

struct DirtyPhysicalWrite {
  std::uint64_t epoch{};
  std::uint32_t address{};
  std::uint32_t size{};
};

// Backend-neutral CPU/GPU/DMA dirty state. Writers publish a monotonically
// increasing epoch to every touched physical 4 KiB page. Consumers keep their
// own last-seen epoch, so Vulkan, D3D12 and future UMA/mobile paths do not need
// synchronous callbacks from scalar CPU stores.
class GuestMemoryCoherency {
 public:
  static constexpr std::uint32_t kPageCount =
      kPhysicalMemorySize / kBasePageSize;
  static constexpr std::uint32_t kWriteJournalCapacity = 65536u;

  GuestMemoryCoherency()
      : page_epochs_(kPageCount),
        write_journal_epochs_(kWriteJournalCapacity),
        write_journal_ranges_(kWriteJournalCapacity) {
    for (auto& epoch : page_epochs_) {
      epoch.store(0, std::memory_order_relaxed);
    }
  }

  GuestMemoryCoherency(const GuestMemoryCoherency&) = delete;
  GuestMemoryCoherency& operator=(const GuestMemoryCoherency&) = delete;

  // Returns an epoch boundary for which every writer at or below the returned
  // epoch has finished publishing its page epochs. Writers that begin after
  // this snapshot receive a newer epoch and are observed by the next consume.
  [[nodiscard]] std::uint64_t current_epoch() const noexcept {
    for (;;) {
      if (active_writers_.load(std::memory_order_acquire) != 0u) continue;
      const auto epoch = write_epoch_.load(std::memory_order_acquire);
      if (active_writers_.load(std::memory_order_acquire) == 0u) return epoch;
    }
  }

  [[nodiscard]] std::atomic<std::uint64_t>* page_epochs_data() noexcept {
    return page_epochs_.data();
  }
  [[nodiscard]] const std::atomic<std::uint64_t>* page_epochs_data() const noexcept {
    return page_epochs_.data();
  }
  [[nodiscard]] std::atomic<std::uint64_t>* write_epoch_data() noexcept {
    return &write_epoch_;
  }
  [[nodiscard]] std::atomic<std::uint32_t>* active_writers_data() noexcept {
    return &active_writers_;
  }
  [[nodiscard]] std::atomic<std::uint64_t>* write_journal_epochs_data() noexcept {
    return write_journal_epochs_.data();
  }
  [[nodiscard]] std::atomic<std::uint64_t>* write_journal_ranges_data() noexcept {
    return write_journal_ranges_.data();
  }

  std::uint64_t mark_write(std::uint32_t physical_address,
                           std::uint32_t width) noexcept {
    if (!width || physical_address >= kPhysicalMemorySize) return current_epoch();
    const auto end = std::min<std::uint64_t>(
        std::uint64_t{physical_address} + width, kPhysicalMemorySize);
    if (end <= physical_address) return current_epoch();

    active_writers_.fetch_add(1u, std::memory_order_acq_rel);
    auto epoch = write_epoch_.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    if (epoch == 0u) {
      write_epoch_.store(1u, std::memory_order_release);
      epoch = 1u;
    }
    const auto first_page = physical_address / kBasePageSize;
    const auto last_page = static_cast<std::uint32_t>((end - 1u) / kBasePageSize);
    for (std::uint32_t page = first_page; page <= last_page; ++page) {
      page_epochs_[page].store(epoch, std::memory_order_release);
    }
    publish_journal(epoch, physical_address,
                    static_cast<std::uint32_t>(end - physical_address));
    active_writers_.fetch_sub(1u, std::memory_order_release);
    return epoch;
  }

  std::uint64_t mark_all_dirty() noexcept {
    active_writers_.fetch_add(1u, std::memory_order_acq_rel);
    auto epoch = write_epoch_.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    if (epoch == 0u) {
      write_epoch_.store(1u, std::memory_order_release);
      epoch = 1u;
    }
    for (auto& page_epoch : page_epochs_) {
      page_epoch.store(epoch, std::memory_order_release);
    }
    publish_journal(epoch, 0u, kPhysicalMemorySize);
    active_writers_.fetch_sub(1u, std::memory_order_release);
    return epoch;
  }

  [[nodiscard]] bool range_changed_since(std::uint32_t physical_address,
                                         std::uint32_t width,
                                         std::uint64_t since_epoch,
                                         std::uint64_t through_epoch =
                                             UINT64_MAX) const noexcept {
    (void)through_epoch;
    if (!width || physical_address >= kPhysicalMemorySize) return false;
    const auto end = std::min<std::uint64_t>(
        std::uint64_t{physical_address} + width, kPhysicalMemorySize);
    if (end <= physical_address) return false;
    const auto first_page = physical_address / kBasePageSize;
    const auto last_page = static_cast<std::uint32_t>((end - 1u) / kBasePageSize);
    for (std::uint32_t page = first_page; page <= last_page; ++page) {
      const auto epoch = page_epochs_[page].load(std::memory_order_acquire);
      // A page newer than through_epoch is still dirty for this consumer.
      // Consuming the newest bytes now is safe; because the consumer only
      // advances to through_epoch, that newer page is intentionally seen again
      // on the next pass rather than allowing an intervening write to hide an
      // older dirty state.
      if (epoch > since_epoch) return true;
    }
    return false;
  }

  // Coalesces dirty pages between (since_epoch, through_epoch] into native
  // physical ranges. max_range_bytes limits staging-buffer chunk size; zero
  // means no explicit limit.
  void collect_dirty_ranges(std::uint64_t since_epoch,
                            std::uint64_t through_epoch,
                            std::uint32_t max_range_bytes,
                            std::vector<DirtyPhysicalRange>& out) const {
    out.clear();
    if (through_epoch <= since_epoch) return;
    const std::uint32_t max_pages = max_range_bytes
                                        ? (std::max)(1u, max_range_bytes /
                                                             kBasePageSize)
                                        : kPageCount;
    for (std::uint32_t page = 0; page < kPageCount;) {
      const auto epoch = page_epochs_[page].load(std::memory_order_acquire);
      if (!(epoch > since_epoch)) {
        ++page;
        continue;
      }
      const auto first = page;
      std::uint32_t count = 0;
      while (page < kPageCount && count < max_pages) {
        const auto candidate =
            page_epochs_[page].load(std::memory_order_acquire);
        if (!(candidate > since_epoch)) break;
        ++page;
        ++count;
      }
      out.push_back({first * kBasePageSize, count * kBasePageSize});
    }
  }

  // Returns byte-precise writes, including the publication epoch of each
  // write, while the requested interval is still in the bounded lock-free
  // journal. Epoch identity lets a coherency consumer acknowledge a write it
  // performed itself without accidentally suppressing unrelated CPU writes.
  [[nodiscard]] bool collect_exact_dirty_writes(
      std::uint64_t since_epoch, std::uint64_t through_epoch,
      std::vector<DirtyPhysicalWrite>& out) const {
    out.clear();
    if (through_epoch <= since_epoch) return true;
    if (through_epoch - since_epoch > kWriteJournalCapacity) return false;
    out.reserve(static_cast<std::size_t>(through_epoch - since_epoch));
    for (auto epoch = since_epoch + 1u; epoch <= through_epoch; ++epoch) {
      const auto index = static_cast<std::uint32_t>(epoch) &
                         (kWriteJournalCapacity - 1u);
      const auto before =
          write_journal_epochs_[index].load(std::memory_order_acquire);
      if (before != epoch) {
        out.clear();
        return false;
      }
      const auto packed =
          write_journal_ranges_[index].load(std::memory_order_relaxed);
      const auto after =
          write_journal_epochs_[index].load(std::memory_order_acquire);
      if (after != epoch) {
        out.clear();
        return false;
      }
      const auto address = static_cast<std::uint32_t>(packed >> 32u);
      const auto size = static_cast<std::uint32_t>(packed);
      if (size) out.push_back({epoch, address, size});
    }
    return true;
  }

  // Compatibility range-only view used by non-source-aware consumers.
  [[nodiscard]] bool collect_exact_dirty_ranges(
      std::uint64_t since_epoch, std::uint64_t through_epoch,
      std::vector<DirtyPhysicalRange>& out) const {
    std::vector<DirtyPhysicalWrite> writes;
    if (!collect_exact_dirty_writes(since_epoch, through_epoch, writes)) {
      out.clear();
      return false;
    }
    out.clear();
    out.reserve(writes.size());
    for (const auto& write : writes) {
      out.push_back({write.address, write.size});
    }
    return true;
  }

 private:
  void publish_journal(std::uint64_t epoch, std::uint32_t address,
                       std::uint32_t size) noexcept {
    const auto index = static_cast<std::uint32_t>(epoch) &
                       (kWriteJournalCapacity - 1u);
    write_journal_ranges_[index].store(
        (std::uint64_t{address} << 32u) | size, std::memory_order_relaxed);
    write_journal_epochs_[index].store(epoch, std::memory_order_release);
  }

  std::vector<std::atomic<std::uint64_t>> page_epochs_{};
  static_assert((kWriteJournalCapacity & (kWriteJournalCapacity - 1u)) == 0u);
  std::vector<std::atomic<std::uint64_t>> write_journal_epochs_{};
  std::vector<std::atomic<std::uint64_t>> write_journal_ranges_{};
  std::atomic<std::uint64_t> write_epoch_{0};
  std::atomic<std::uint32_t> active_writers_{0};
};

}  // namespace xenon::memory

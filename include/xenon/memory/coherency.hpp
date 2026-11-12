#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <mutex>
#include <vector>

#include "xenon/cpu/memory_ordering.hpp"
#include "xenon/memory/types.hpp"

namespace xenon::memory {

// Makes every store any thread of this process has issued globally visible
// before it returns (Windows FlushProcessWriteBuffers, Linux membarrier).
// Lets the side that runs rarely pay for ordering that hot stores skip.
void flush_process_write_buffers() noexcept;

struct DirtyPhysicalRange {
  std::uint32_t address{};
  std::uint32_t size{};
};

struct DirtyPhysicalWrite {
  std::uint64_t epoch{};
  std::uint32_t address{};
  std::uint32_t size{};
  xenon::cpu::MemoryOrderingDomain ordering_domain{
      xenon::cpu::MemoryOrderingDomain::Normal};
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
        write_journal_ranges_(kWriteJournalCapacity),
        write_journal_domains_(kWriteJournalCapacity) {
    for (auto& epoch : page_epochs_) {
      epoch.store(0, std::memory_order_relaxed);
    }
  }

  GuestMemoryCoherency(const GuestMemoryCoherency&) = delete;
  GuestMemoryCoherency& operator=(const GuestMemoryCoherency&) = delete;

  // Returns an epoch boundary for which every writer at or below the returned
  // epoch has finished publishing its page epochs. Writers that begin after
  // this snapshot receive a newer epoch and are observed by the next consume.
  // Pages dirtied by native-access stores are collected into page epochs
  // first, so they are part of the boundary.
  [[nodiscard]] std::uint64_t current_epoch() const noexcept {
    collect_cpu_dirty_pages();
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
  [[nodiscard]] std::atomic<std::uint8_t>* write_journal_domains_data() noexcept {
    return write_journal_domains_.data();
  }
  [[nodiscard]] std::atomic<std::uint64_t>* cpu_dirty_page_bits_data() noexcept {
    return cpu_dirty_page_bits_.data();
  }
  [[nodiscard]] std::atomic<std::uint64_t>* cpu_dirty_summary_bits_data() noexcept {
    return cpu_dirty_summary_bits_.data();
  }

  // Moves the pages native-access stores marked dirty into page epochs, one
  // new epoch per run of adjacent pages. Those stores test a page's bit
  // before setting it, so one that found it set may still be in its core's
  // store buffer when the bit is cleared here; flush_process_write_buffers()
  // drains every thread's pending stores before the epochs are published, so
  // a consumer that sees an epoch also sees the data.
  //
  // Each run is journaled as a write of its whole pages: the stores' bytes
  // and cache types are not recorded, so the run covers every byte of its
  // pages in the strongest ordering domain (cache-inhibited), exactly what
  // the page-level path reports. Consumers then keep their exact history and
  // reconcile only these pages. Without the journal entry every consumer took
  // the page-level path, scanning all 131072 page epochs: with generated code
  // storing between nearly every two draws, AC6 spent ~50 us of each draw's
  // vertex and texture checks there and ran its 3D scenes at 1-3 fps.
  // Returns the newest epoch, or 0 when nothing was dirty.
  std::uint64_t collect_cpu_dirty_pages() const noexcept {
    bool any = false;
    for (const auto& summary : cpu_dirty_summary_bits_) {
      if (summary.load(std::memory_order_acquire)) {
        any = true;
        break;
      }
    }
    if (!any) return 0u;

    std::lock_guard lock(collect_mutex_);
    collected_pages_.clear();
    for (std::size_t s = 0; s < cpu_dirty_summary_bits_.size(); ++s) {
      auto words = cpu_dirty_summary_bits_[s].exchange(0u, std::memory_order_acq_rel);
      while (words) {
        const auto word_index = s * 64u + static_cast<std::size_t>(std::countr_zero(words));
        words &= words - 1u;
        auto pages = cpu_dirty_page_bits_[word_index].exchange(0u, std::memory_order_acq_rel);
        while (pages) {
          collected_pages_.push_back(static_cast<std::uint32_t>(
              word_index * 64u + static_cast<std::size_t>(std::countr_zero(pages))));
          pages &= pages - 1u;
        }
      }
    }
    if (collected_pages_.empty()) return 0u;

    flush_process_write_buffers();
    active_writers_.fetch_add(1u, std::memory_order_acq_rel);
    std::uint64_t epoch = 0u;
    // collected_pages_ is ascending: the bits are walked in page order.
    for (std::size_t first = 0; first < collected_pages_.size();) {
      std::size_t end = first + 1u;
      while (end < collected_pages_.size() &&
             collected_pages_[end] == collected_pages_[end - 1u] + 1u)
        ++end;
      epoch = write_epoch_.fetch_add(1u, std::memory_order_acq_rel) + 1u;
      if (epoch == 0u) {
        write_epoch_.store(1u, std::memory_order_release);
        epoch = 1u;
      }
      for (auto i = first; i < end; ++i)
        page_epochs_[collected_pages_[i]].store(epoch, std::memory_order_release);
      publish_journal(epoch, collected_pages_[first] * kBasePageSize,
                      static_cast<std::uint32_t>(end - first) * kBasePageSize,
                      xenon::cpu::MemoryOrderingDomain::CacheInhibited);
      first = end;
    }
    active_writers_.fetch_sub(1u, std::memory_order_release);
    return epoch;
  }

  std::uint64_t mark_write(
      std::uint32_t physical_address, std::uint32_t width,
      xenon::cpu::MemoryOrderingDomain ordering_domain =
          xenon::cpu::MemoryOrderingDomain::Normal) noexcept {
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
                    static_cast<std::uint32_t>(end - physical_address),
                    ordering_domain);
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
    publish_journal(epoch, 0u, kPhysicalMemorySize,
                    xenon::cpu::MemoryOrderingDomain::Normal);
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

  // The newest epoch any page of the range was written at (page granularity:
  // native-access stores not yet collected show up later, at a newer epoch).
  [[nodiscard]] std::uint64_t newest_page_epoch(std::uint32_t physical_address,
                                                std::uint32_t width) const noexcept {
    if (!width || physical_address >= kPhysicalMemorySize) return 0u;
    const auto end = std::min<std::uint64_t>(std::uint64_t{physical_address} + width,
                                             kPhysicalMemorySize);
    std::uint64_t newest = 0u;
    for (auto page = physical_address / kBasePageSize;
         page <= static_cast<std::uint32_t>((end - 1u) / kBasePageSize); ++page)
      newest = (std::max)(newest, page_epochs_[page].load(std::memory_order_acquire));
    return newest;
  }
  // The epoch page `page` (of kBasePageSize bytes) was last written at.
  [[nodiscard]] std::uint64_t page_epoch(std::uint32_t page) const noexcept {
    return page < kPageCount ? page_epochs_[page].load(std::memory_order_acquire) : 0u;
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
      const auto domain = static_cast<xenon::cpu::MemoryOrderingDomain>(
          write_journal_domains_[index].load(std::memory_order_relaxed));
      if (size) out.push_back({epoch, address, size, domain});
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
  void publish_journal(
      std::uint64_t epoch, std::uint32_t address, std::uint32_t size,
      xenon::cpu::MemoryOrderingDomain ordering_domain) const noexcept {
    const auto index = static_cast<std::uint32_t>(epoch) &
                       (kWriteJournalCapacity - 1u);
    write_journal_ranges_[index].store(
        (std::uint64_t{address} << 32u) | size, std::memory_order_relaxed);
    write_journal_domains_[index].store(
        static_cast<std::uint8_t>(ordering_domain), std::memory_order_relaxed);
    write_journal_epochs_[index].store(epoch, std::memory_order_release);
  }

  mutable std::vector<std::atomic<std::uint64_t>> page_epochs_{};
  static_assert((kWriteJournalCapacity & (kWriteJournalCapacity - 1u)) == 0u);
  // Mutable like page_epochs_: collect_cpu_dirty_pages() journals the pages
  // it collects from const consumers.
  mutable std::vector<std::atomic<std::uint64_t>> write_journal_epochs_{};
  mutable std::vector<std::atomic<std::uint64_t>> write_journal_ranges_{};
  mutable std::vector<std::atomic<std::uint8_t>> write_journal_domains_{};
  // Collection runs from const consumers (current_epoch), so the epoch and
  // writer counters it advances are mutable like the dirty bits it clears.
  mutable std::atomic<std::uint64_t> write_epoch_{0};
  mutable std::atomic<std::uint32_t> active_writers_{0};
  static constexpr std::size_t kDirtyWordCount = kPageCount / 64u;
  static_assert(kPageCount % (64u * 64u) == 0u);
  mutable std::vector<std::atomic<std::uint64_t>> cpu_dirty_page_bits_ =
      std::vector<std::atomic<std::uint64_t>>(kDirtyWordCount);
  mutable std::vector<std::atomic<std::uint64_t>> cpu_dirty_summary_bits_ =
      std::vector<std::atomic<std::uint64_t>>(kDirtyWordCount / 64u);
  mutable std::mutex collect_mutex_;
  mutable std::vector<std::uint32_t> collected_pages_;
};

}  // namespace xenon::memory

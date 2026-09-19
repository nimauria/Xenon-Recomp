#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "xenon/cpu/executable_code_cache.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/memory/gpu_coherency.hpp"

using namespace xenon::cpu;
using namespace xenon::memory;

namespace {

std::uint32_t xorshift32(std::uint32_t& state) {
  state ^= state << 13u;
  state ^= state >> 17u;
  state ^= state << 5u;
  return state;
}

bool covered_by(const std::vector<DirtyPhysicalRange>& ranges,
                std::uint32_t address, std::uint32_t size) {
  const auto end = std::uint64_t{address} + size;
  for (const auto& range : ranges) {
    const auto range_end = std::uint64_t{range.address} + range.size;
    if (range.address <= address && range_end >= end) return true;
  }
  return false;
}

ExecutionResult fuzz_native(CpuState&, MemoryPort&, RuntimeServices&) {
  return {FlowReason::Fallthrough, 0x00800004u, 0u};
}

std::uint32_t fuzz_seed_from_environment() {
  const char* value = std::getenv("XENON_MEMORY_FUZZ_SEED");
  if (!value || !*value) return 0x6D656D32u;
  char* end = nullptr;
  const auto parsed = std::strtoul(value, &end, 0);
  return end && *end == '\0' ? static_cast<std::uint32_t>(parsed)
                              : 0x6D656D32u;
}

std::uint32_t fuzz_scale_percent_from_environment() {
  const char* value = std::getenv("XENON_MEMORY_FUZZ_SCALE");
  if (!value || !*value) return 100u;
  char* end = nullptr;
  const auto parsed = std::strtoul(value, &end, 10);
  if (!end || *end != '\0' || parsed == 0u) return 100u;
  return static_cast<std::uint32_t>((std::min)(parsed, 1000ul));
}

std::uint32_t scaled_iterations(std::uint32_t count, std::uint32_t scale,
                                std::uint32_t minimum = 1u) {
  return (std::max)(minimum, static_cast<std::uint32_t>(
                                 (std::uint64_t{count} * scale) / 100u));
}

}  // namespace

int main() {
  const auto fuzz_seed = fuzz_seed_from_environment();
  const auto fuzz_scale = fuzz_scale_percent_from_environment();
  // Architectural fixed aliases must remain coherent under randomized writes.
  // A/C/7F begin at physical 0; E begins one 4 KiB page into physical RAM.
  {
    AddressSpace memory(GuestTranslationMode::Compact);
    assert(memory.initialize());
    std::uint32_t rng = 0xA11A51A5u ^ fuzz_seed;
    for (std::uint32_t i = 0; i < scaled_iterations(5000u, fuzz_scale); ++i) {
      const auto physical = (xorshift32(rng) & 0x00FFFFFCu);
      const auto value = xorshift32(rng);
      switch (i & 3u) {
        case 0: memory.write32_be(kPhysical64KBase + physical, value); break;
        case 1: memory.write32_be(kPhysical16MBase + physical, value); break;
        case 2: memory.write32_be(kGpuWritebackBase + physical, value); break;
        default:
          if (physical < kPhysicalMemorySize - kPhysical4KViewOffset) {
            memory.write32_be(kPhysical4KBase + physical, value);
            assert(memory.read32_be(kPhysical64KBase + physical +
                                    kPhysical4KViewOffset) == value);
            continue;
          }
          break;
      }
      assert(memory.read32_be(kPhysical64KBase + physical) == value);
      assert(memory.read32_be(kPhysical16MBase + physical) == value);
      assert(memory.read32_be(kGpuWritebackBase + physical) == value);
    }
    assert(memory.validate_invariants());
  }

  // Model-based dynamic mapping/ownership/reverse-map assault. A separately
  // owned physical pool remains live while virtual aliases churn around it.
  {
    AddressSpace memory(GuestTranslationMode::Compact);
    assert(memory.initialize());
    constexpr std::uint32_t kPhysicalPages = 64u;
    constexpr std::uint32_t kVirtualSlots = 48u;
    constexpr GuestAddress kVirtualBase = 0x03000000u;
    std::uint32_t pool{};
    assert(memory.allocate_physical(kPhysicalPages * kBasePageSize,
                                    kBasePageSize, false, pool));

    std::array<int, kVirtualSlots> model{};
    model.fill(-1);
    std::array<std::uint32_t, kPhysicalPages> expected_values{};
    std::uint32_t rng = 0xC001D00Du ^ fuzz_seed;

    for (std::uint32_t iteration = 0;
         iteration < scaled_iterations(20000u, fuzz_scale); ++iteration) {
      const auto slot = xorshift32(rng) % kVirtualSlots;
      const auto va = kVirtualBase + slot * kBasePageSize;
      const auto op = xorshift32(rng) % 7u;

      if (op <= 1u && model[slot] < 0) {
        const auto page = static_cast<int>(xorshift32(rng) % kPhysicalPages);
        assert(memory.map_virtual_to_physical(
            va, pool + static_cast<std::uint32_t>(page) * kBasePageSize,
            kBasePageSize, kReadWrite));
        model[slot] = page;
      } else if (op == 2u && model[slot] >= 0) {
        assert(memory.release(va));
        model[slot] = -1;
      } else if ((op == 3u || op == 4u) && model[slot] >= 0) {
        const auto value = xorshift32(rng);
        memory.write32_be(va, value);
        expected_values[static_cast<std::size_t>(model[slot])] = value;
        const auto physical = pool +
            static_cast<std::uint32_t>(model[slot]) * kBasePageSize;
        assert(memory.read32_be(kPhysical64KBase + physical) == value);
      } else if (op == 5u && model[slot] >= 0) {
        const auto page = static_cast<std::uint32_t>(model[slot]);
        const auto aliases = memory.dynamic_guest_aliases_for_physical(
            pool + page * kBasePageSize);
        assert(std::find(aliases.begin(), aliases.end(), va) != aliases.end());
        assert(memory.read32_be(va) == expected_values[page]);
      } else if (op == 6u && model[slot] >= 0) {
        Protect old{};
        assert(memory.protect(va, kBasePageSize, Protect::Read, &old));
        const auto query = memory.query(va);
        assert(query && query->current_protect == Protect::Read);
        assert(memory.protect(va, kBasePageSize, kReadWrite));
      }

      if ((iteration % 97u) == 0u) {
        std::string error;
        assert(memory.validate_invariants(&error));
        for (std::uint32_t check_slot = 0; check_slot < kVirtualSlots;
             ++check_slot) {
          if (model[check_slot] < 0) continue;
          const auto check_va = kVirtualBase + check_slot * kBasePageSize;
          const auto expected_physical =
              pool + static_cast<std::uint32_t>(model[check_slot]) * kBasePageSize;
          assert(memory.get_physical_address(check_va) == expected_physical);
        }
      }
    }

    // Physical ownership cannot be released while any model alias remains.
    bool any_live = false;
    for (const auto page : model) any_live |= page >= 0;
    if (any_live) {
      assert(!memory.free_physical(pool, kPhysicalPages * kBasePageSize));
    }
    for (std::uint32_t slot = 0; slot < kVirtualSlots; ++slot) {
      if (model[slot] >= 0) {
        assert(memory.release(kVirtualBase + slot * kBasePageSize));
      }
    }
    assert(memory.validate_invariants());
    assert(memory.free_physical(pool, kPhysicalPages * kBasePageSize));
    assert(memory.validate_invariants());
  }

  // Anonymous commit/decommit/recommit fuzz catches stale reachability and
  // physical-page reuse issues without depending on allocator implementation.
  {
    AddressSpace memory(GuestTranslationMode::Compact);
    assert(memory.initialize());
    constexpr GuestAddress kBase = 0x06000000u;
    constexpr std::uint32_t kSlots = 32u;
    std::array<bool, kSlots> committed{};
    std::array<std::uint32_t, kSlots> values{};
    std::uint32_t rng = 0x5151F00Du ^ fuzz_seed;
    for (std::uint32_t iteration = 0;
         iteration < scaled_iterations(12000u, fuzz_scale); ++iteration) {
      const auto slot = xorshift32(rng) % kSlots;
      const auto va = kBase + slot * kBasePageSize;
      if (!committed[slot]) {
        assert(memory.commit_fixed(va, kBasePageSize, kReadWrite));
        committed[slot] = true;
        values[slot] = xorshift32(rng);
        memory.write32_be(va, values[slot]);
      } else if ((xorshift32(rng) & 3u) == 0u) {
        assert(memory.release(va));
        committed[slot] = false;
        assert(memory.get_physical_address(va) == 0xFFFFFFFFu);
      } else {
        assert(memory.read32_be(va) == values[slot]);
        values[slot] = xorshift32(rng);
        memory.write32_be(va, values[slot]);
      }
      if ((iteration % 101u) == 0u) assert(memory.validate_invariants());
    }
    for (std::uint32_t slot = 0; slot < kSlots; ++slot) {
      if (committed[slot]) assert(memory.release(kBase + slot * kBasePageSize));
    }
    assert(memory.validate_invariants());
  }

  // XEX alias coherence, protection transitions and executable generation
  // invalidation are exercised together so stale native code cannot survive a
  // mapping/protection/write sequence through the alternate XEX view.
  {
    AddressSpace memory(GuestTranslationMode::Compact);
    assert(memory.initialize());
    constexpr GuestAddress kXex = 0x83000000u;
    constexpr GuestAddress kXexAlias = 0x93000000u;
    assert(memory.commit_fixed(kXex, kLargePageSize, kReadWriteExecute));
    memory.write32_be(kXex, 0x60000000u);
    ExecutableCodeCache cache;
    assert(cache.register_current(memory, kXex, kXex, 4u, fuzz_native));
    auto stamp = memory.executable_page_stamp(kXex);
    assert(stamp == memory.executable_page_stamp(kXexAlias));

    std::uint32_t rng = 0xE9EC7AB1u ^ fuzz_seed;
    for (std::uint32_t i = 0; i < scaled_iterations(1000u, fuzz_scale); ++i) {
      const auto prior = stamp;
      if ((xorshift32(rng) & 1u) != 0u) {
        memory.write32_be(kXexAlias, xorshift32(rng));
      } else {
        const auto physical = memory.get_physical_address(kXex);
        std::array<std::byte, 4u> bytes{
            static_cast<std::byte>(xorshift32(rng)), std::byte{0x11},
            std::byte{0x22}, std::byte{0x33}};
        assert(memory.write_physical(physical, bytes));
      }
      stamp = memory.executable_page_stamp(kXex);
      assert(stamp.physical_page == prior.physical_page);
      assert(stamp.generation != prior.generation);
      assert(cache.lookup(memory, kXex) == nullptr);
      assert(cache.register_current(memory, kXex, kXex, 4u, fuzz_native));

      if ((i % 31u) == 0u) {
        assert(memory.protect(kXex, kBasePageSize, kReadWrite));
        assert(!memory.executable_page_stamp(kXex).executable());
        assert(cache.lookup(memory, kXex) == nullptr);
        assert(memory.protect(kXex, kBasePageSize, kReadWriteExecute));
        assert(cache.register_current(memory, kXex, kXex, 4u, fuzz_native));
      }
    }
    assert(memory.validate_invariants());
  }

  // Dirty tracking model: every randomized publication must be observable both
  // in exact journal history while retained and in page-level fallback after
  // the bounded journal wraps. No written page may disappear from the model.
  {
    GuestMemoryCoherency coherency;
    constexpr std::uint32_t kModelPages = 128u;
    std::array<bool, kModelPages> dirty{};
    std::uint32_t rng = 0xD17A7E55u ^ fuzz_seed;
    const auto start = coherency.current_epoch();
    for (std::uint32_t i = 0; i < scaled_iterations(20000u, fuzz_scale, 256u); ++i) {
      const auto page = xorshift32(rng) % kModelPages;
      const auto offset = xorshift32(rng) & (kBasePageSize - 1u);
      const auto width = (std::min)(64u, kBasePageSize - offset);
      coherency.mark_write(page * kBasePageSize + offset, width);
      dirty[page] = true;
    }
    const auto through = coherency.current_epoch();
    std::vector<DirtyPhysicalRange> exact;
    assert(coherency.collect_exact_dirty_ranges(start, through, exact));
    for (std::uint32_t page = 0; page < kModelPages; ++page) {
      if (!dirty[page]) continue;
      bool found = false;
      for (const auto& range : exact) {
        if (range.address / kBasePageSize == page) {
          found = true;
          break;
        }
      }
      assert(found);
    }

    for (std::uint32_t i = 0; i < GuestMemoryCoherency::kWriteJournalCapacity +
                                      1024u;
         ++i) {
      const auto page = xorshift32(rng) % kModelPages;
      coherency.mark_write(page * kBasePageSize, 1u);
      dirty[page] = true;
    }
    const auto wrapped_through = coherency.current_epoch();
    assert(!coherency.collect_exact_dirty_ranges(start, wrapped_through, exact));
    std::vector<DirtyPhysicalRange> page_ranges;
    coherency.collect_dirty_ranges(start, wrapped_through, 0u, page_ranges);
    for (std::uint32_t page = 0; page < kModelPages; ++page) {
      if (dirty[page]) {
        assert(covered_by(page_ranges, page * kBasePageSize, kBasePageSize));
      }
    }
  }

  // Reservation model: writes through aliases and controlled DMA invalidate an
  // overlapping LR/SC reservation; non-overlapping granules do not.
  {
    AddressSpace memory(GuestTranslationMode::Compact);
    assert(memory.initialize());
    constexpr GuestAddress kWord = 0x07000000u;
    assert(memory.commit_fixed(kWord, kBasePageSize, kReadWrite));
    const auto physical = memory.get_physical_address(kWord);
    assert(physical != 0xFFFFFFFFu);
    std::uint32_t rng = 0x13579BDFu ^ fuzz_seed;
    for (std::uint32_t i = 0; i < scaled_iterations(4000u, fuzz_scale); ++i) {
      const auto seed = xorshift32(rng);
      memory.write32_be(kWord, seed);
      std::uint32_t observed{};
      const auto token = memory.reserve32(kWord, observed);
      assert(observed == seed);
      if ((i % 3u) == 0u) {
        memory.write8(kPhysical64KBase + physical + 8u,
                      static_cast<std::uint8_t>(i));
        assert(!memory.store_conditional32(kWord, token, seed + 1u));
      } else if ((i % 3u) == 1u) {
        const std::array<std::byte, 1u> byte{std::byte{0xA5}};
        assert(memory.write_physical(physical + 16u, byte));
        assert(!memory.store_conditional32(kWord, token, seed + 1u));
      } else {
        memory.write8(kPhysical64KBase + physical + kReservationGranuleSize,
                      0x5Au);
        assert(memory.store_conditional32(kWord, token, seed + 1u));
      }
    }
  }

  // Six-thread randomized access against independent cache lines plus a DMA
  // writer. This target is intentionally TSan-friendly: shared control state is
  // atomic and guest memory itself goes through Memory V2 access APIs.
  {
    AddressSpace memory(GuestTranslationMode::Compact);
    assert(memory.initialize());
    constexpr GuestAddress kBase = 0x08000000u;
    constexpr std::uint32_t kThreads = 6u;
    assert(memory.commit_fixed(kBase, kBasePageSize * 2u, kReadWrite));
    const auto physical = memory.get_physical_address(kBase);
    assert(physical != 0xFFFFFFFFu);
    std::atomic<bool> start{false};
    std::array<std::thread, kThreads> workers;
    for (std::uint32_t thread = 0; thread < kThreads; ++thread) {
      workers[thread] = std::thread([&, thread] {
        auto access = memory.access_context();
        std::uint32_t rng = 0x900DF00Du ^ fuzz_seed ^
                            (thread * 0x9E3779B9u);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        const auto thread_base = kBase + thread * 256u;
        for (std::uint32_t i = 0; i < scaled_iterations(20000u, fuzz_scale, 256u); ++i) {
          const auto offset = (xorshift32(rng) & 0xFCu);
          const auto address = thread_base + offset;
          access.write32_be(address, rng);
          (void)access.read32_be(address);
        }
      });
    }
    std::thread dma([&] {
      std::uint32_t rng = 0xD00DFEEDu ^ fuzz_seed;
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      for (std::uint32_t i = 0; i < scaled_iterations(5000u, fuzz_scale); ++i) {
        const auto offset = kBasePageSize + (xorshift32(rng) & 0xFFCu);
        const std::array<std::byte, 4u> bytes{
            static_cast<std::byte>(rng), static_cast<std::byte>(rng >> 8u),
            static_cast<std::byte>(rng >> 16u), static_cast<std::byte>(rng >> 24u)};
        assert(memory.write_physical(physical + offset, bytes));
      }
    });
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    dma.join();
    assert(memory.validate_invariants());
  }

  // GPU/APU read-side snapshots must be data-race-safe with concurrent CPU
  // stores. Production graphics code consumes physical RAM through this path
  // rather than dereferencing physical_data() directly.
  {
    AddressSpace memory(GuestTranslationMode::Compact);
    assert(memory.initialize());
    constexpr GuestAddress kBase = 0x08100000u;
    constexpr std::uint32_t kBytes = 4u * kBasePageSize;
    assert(memory.commit_fixed(kBase, kBytes, kReadWrite));
    const auto physical = memory.get_physical_address(kBase);
    assert(physical != 0xFFFFFFFFu);

    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    std::array<std::thread, 3> writers;
    for (std::uint32_t thread = 0; thread < writers.size(); ++thread) {
      writers[thread] = std::thread([&, thread] {
        auto access = memory.access_context();
        const auto address = kBase + thread * kBasePageSize;
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (std::uint32_t i = 0;
             i < scaled_iterations(12000u, fuzz_scale, 256u); ++i) {
          access.write64_be(address + ((i * 8u) & (kBasePageSize - 8u)),
                            (std::uint64_t{thread + 1u} << 56u) | i);
        }
      });
    }
    std::thread snapshot_reader([&] {
      std::array<std::byte, 3u * kBasePageSize> snapshot{};
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      for (std::uint32_t i = 0;
           i < scaled_iterations(6000u, fuzz_scale, 128u); ++i) {
        if (!memory.copy_physical_range(physical, snapshot)) {
          failed.store(true, std::memory_order_relaxed);
          return;
        }
      }
    });
    start.store(true, std::memory_order_release);
    for (auto& writer : writers) writer.join();
    snapshot_reader.join();
    assert(!failed.load(std::memory_order_relaxed));
    assert(memory.validate_invariants());
  }

  std::cout << "Memory V2 Phase 21 hardening/model-fuzz tests passed "
            << "(seed=" << fuzz_seed << ", scale=" << fuzz_scale << "%)\n";
  return 0;
}

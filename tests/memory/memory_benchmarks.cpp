#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

#include "xenon/memory/address_space.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using xenon::memory::AddressSpace;
using xenon::memory::GuestAddress;

template <typename Function>
void measure(std::string_view name, std::uint64_t operations, Function&& function) {
  const auto begin = Clock::now();
  const auto checksum = function();
  const auto elapsed = std::chrono::duration<double, std::nano>(Clock::now() - begin);
  std::cout << name << ',' << operations << ',' << std::fixed
            << std::setprecision(3) << elapsed.count() / double(operations)
            << ',' << checksum << '\n';
}

}  // namespace

int main() {
  using namespace xenon::memory;
  constexpr GuestAddress base = 0x02000000u;
  constexpr std::uint32_t bytes = 2u * 1024u * 1024u;
  constexpr std::uint64_t iterations = 1'000'000u;

  AddressSpace memory;
  if (!memory.initialize() || !memory.commit_fixed(base, bytes, kReadWrite)) {
    std::cerr << "Memory V2 benchmark setup failed\n";
    return 1;
  }
  auto access = memory.access_context();
  std::cout << "benchmark,operations,ns_per_operation,checksum\n";

  measure("load_store_8", iterations, [&] {
    std::uint64_t sum = 0;
    for (std::uint64_t i = 0; i < iterations; ++i) {
      const auto address = base + static_cast<std::uint32_t>(i & 0xFFFFu);
      access.write8(address, static_cast<std::uint8_t>(i));
      sum += access.read8(address);
    }
    return sum;
  });
  measure("load_store_16_be", iterations, [&] {
    std::uint64_t sum = 0;
    for (std::uint64_t i = 0; i < iterations; ++i) {
      const auto address = base + static_cast<std::uint32_t>((i * 2u) & 0xFFFEu);
      access.write16_be(address, static_cast<std::uint16_t>(i));
      sum += access.read16_be(address);
    }
    return sum;
  });
  measure("load_store_32_be", iterations, [&] {
    std::uint64_t sum = 0;
    for (std::uint64_t i = 0; i < iterations; ++i) {
      const auto address = base + static_cast<std::uint32_t>((i * 4u) & 0xFFFCu);
      access.write32_be(address, static_cast<std::uint32_t>(i));
      sum += access.read32_be(address);
    }
    return sum;
  });
  measure("load_store_64_be", iterations, [&] {
    std::uint64_t sum = 0;
    for (std::uint64_t i = 0; i < iterations; ++i) {
      const auto address = base + static_cast<std::uint32_t>((i * 8u) & 0xFFF8u);
      access.write64_be(address, i);
      sum ^= access.read64_be(address);
    }
    return sum;
  });
  measure("load_store_128", iterations / 4u, [&] {
    xenon::cpu::Vector128 value{};
    std::uint64_t sum = 0;
    for (std::uint64_t i = 0; i < iterations / 4u; ++i) {
      value.set_u32_be(0, static_cast<std::uint32_t>(i >> 32u));
      value.set_u32_be(1, static_cast<std::uint32_t>(i));
      const auto address = base + static_cast<std::uint32_t>((i * 16u) & 0xFFF0u);
      access.write128(address, value);
      const auto returned = access.read128(address);
      sum ^= (std::uint64_t{returned.u32_be(0)} << 32u) |
             returned.u32_be(1);
    }
    return sum;
  });
  measure("random_translation_32", iterations, [&] {
    std::uint32_t random = 0x12345678u;
    std::uint64_t sum = 0;
    for (std::uint64_t i = 0; i < iterations; ++i) {
      random = random * 1664525u + 1013904223u;
      const auto address = base + ((random & (bytes - 4u)) & ~GuestAddress{3u});
      access.write32_be(address, random);
      sum += access.read32_be(address);
    }
    return sum;
  });
  measure("cross_page_64", iterations / 16u, [&] {
    std::uint64_t sum = 0;
    for (std::uint64_t i = 0; i < iterations / 16u; ++i) {
      const auto address = base + 0x1000u - 4u;
      memory.write64_be(address, i);
      sum ^= memory.read64_be(address);
    }
    return sum;
  });

  std::array<std::byte, 64u * 1024u> block{};
  measure("block_write_64k", 256u, [&] {
    for (std::uint32_t i = 0; i < 256u; ++i) {
      access.write_bytes(base + 0x10000u, block);
    }
    return std::uint64_t(access.read8(base + 0x10000u));
  });
  measure("block_zero_64k", 256u, [&] {
    for (std::uint32_t i = 0; i < 256u; ++i) {
      access.fill_bytes(base + 0x10000u, block.size(), 0u);
    }
    return std::uint64_t(access.read8(base + 0x10000u));
  });

  constexpr GuestAddress mmio_base = 0xFFF10000u;
  std::atomic<std::uint64_t> mmio_value{};
  if (!memory.add_mmio_range(
          mmio_base, 0x1000u,
          [&](GuestAddress, std::uint32_t) { return mmio_value.load(); },
          [&](GuestAddress, std::uint32_t, std::uint64_t value) {
            mmio_value.store(value);
          }, "benchmark")) {
    return 2;
  }
  measure("mmio_slow_path_32", iterations / 100u, [&] {
    std::uint64_t sum = 0;
    for (std::uint64_t i = 0; i < iterations / 100u; ++i) {
      memory.write32_be(mmio_base, static_cast<std::uint32_t>(i));
      sum += memory.read32_be(mmio_base);
    }
    return sum;
  });

  measure("reservation_32", iterations / 100u, [&] {
    std::uint64_t sum = 0;
    for (std::uint64_t i = 0; i < iterations / 100u; ++i) {
      std::uint32_t value{};
      const auto token = memory.reserve32(base, value);
      sum += memory.store_conditional32(base, token, value + 1u);
    }
    return sum;
  });

  measure("six_thread_contention", 6u * 100'000u, [&] {
    std::array<std::thread, 6> threads;
    std::array<std::uint64_t, 6> sums{};
    for (std::uint32_t thread = 0; thread < threads.size(); ++thread) {
      threads[thread] = std::thread([&, thread] {
        auto local = memory.access_context();
        const auto address = base + 0x180000u + thread * 128u;
        for (std::uint32_t i = 0; i < 100'000u; ++i) {
          local.write32_be(address, i);
          sums[thread] += local.read32_be(address);
        }
      });
    }
    for (auto& thread : threads) thread.join();
    std::uint64_t sum = 0;
    for (const auto value : sums) sum += value;
    return sum;
  });

  const auto epoch = memory.coherency().current_epoch();
  access.fill_bytes(base, bytes, 0xCCu);
  measure("dirty_range_coalesce_2m", 1u, [&] {
    std::vector<DirtyPhysicalRange> ranges;
    memory.coherency().collect_dirty_ranges(
        epoch, memory.coherency().current_epoch(), 0u, ranges);
    std::uint64_t total = 0;
    for (const auto& range : ranges) total += range.size;
    return total;
  });

  return 0;
}

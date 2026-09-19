#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "xenon/memory/address_space.hpp"
#include "xenon/memory/gpu_coherency.hpp"
#include "xenon/memory/host_vm.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using xenon::memory::AddressSpace;
using xenon::memory::GuestAddress;

struct BenchmarkResult {
  std::string name;
  std::uint64_t operations{};
  std::uint64_t bytes{};
  double seconds{};
  std::uint64_t checksum{};
};

struct BenchmarkMetadata {
  std::string platform;
  std::string architecture;
  std::string compiler;
  std::string build_config;
  std::string translation_mode;
  std::uint32_t pointer_bits{};
  bool fixed_shared_mapping{};
};

enum class OutputFormat { Csv, Json, Human };

struct Options {
  OutputFormat format{OutputFormat::Csv};
  std::string output_path;
  std::uint64_t iterations{1'000'000u};
};

std::string platform_name() {
#if defined(__ANDROID__)
  return "Android";
#elif defined(_WIN32)
  return "Windows";
#elif defined(__linux__)
  return "Linux";
#elif defined(__APPLE__)
  return "macOS";
#else
  return "Unknown";
#endif
}

std::string architecture_name() {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "ARM64";
#elif defined(__x86_64__) || defined(_M_X64)
  return "x86-64";
#elif defined(__i386__) || defined(_M_IX86)
  return "x86";
#else
  return "Unknown";
#endif
}

std::string compiler_name() {
#if defined(__clang__)
  return "Clang " + std::to_string(__clang_major__) + "." +
         std::to_string(__clang_minor__);
#elif defined(__GNUC__)
  return "GCC " + std::to_string(__GNUC__) + "." +
         std::to_string(__GNUC_MINOR__);
#elif defined(_MSC_VER)
  return "MSVC " + std::to_string(_MSC_VER);
#else
  return "Unknown";
#endif
}

std::string json_escape(std::string_view input) {
  std::string out;
  out.reserve(input.size() + 8u);
  for (const char c : input) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c; break;
    }
  }
  return out;
}

bool parse_options(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--help") {
      std::cout
          << "Usage: xenon_memory_v2_benchmarks [--format=csv|json|human] "
             "[--output=PATH] [--iterations=N]\n";
      return false;
    }
    if (arg.starts_with("--format=")) {
      const auto value = arg.substr(9u);
      if (value == "csv") options.format = OutputFormat::Csv;
      else if (value == "json") options.format = OutputFormat::Json;
      else if (value == "human") options.format = OutputFormat::Human;
      else {
        std::cerr << "Unknown benchmark format: " << value << '\n';
        return false;
      }
      continue;
    }
    if (arg.starts_with("--output=")) {
      options.output_path = std::string(arg.substr(9u));
      continue;
    }
    if (arg.starts_with("--iterations=")) {
      const auto value = std::string(arg.substr(13u));
      char* end = nullptr;
      const auto parsed = std::strtoull(value.c_str(), &end, 10);
      if (!end || *end != '\0' || parsed == 0u) {
        std::cerr << "Invalid --iterations value\n";
        return false;
      }
      options.iterations = parsed;
      continue;
    }
    std::cerr << "Unknown benchmark option: " << arg << '\n';
    return false;
  }
  return true;
}

template <typename Function>
void measure(std::vector<BenchmarkResult>& results, std::string_view name,
             std::uint64_t operations, std::uint64_t bytes, Function&& function) {
  const auto begin = Clock::now();
  const auto checksum = static_cast<std::uint64_t>(function());
  const auto elapsed = std::chrono::duration<double>(Clock::now() - begin).count();
  results.push_back(
      {std::string(name), operations, bytes, elapsed, checksum});
}

double ns_per_operation(const BenchmarkResult& result) {
  return result.operations
             ? result.seconds * 1'000'000'000.0 /
                   static_cast<double>(result.operations)
             : 0.0;
}

double gib_per_second(const BenchmarkResult& result) {
  return result.bytes && result.seconds > 0.0
             ? (static_cast<double>(result.bytes) / (1024.0 * 1024.0 * 1024.0)) /
                   result.seconds
             : 0.0;
}

void emit_csv(std::ostream& out, const BenchmarkMetadata& metadata,
              const std::vector<BenchmarkResult>& results) {
  out << "# platform=" << metadata.platform << '\n'
      << "# architecture=" << metadata.architecture << '\n'
      << "# compiler=" << metadata.compiler << '\n'
      << "# build_config=" << metadata.build_config << '\n'
      << "# translation_mode=" << metadata.translation_mode << '\n'
      << "# pointer_bits=" << metadata.pointer_bits << '\n'
      << "# fixed_shared_mapping=" << (metadata.fixed_shared_mapping ? 1 : 0)
      << '\n';
  out << "benchmark,operations,bytes,seconds,ns_per_operation,gib_per_second,checksum\n";
  out << std::fixed << std::setprecision(3);
  for (const auto& result : results) {
    out << result.name << ',' << result.operations << ',' << result.bytes << ','
        << std::setprecision(9) << result.seconds << ',' << std::setprecision(3)
        << ns_per_operation(result) << ',' << gib_per_second(result) << ','
        << result.checksum << '\n';
  }
}

void emit_json(std::ostream& out, const BenchmarkMetadata& metadata,
               const std::vector<BenchmarkResult>& results) {
  out << "{\n  \"schema\": \"xenon-memory-v2-benchmark-v1\",\n"
      << "  \"platform\": \"" << json_escape(metadata.platform) << "\",\n"
      << "  \"architecture\": \"" << json_escape(metadata.architecture)
      << "\",\n"
      << "  \"compiler\": \"" << json_escape(metadata.compiler) << "\",\n"
      << "  \"build_config\": \"" << json_escape(metadata.build_config)
      << "\",\n"
      << "  \"translation_mode\": \""
      << json_escape(metadata.translation_mode) << "\",\n"
      << "  \"pointer_bits\": " << metadata.pointer_bits << ",\n"
      << "  \"fixed_shared_mapping\": "
      << (metadata.fixed_shared_mapping ? "true" : "false") << ",\n"
      << "  \"benchmarks\": [\n";
  for (std::size_t i = 0; i < results.size(); ++i) {
    const auto& result = results[i];
    out << "    {\"name\": \"" << json_escape(result.name)
        << "\", \"operations\": " << result.operations
        << ", \"bytes\": " << result.bytes << ", \"seconds\": "
        << std::fixed << std::setprecision(9) << result.seconds
        << ", \"ns_per_operation\": " << std::setprecision(3)
        << ns_per_operation(result) << ", \"gib_per_second\": "
        << gib_per_second(result) << ", \"checksum\": " << result.checksum
        << "}" << (i + 1u == results.size() ? "\n" : ",\n");
  }
  out << "  ]\n}\n";
}

void emit_human(std::ostream& out, const BenchmarkMetadata& metadata,
                const std::vector<BenchmarkResult>& results) {
  out << "Xenon Memory V2 benchmark report\n"
      << "Platform: " << metadata.platform << ' ' << metadata.architecture
      << " | Compiler: " << metadata.compiler
      << " | Build: " << metadata.build_config
      << " | Translation: " << metadata.translation_mode << "\n\n";
  out << std::left << std::setw(40) << "Benchmark" << std::right
      << std::setw(14) << "ns/op" << std::setw(14) << "GiB/s"
      << std::setw(16) << "operations" << '\n';
  for (const auto& result : results) {
    out << std::left << std::setw(40) << result.name << std::right << std::fixed
        << std::setprecision(3) << std::setw(14) << ns_per_operation(result)
        << std::setw(14) << gib_per_second(result) << std::setw(16)
        << result.operations << '\n';
  }
}

std::uint32_t xorshift32(std::uint32_t& state) {
  state ^= state << 13u;
  state ^= state >> 17u;
  state ^= state << 5u;
  return state;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace xenon::memory;

  Options options;
  if (!parse_options(argc, argv, options)) return argc > 1 ? 2 : 0;

  constexpr GuestAddress base = 0x02000000u;
  constexpr std::uint32_t bytes = 2u * 1024u * 1024u;
  const std::uint64_t iterations = options.iterations;
  std::vector<BenchmarkResult> results;
  results.reserve(32u);

  AddressSpace memory;
  if (!memory.initialize() || !memory.commit_fixed(base, bytes, kReadWrite)) {
    std::cerr << "Memory V2 benchmark setup failed\n";
    return 1;
  }
  auto access = memory.access_context();

  BenchmarkMetadata metadata{
      platform_name(), architecture_name(), compiler_name(),
#ifdef XENON_BENCHMARK_BUILD_CONFIG
      XENON_BENCHMARK_BUILD_CONFIG,
#else
      "unknown",
#endif
      memory.direct_aperture_active() ? "direct-aperture" : "compact",
      static_cast<std::uint32_t>(sizeof(void*) * 8u),
      host_vm::supports_fixed_shared_mapping()};

  measure(results, "load_store_8", iterations, iterations * 2u, [&] {
    std::uint64_t sum = 0;
    for (std::uint64_t i = 0; i < iterations; ++i) {
      const auto address = base + static_cast<std::uint32_t>(i & 0xFFFFu);
      access.write8(address, static_cast<std::uint8_t>(i));
      sum += access.read8(address);
    }
    return sum;
  });
  measure(results, "load_store_16_be", iterations, iterations * 4u, [&] {
    std::uint64_t sum = 0;
    for (std::uint64_t i = 0; i < iterations; ++i) {
      const auto address = base + static_cast<std::uint32_t>((i * 2u) & 0xFFFEu);
      access.write16_be(address, static_cast<std::uint16_t>(i));
      sum += access.read16_be(address);
    }
    return sum;
  });
  measure(results, "load_store_32_be", iterations, iterations * 8u, [&] {
    std::uint64_t sum = 0;
    for (std::uint64_t i = 0; i < iterations; ++i) {
      const auto address = base + static_cast<std::uint32_t>((i * 4u) & 0xFFFCu);
      access.write32_be(address, static_cast<std::uint32_t>(i));
      sum += access.read32_be(address);
    }
    return sum;
  });
  measure(results, "load_store_64_be", iterations, iterations * 16u, [&] {
    std::uint64_t sum = 0;
    for (std::uint64_t i = 0; i < iterations; ++i) {
      const auto address = base + static_cast<std::uint32_t>((i * 8u) & 0xFFF8u);
      access.write64_be(address, i);
      sum ^= access.read64_be(address);
    }
    return sum;
  });
  measure(results, "load_store_128", (std::max)(std::uint64_t{1}, iterations / 4u),
          (std::max)(std::uint64_t{1}, iterations / 4u) * 32u, [&] {
    xenon::cpu::Vector128 value{};
    std::uint64_t sum = 0;
    const auto count = (std::max)(std::uint64_t{1}, iterations / 4u);
    for (std::uint64_t i = 0; i < count; ++i) {
      value.set_u32_be(0, static_cast<std::uint32_t>(i >> 32u));
      value.set_u32_be(1, static_cast<std::uint32_t>(i));
      const auto address = base + static_cast<std::uint32_t>((i * 16u) & 0xFFF0u);
      access.write128(address, value);
      const auto returned = access.read128(address);
      sum ^= (std::uint64_t{returned.u32_be(0)} << 32u) | returned.u32_be(1);
    }
    return sum;
  });

  measure(results, "sequential_ram_32", iterations, iterations * 8u, [&] {
    std::uint64_t sum = 0;
    constexpr std::uint32_t mask = bytes - 4u;
    for (std::uint64_t i = 0; i < iterations; ++i) {
      const auto address = base + (static_cast<std::uint32_t>(i * 4u) & mask & ~3u);
      access.write32_be(address, static_cast<std::uint32_t>(i));
      sum += access.read32_be(address);
    }
    return sum;
  });
  measure(results, "random_ram_32", iterations, iterations * 8u, [&] {
    std::uint32_t random = 0x12345678u;
    std::uint64_t sum = 0;
    for (std::uint64_t i = 0; i < iterations; ++i) {
      xorshift32(random);
      const auto address = base + ((random & (bytes - 4u)) & ~GuestAddress{3u});
      access.write32_be(address, random);
      sum += access.read32_be(address);
    }
    return sum;
  });

  AddressSpace compact_memory(GuestTranslationMode::Compact);
  if (!compact_memory.initialize() ||
      !compact_memory.commit_fixed(base, bytes, kReadWrite)) {
    std::cerr << "Compact benchmark setup failed\n";
    return 3;
  }
  auto compact = compact_memory.access_context();
  measure(results, "guest_to_host_translation_compact_32", iterations,
          iterations * 4u, [&] {
    std::uint32_t random = 0x12345678u;
    std::uint64_t sum = 0;
    for (std::uint64_t i = 0; i < iterations; ++i) {
      xorshift32(random);
      const auto address = base + ((random & (bytes - 4u)) & ~GuestAddress{3u});
      sum += compact.read32_be(address);
    }
    return sum;
  });
  if (host_vm::supports_fixed_shared_mapping() && sizeof(void*) >= 8u) {
    AddressSpace direct_memory(GuestTranslationMode::DirectAperture);
    if (direct_memory.initialize() && direct_memory.direct_aperture_active() &&
        direct_memory.commit_fixed(base, bytes, kReadWrite)) {
      auto direct = direct_memory.access_context();
      measure(results, "guest_to_host_translation_direct_32", iterations,
              iterations * 4u, [&] {
        std::uint32_t random = 0x12345678u;
        std::uint64_t sum = 0;
        for (std::uint64_t i = 0; i < iterations; ++i) {
          xorshift32(random);
          const auto address =
              base + ((random & (bytes - 4u)) & ~GuestAddress{3u});
          sum += direct.read32_be(address);
        }
        return sum;
      });
    }
  }

  measure(results, "cross_page_64", (std::max)(std::uint64_t{1}, iterations / 16u),
          (std::max)(std::uint64_t{1}, iterations / 16u) * 16u, [&] {
    std::uint64_t sum = 0;
    const auto count = (std::max)(std::uint64_t{1}, iterations / 16u);
    for (std::uint64_t i = 0; i < count; ++i) {
      const auto address = base + 0x1000u - 4u;
      memory.write64_be(address, i);
      sum ^= memory.read64_be(address);
    }
    return sum;
  });

  const auto physical_base = memory.get_physical_address(base);
  if (physical_base == 0xFFFFFFFFu) return 5;
  measure(results, "physical_alias_32", iterations, iterations * 8u, [&] {
    std::uint64_t sum = 0;
    const auto alias = kPhysical64KBase + physical_base;
    for (std::uint64_t i = 0; i < iterations; ++i) {
      access.write32_be(base, static_cast<std::uint32_t>(i));
      sum += access.read32_be(alias);
    }
    return sum;
  });

  std::array<std::byte, 64u * 1024u> block{};
  for (std::size_t i = 0; i < block.size(); ++i) {
    block[i] = static_cast<std::byte>(i & 0xFFu);
  }
  access.write_bytes(base + 0x10000u, block);
  measure(results, "block_write_64k", 256u, 256u * block.size(), [&] {
    for (std::uint32_t i = 0; i < 256u; ++i) {
      access.write_bytes(base + 0x10000u, block);
    }
    return std::uint64_t(access.read8(base + 0x10000u));
  });
  measure(results, "block_zero_64k", 256u, 256u * block.size(), [&] {
    for (std::uint32_t i = 0; i < 256u; ++i) {
      access.fill_bytes(base + 0x10000u, static_cast<std::uint32_t>(block.size()),
                        0u);
    }
    return std::uint64_t(access.read8(base + 0x10000u));
  });
  access.write_bytes(base + 0x10000u, block);
  measure(results, "block_copy_64k", 256u, 256u * block.size(), [&] {
    for (std::uint32_t i = 0; i < 256u; ++i) {
      memory.copy(base + 0x20000u, base + 0x10000u,
                  static_cast<std::uint32_t>(block.size()));
    }
    return std::uint64_t(access.read8(base + 0x20000u + 123u));
  });

  constexpr GuestAddress mmio_base = 0xFFF10000u;
  std::atomic<std::uint64_t> mmio_value{};
  if (!memory.add_mmio_range(
          mmio_base, 0x1000u,
          [&](GuestAddress, std::uint32_t) { return mmio_value.load(); },
          [&](GuestAddress, std::uint32_t, std::uint64_t value) {
            mmio_value.store(value);
          },
          "benchmark")) {
    return 6;
  }
  const auto mmio_iterations = (std::max)(std::uint64_t{1}, iterations / 100u);
  measure(results, "mmio_slow_path_32", mmio_iterations, mmio_iterations * 8u,
          [&] {
    std::uint64_t sum = 0;
    for (std::uint64_t i = 0; i < mmio_iterations; ++i) {
      memory.write32_be(mmio_base, static_cast<std::uint32_t>(i));
      sum += memory.read32_be(mmio_base);
    }
    return sum;
  });

  constexpr GuestAddress mmio_catalogue_base = 0xFFD00000u;
  constexpr std::uint32_t mmio_catalogue_count = 256u;
  for (std::uint32_t i = 0; i < mmio_catalogue_count; ++i) {
    const auto device_base = mmio_catalogue_base + i * kBasePageSize;
    if (!memory.add_mmio_range(
            device_base, kBasePageSize,
            [i](GuestAddress, std::uint32_t) -> std::uint64_t { return i; },
            [](GuestAddress, std::uint32_t, std::uint64_t) {},
            "benchmark-catalogue")) {
      return 7;
    }
  }
  measure(results, "normal_ram_with_256_mmio_devices", iterations,
          iterations * 8u, [&] {
    std::uint64_t sum = 0;
    for (std::uint64_t i = 0; i < iterations; ++i) {
      const auto address = base + static_cast<std::uint32_t>((i * 4u) & 0xFFFCu);
      access.write32_be(address, static_cast<std::uint32_t>(i));
      sum += access.read32_be(address);
    }
    return sum;
  });

  const auto reservation_iterations = (std::max)(std::uint64_t{1}, iterations / 100u);
  measure(results, "reservation_32", reservation_iterations,
          reservation_iterations * 8u, [&] {
    std::uint64_t sum = 0;
    for (std::uint64_t i = 0; i < reservation_iterations; ++i) {
      std::uint32_t value{};
      const auto token = memory.reserve32(base, value);
      sum += memory.store_conditional32(base, token, value + 1u);
    }
    return sum;
  });

  measure(results, "six_thread_contention", 6u * 100'000u,
          6u * 100'000u * 8u, [&] {
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
  measure(results, "dirty_range_coalesce_2m", 1u, bytes, [&] {
    std::vector<DirtyPhysicalRange> ranges;
    memory.coherency().collect_dirty_ranges(
        epoch, memory.coherency().current_epoch(), 0u, ranges);
    std::uint64_t total = 0;
    for (const auto& range : ranges) total += range.size;
    return total;
  });

  // Cold allocator measurements. Keep this independent of the 2 MiB virtual
  // working set so allocation/free timing does not perturb the RAM benchmarks.
  AddressSpace allocator_memory(GuestTranslationMode::Compact);
  if (!allocator_memory.initialize()) return 8;
  const auto alloc_iterations =
      static_cast<std::uint32_t>((std::max)(std::uint64_t{1000}, iterations / 100u));
  measure(results, "physical_allocate_free_4k", alloc_iterations * 2ull,
          alloc_iterations * kBasePageSize, [&] {
    std::uint64_t sum = 0;
    for (std::uint32_t i = 0; i < alloc_iterations; ++i) {
      std::uint32_t physical{};
      if (!allocator_memory.allocate_physical(kBasePageSize, kBasePageSize,
                                               (i & 1u) != 0u, physical)) {
        return std::numeric_limits<std::uint64_t>::max();
      }
      sum += physical;
      if (!allocator_memory.free_physical(physical, kBasePageSize)) {
        return std::numeric_limits<std::uint64_t>::max();
      }
    }
    return sum;
  });

  struct Allocation { std::uint32_t base{}; std::uint32_t size{}; };
  std::vector<Allocation> live;
  live.reserve(512u);
  std::uint32_t rng = 0xBADC0FFEu;
  measure(results, "physical_fragmentation_mixed", 4096u, 0u, [&] {
    std::uint64_t sum = 0;
    for (std::uint32_t i = 0; i < 4096u; ++i) {
      xorshift32(rng);
      const bool do_free = !live.empty() && ((rng & 3u) == 0u || live.size() > 384u);
      if (do_free) {
        const auto index = static_cast<std::size_t>(rng % live.size());
        const auto allocation = live[index];
        if (!allocator_memory.free_physical(allocation.base, allocation.size)) {
          return std::numeric_limits<std::uint64_t>::max();
        }
        live[index] = live.back();
        live.pop_back();
        sum ^= allocation.base;
      } else {
        const auto pages = 1u << ((rng >> 4u) & 3u);
        const auto alignment = kBasePageSize << ((rng >> 8u) & 2u);
        std::uint32_t physical{};
        if (allocator_memory.allocate_physical(pages * kBasePageSize, alignment,
                                               (rng & 0x1000u) != 0u, physical)) {
          live.push_back({physical, pages * kBasePageSize});
          sum += physical;
        }
      }
    }
    for (const auto& allocation : live) {
      if (!allocator_memory.free_physical(allocation.base, allocation.size)) {
        return std::numeric_limits<std::uint64_t>::max();
      }
    }
    live.clear();
    return sum;
  });

  // Backend-neutral CPU->GPU synchronization planning and the exact snapshot
  // operation used by the discrete Vulkan/D3D12 mirrors. This is intentionally
  // hardware-independent so one results schema works on every target. Native
  // API queue/copy bandwidth can be layered on top by backend-specific tools.
  constexpr std::uint32_t gpu_bytes = 4u * 1024u * 1024u;
  std::uint32_t gpu_physical{};
  if (!allocator_memory.allocate_physical(gpu_bytes, kBasePageSize, false,
                                           gpu_physical)) {
    return 9;
  }
  GuestMemoryGpuCoherency gpu_tracker;
  gpu_tracker.reset(kPhysicalMemorySize, false, GpuMemoryTopology::DiscreteMirror);
  const auto gpu_iterations = 16u;
  measure(results, "cpu_to_gpu_sync_plan_4m", gpu_iterations,
          std::uint64_t{gpu_iterations} * gpu_bytes, [&] {
    std::uint64_t sum = 0;
    for (std::uint32_t i = 0; i < gpu_iterations; ++i) {
      allocator_memory.coherency().mark_write(gpu_physical, gpu_bytes);
      const auto plan = gpu_tracker.plan_upload(
          allocator_memory.coherency(), gpu_physical, gpu_bytes,
          1u * 1024u * 1024u, GpuRangeUsage::VertexBuffer);
      for (const auto& range : plan.ranges) {
        sum += range.size;
        gpu_tracker.commit_cpu_upload(range.address, range.size);
      }
    }
    return sum;
  });

  std::vector<std::byte> upload_staging(1u * 1024u * 1024u);
  measure(results, "gpu_upload_snapshot_bandwidth_4m", gpu_iterations,
          std::uint64_t{gpu_iterations} * gpu_bytes, [&] {
    std::uint64_t sum = 0;
    for (std::uint32_t i = 0; i < gpu_iterations; ++i) {
      allocator_memory.coherency().mark_write(gpu_physical, gpu_bytes);
      const auto plan = gpu_tracker.plan_upload(
          allocator_memory.coherency(), gpu_physical, gpu_bytes,
          static_cast<std::uint32_t>(upload_staging.size()),
          GpuRangeUsage::VertexBuffer);
      for (const auto& range : plan.ranges) {
        if (!allocator_memory.copy_physical_range(
                range.address,
                std::span<std::byte>(upload_staging).first(range.size))) {
          return std::numeric_limits<std::uint64_t>::max();
        }
        gpu_tracker.commit_cpu_upload(range.address, range.size);
        sum ^= static_cast<std::uint8_t>(upload_staging[0]);
        sum += range.size;
      }
    }
    return sum;
  });
  if (!allocator_memory.free_physical(gpu_physical, gpu_bytes)) return 10;

  std::unique_ptr<std::ofstream> file;
  std::ostream* output = &std::cout;
  if (!options.output_path.empty()) {
    file = std::make_unique<std::ofstream>(options.output_path,
                                           std::ios::out | std::ios::trunc);
    if (!*file) {
      std::cerr << "Unable to open benchmark output: " << options.output_path
                << '\n';
      return 11;
    }
    output = file.get();
  }
  switch (options.format) {
    case OutputFormat::Csv: emit_csv(*output, metadata, results); break;
    case OutputFormat::Json: emit_json(*output, metadata, results); break;
    case OutputFormat::Human: emit_human(*output, metadata, results); break;
  }
  return 0;
}

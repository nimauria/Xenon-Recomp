// gpu-replay: replays a portable GPU capture (a live recording of a running
// title - XENON_GPU_CAPTURE=1, see gpu_pump.cpp - or a single submission)
// through a real native backend, repeatedly, and reports what it cost: a
// deterministic renderer benchmark of a real workload, without the game.
//
// Each iteration restores the captured guest memory, plays every command and
// waits for the GPU (as a frame's flip does). The captured EDRAM is loaded
// (and the backend's native render targets discarded) in the warm-up only,
// so measured iterations run with render targets kept from the previous one,
// as a running title's frames do; --cold-edram reloads it every iteration,
// measuring the canonical EDRAM loads instead. Reported: wall time per
// iteration (restore and consume apart), host microseconds per draw, GPU
// busy/idle, submissions, fence waits, index cache and resolve counts.
// --profile samples every thread with the host sampler. --verify reloads
// EDRAM every iteration, makes it and the captured resolve destinations
// canonical afterwards and checks every iteration produced the same bytes
// (the replay is deterministic).
//
//   gpu-replay <capture.xgc> [--iterations N] [--warmup N] [--vulkan]
//              [--cold-edram] [--profile] [--verify]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "xenon/gpu/capture.hpp"
#include "xenon/gpu/graphics_system.hpp"
#include "xenon/gpu/perf_stats.hpp"
#include "xenon/logging/host_sampler.hpp"
#include "xenon/memory/address_space.hpp"

#if defined(XENON_GPU_REPLAY_D3D12)
#include "xenon/gpu/d3d12/backend.hpp"
#endif
#if defined(XENON_GPU_REPLAY_VULKAN)
#include "xenon/gpu/vulkan/backend.hpp"
#endif

using namespace xenon::gpu;

namespace {

struct Options {
  std::string path{};
  std::uint32_t iterations{20};
  std::uint32_t warmup{3};
  bool vulkan{};
  bool profile{};
  bool verify{};
  bool cold_edram{};
};

bool parse(int argc, char** argv, Options& options) {
  const auto usage = [] {
    std::cerr << "usage: gpu-replay <capture.xgc> [--iterations N] [--warmup N] [--vulkan] "
                 "[--cold-edram] [--profile] [--verify]\n";
    return false;
  };
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto number = [&](std::uint32_t& out) {
      if (i + 1 >= argc) return false;
      char* end = nullptr;
      const auto value = std::strtoul(argv[++i], &end, 10);
      if (!end || *end != '\0') return false;
      out = static_cast<std::uint32_t>(value);
      return true;
    };
    if (arg == "--iterations") {
      if (!number(options.iterations) || !options.iterations) return usage();
    } else if (arg == "--warmup") {
      if (!number(options.warmup)) return usage();
    } else if (arg == "--vulkan") {
      options.vulkan = true;
    } else if (arg == "--profile") {
      options.profile = true;
    } else if (arg == "--verify") {
      options.verify = true;
    } else if (arg == "--cold-edram") {
      options.cold_edram = true;
    } else if (!arg.empty() && arg[0] != '-' && options.path.empty()) {
      options.path = std::string(arg);
    } else {
      return usage();
    }
  }
  return options.path.empty() ? usage() : true;
}

std::uint64_t hash_bytes(std::uint64_t hash, std::span<const std::byte> bytes) {
  for (const auto byte : bytes) hash = (hash ^ std::to_integer<std::uint64_t>(byte)) * 1099511628211ull;
  return hash;
}

template <typename Backend>
int run(Backend& backend, const Options& options, const PortableSubmissionCapture& capture) {
  xenon::memory::AddressSpace memory;
  if (!memory.initialize()) {
    std::cerr << "gpu-replay: guest memory could not be initialized\n";
    return 1;
  }
  GraphicsSystem graphics(memory);
  std::uint64_t draws = 0, resolves = 0;
  for (const auto& command : capture.frontend.commands)
    if (std::holds_alternative<ir::DrawPacket>(command)) ++draws;

  // The bytes an iteration produced: canonical EDRAM and the captured
  // resolve destinations, read back through Memory V2.
  const auto produced = [&]() -> std::optional<std::uint64_t> {
    if (!graphics.make_edram_canonical(backend)) return std::nullopt;
    auto hash = hash_bytes(1469598103934665603ull, graphics.edram().bytes());
    for (const auto& range : capture.physical_ranges) {
      if ((range.usage & CaptureRangeUsage::ResolveDestination) == CaptureRangeUsage::None)
        continue;
      const auto size = static_cast<std::uint32_t>(range.bytes.size());
      if (!graphics.make_guest_memory_cpu_visible(backend, range.physical_address, size))
        return std::nullopt;
      std::vector<std::byte> bytes(size);
      if (!memory.copy_physical_range(range.physical_address, bytes)) return std::nullopt;
      hash = hash_bytes(hash, bytes);
    }
    return hash;
  };

  GraphicsSystem::ReplayTiming timing{};
  bool edram_loaded = false;
  const auto iteration = [&]() {
    std::string error;
    const bool restore_edram = options.cold_edram || options.verify || !edram_loaded;
    edram_loaded = true;
    if (!graphics.replay_portable_capture(backend, capture, &error, &timing, restore_edram)) {
      std::cerr << "gpu-replay: replay failed: " << error << "\n";
      return false;
    }
    // Wait for the GPU, as the frame's flip does.
    backend.begin_submission(memory, graphics.edram());
    const bool published = backend.publish_gpu_writes(/*wait=*/true);
    backend.end_submission();
    if (!published) {
      std::cerr << "gpu-replay: waiting for the GPU failed: " << backend.last_error() << "\n";
      return false;
    }
    return true;
  };

  for (std::uint32_t i = 0; i < options.warmup; ++i)
    if (!iteration()) return 1;

  xenon::logging::HostSampler sampler;
  if (options.profile) {
    xenon::logging::HostSamplerOptions sampling{};
    sampling.min_thread_samples = 20;
    sampling.top_functions = 40;
    (void)sampler.start(sampling);
  }
  const auto peek = [](perf::Counter counter) { return perf::peek(counter); };
  const auto gpu_busy_before = peek(perf::Counter::GpuBusyNs);
  const auto gpu_idle_before = peek(perf::Counter::GpuIdleNs);
  const auto fence_before = peek(perf::Counter::FenceWaitNs);
  const auto submits_before = peek(perf::Counter::Submits);
  const auto hits_before = peek(perf::Counter::IndexCacheHits);
  const auto misses_before = peek(perf::Counter::IndexCacheMisses);
  const auto resolves_before = peek(perf::Counter::Resolves);
  const auto edram_before = peek(perf::Counter::EdramCpuTransfers);
  const auto readback_before = peek(perf::Counter::ReadbackBytes);

  std::vector<double> total_ms, restore_ms, consume_ms;
  std::optional<std::uint64_t> first_hash;
  bool deterministic = true;
  for (std::uint32_t i = 0; i < options.iterations; ++i) {
    const auto start = std::chrono::steady_clock::now();
    if (!iteration()) return 1;
    total_ms.push_back(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    restore_ms.push_back(double(timing.restore_ns) / 1e6);
    consume_ms.push_back(double(timing.consume_ns) / 1e6);
    if (options.verify) {
      const auto hash = produced();
      if (!hash) {
        std::cerr << "gpu-replay: could not read back the produced bytes: " << backend.last_error()
                  << "\n";
        return 1;
      }
      if (!first_hash) first_hash = hash;
      if (*hash != *first_hash) deterministic = false;
    }
  }
  if (options.profile) sampler.stop();
  resolves = peek(perf::Counter::Resolves) - resolves_before;

  const auto summary = [](std::vector<double> values) {
    std::sort(values.begin(), values.end());
    double sum = 0;
    for (const auto v : values) sum += v;
    const auto at = [&](double p) {
      return values[(std::min)(values.size() - 1u,
                               static_cast<std::size_t>(p * double(values.size() - 1u) + 0.5))];
    };
    return "mean " + std::to_string(sum / double(values.size())) + ", p50 " +
           std::to_string(at(0.5)) + ", p99 " + std::to_string(at(0.99)) + ", max " +
           std::to_string(values.back());
  };
  double consume_sum = 0;
  for (const auto v : consume_ms) consume_sum += v;
  const double n = options.iterations;
  std::cout << "gpu-replay (" << (options.vulkan ? "Vulkan" : "D3D12") << ", "
            << (options.cold_edram || options.verify ? "EDRAM reloaded every iteration"
                                                     : "steady EDRAM")
            << "): " << options.path
            << "\n  " << capture.frontend.commands.size() << " commands, " << draws
            << " draws, " << capture.physical_ranges.size() << " guest ranges ("
            << capture.physical_byte_count() / (1024u * 1024u) << " MiB)"
            << (capture.complete ? "" : ", incomplete capture") << "\n"
            << "  iteration ms: " << summary(total_ms) << "\n"
            << "  restore ms:   " << summary(restore_ms) << "\n"
            << "  consume ms:   " << summary(consume_ms) << "\n"
            << "  host us per draw (consume, wall): "
            << (draws ? consume_sum * 1000.0 / (n * double(draws)) : 0.0) << "\n"
            << "  per iteration: resolves " << double(resolves) / n << ", GPU busy "
            << double(peek(perf::Counter::GpuBusyNs) - gpu_busy_before) / 1e6 / n
            << " ms, GPU idle between batches "
            << double(peek(perf::Counter::GpuIdleNs) - gpu_idle_before) / 1e6 / n
            << " ms, host fence waits "
            << double(peek(perf::Counter::FenceWaitNs) - fence_before) / 1e6 / n
            << " ms, submissions " << double(peek(perf::Counter::Submits) - submits_before) / n
            << "\n  index cache: " << peek(perf::Counter::IndexCacheHits) - hits_before
            << " hits, " << peek(perf::Counter::IndexCacheMisses) - misses_before
            << " misses; CPU EDRAM transfers "
            << peek(perf::Counter::EdramCpuTransfers) - edram_before << "; readback "
            << double(peek(perf::Counter::ReadbackBytes) - readback_before) / (1024.0 * 1024.0) / n
            << " MiB per iteration\n";
  for (const auto& diagnostic : capture.diagnostics) std::cout << "  note: " << diagnostic << "\n";
  if (options.verify)
    std::cout << "  deterministic: " << (deterministic ? "yes" : "NO - iterations differ") << "\n";
  if (options.profile) std::cout << "\n" << sampler.report();
  return options.verify && !deterministic ? 3 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options options{};
  if (!parse(argc, argv, options)) return 2;
  xenon::logging::set_current_thread_name("gpu-replay");
  std::string error;
  const auto capture = load_portable_capture(options.path, &error);
  if (!capture) {
    std::cerr << "gpu-replay: " << options.path << ": " << error << "\n";
    return 2;
  }
  if (options.vulkan) {
#if defined(XENON_GPU_REPLAY_VULKAN)
    xenon::gpu::vulkan::Backend backend;
    if (!backend.initialize({.enable_validation = false})) {
      std::cerr << "gpu-replay: Vulkan backend: " << backend.error() << "\n";
      return 2;
    }
    return run(backend, options, *capture);
#else
    std::cerr << "gpu-replay: built without the Vulkan backend\n";
    return 2;
#endif
  }
#if defined(XENON_GPU_REPLAY_D3D12)
  xenon::gpu::d3d12::Backend backend;
  if (!backend.initialize({.enable_debug_layer = false, .allow_software_adapter = false})) {
    std::cerr << "gpu-replay: D3D12 backend: " << backend.error() << "\n";
    return 2;
  }
  return run(backend, options, *capture);
#else
  std::cerr << "gpu-replay: no default backend on this host; pass --vulkan\n";
  return 2;
#endif
}

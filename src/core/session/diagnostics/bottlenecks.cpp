// bottlenecks.log: the run's ranked bottlenecks (see bottleneck_report.hpp),
// rewritten by the performance watch every few seconds and at stop from the
// GPU perf counters and what the watch saw.

#include <chrono>
#include <filesystem>
#include <fstream>

#include "xenon/core/bottleneck_report.hpp"
#include "xenon/core/session.hpp"
#include "xenon/gpu/perf_stats.hpp"
#include "xenon/logging/probe_log.hpp"

namespace xenon::core {

void XenonSession::write_bottleneck_report() {
  using gpu::perf::Counter;
  using gpu::perf::lifetime;
  const auto ms = [](Counter counter) { return static_cast<double>(lifetime(counter)) / 1e6; };
  const auto n = [](Counter counter) { return lifetime(counter); };
  std::vector<BottleneckItem> items = bottleneck_ledger_.items();
  const auto add = [&](const char* name, Counter time, Counter count = Counter::Count) {
    items.push_back({"gpu", name, ms(time), count == Counter::Count ? 0u : n(count), 0.0});
  };
  add("ring decode (frontend)", Counter::RingNs);
  add("IR playback", Counter::IrNs);
  add("host present", Counter::PresentNs);
  add("fence wait on host GPU", Counter::FenceWaitNs, Counter::FenceWaits);
  add("shader compile on draw path", Counter::ShaderCompileNs, Counter::ShaderCompiles);
  add("pipeline creation", Counter::PipelineCreateNs, Counter::PipelineCreates);
  add("draw waited for shader", Counter::ShaderWaitNs, Counter::ShaderWaits);
  add("readback wait", Counter::ReadbackWaitNs, Counter::ReadbackWaits);
  add("guest writes held off by readback", Counter::ReadbackWindowNs);
  add("frontend waiting for backend", Counter::BackendWaitNs, Counter::BackendWaits);
  add("backend thread busy", Counter::BackendNs);
  add("queue submit", Counter::SubmitNs, Counter::Submits);
  add("vertex bounds on CPU", Counter::BoundsNs, Counter::BoundsEvaluations);
  add("EDRAM through CPU", Counter::EdramCpuNs, Counter::EdramCpuTransfers);
  add("EDRAM GPU load record", Counter::EdramGpuLoadNs, Counter::EdramGpuLoads);
  add("EDRAM GPU store", Counter::EdramGpuStoreNs, Counter::EdramGpuStores);
  add("barrier: CP interrupt", Counter::BarrierInterruptNs, Counter::BarrierInterrupts);
  add("barrier: front-buffer flip", Counter::BarrierSwapNs, Counter::BarrierSwaps);
  add("barrier: wait on memory/register", Counter::BarrierWaitNs, Counter::BarrierWaits);
  add("barrier: guest write/instruction store", Counter::BarrierWriteNs, Counter::BarrierWrites);
  add("GPU idle (starved of work)", Counter::GpuIdleNs, Counter::GpuBatches);

  const std::vector<BottleneckRatio> ratios = {
      {"constant slots reused", n(Counter::ConstantReuses), n(Counter::ConstantUploads)},
      {"command-list state sets skipped", n(Counter::BindsSkipped), n(Counter::BindsSet)},
      {"descriptor bindings reused", n(Counter::BindingsReused), n(Counter::BindingsWritten)},
      {"IM_LOAD decoded-program hits", n(Counter::ImLoadHits), n(Counter::ImLoadMisses)},
      {"index conversions reused", n(Counter::IndexCacheHits), n(Counter::IndexCacheMisses)},
  };
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - perf_watch_start_).count();
  const auto text =
      format_bottleneck_report(seconds, n(Counter::Swaps), rank_bottlenecks(items), ratios);
  try {
    const auto directory = logging::probe_log_directory();
    const auto path = directory.empty() ? std::filesystem::path("bottlenecks.log")
                                        : std::filesystem::path(directory) / "bottlenecks.log";
    auto partial = path;
    partial += ".partial";
    {
      std::ofstream out(partial, std::ios::binary | std::ios::trunc);
      out << text;
      if (!out) return;
    }
    std::error_code ec;
    std::filesystem::rename(partial, path, ec);
  } catch (...) {
    // Diagnostics must never take the session down.
  }
}

}  // namespace xenon::core

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "xenon/audio/system.hpp"
#include "xenon/core/session.hpp"
#include "xenon/gpu/perf_stats.hpp"
#include "xenon/core/guest_code_map.hpp"
#include "xenon/kernel/wait_profile.hpp"
#include "xenon/logging/host_sampler.hpp"
#include "xenon/logging/probe_log.hpp"

namespace xenon::core {

void XenonSession::start_guest_profile() {
  stop_guest_profile();
  start_perf_watch();
  // XENON_HOST_PROFILE: the host threads' own sampling profile, beside the
  // guest one (host_profile.log in the probe directory at stop).
  logging::start_host_profile_from_environment();
  // Generated game code has no debug symbols: name it by guest function from
  // the module's own registry, so the host profile says which guest routine
  // the time went to (and which runtime helper it called).
  if (logging::host_sampler_options_from_environment() && compiled_registry_binder_ &&
      memory_ && loaded_xex_) {
    try {
      cpu::CpuState probe_state{};
      cpu::ExecutionContext probe(probe_state, *memory_, *this);
      compiled_registry_binder_(probe);
      std::uint32_t begin = ~0u;
      std::uint32_t end = 0u;
      for (const auto& range : loaded_xex_->executable_ranges) {
        begin = (std::min)(begin, static_cast<std::uint32_t>(range.begin));
        end = (std::max)(end, static_cast<std::uint32_t>(range.end));
      }
      if (probe.compiled_lookup && begin < end) {
        auto map = std::make_shared<const GuestCodeMap>(GuestCodeMap::build(
            [&probe](std::uint32_t guest) {
              return reinterpret_cast<std::uintptr_t>(probe.compiled_lookup(
                  probe.compiled_registry, probe, guest, cpu::CompiledLookupKind::Call));
            },
            begin, end));
        if (!map->empty())
          logging::set_code_symbolizer(
              [map](std::uintptr_t address) { return map->describe(address); });
      }
    } catch (...) {
      // Unnamed game code is still profiled.
    }
  }
  // Wait attribution (XENON_WAIT_PROFILE=1, or with the host profile): every
  // blocking guest wait by thread, object and releasing signal
  // (wait_profile.log in the probe directory at stop).
  {
    const char* wait_setting = std::getenv("XENON_WAIT_PROFILE");
    const char* host_setting = std::getenv("XENON_HOST_PROFILE");
    const auto on = [](const char* value) {
      return value && *value && std::string_view(value) != "0";
    };
    if (on(wait_setting) || on(host_setting)) {
      kernel::reset_wait_profile();
      kernel::set_wait_profile_enabled(true);
    }
    // The per-frame timeline (frame_timeline.log; gpu/perf_stats.hpp).
    if (on(std::getenv("XENON_FRAME_TIMELINE")) || on(host_setting))
      gpu::perf::set_frame_timeline(true);
  }
  const char* setting = std::getenv("XENON_GUEST_PROFILE");
  if (!setting || !*setting || std::string_view(setting) == "0") return;
  guest_profile_path_ = std::string_view(setting) == "1" ? "guest_profile.log" : setting;
  guest_profiler_ = {};
  guest_profile_running_.store(true);
  guest_profile_thread_ = std::thread([this] {
    const auto start = std::chrono::steady_clock::now();
    auto next_snapshot = start + std::chrono::seconds(30);
    std::unordered_map<std::uint32_t, std::string> names;
    // The audio render path's rates every 5 s, in the session log: the game's
    // mixer must deliver 187.5 frames/s (48 kHz / 256) for the device not to
    // run dry. Counters are cumulative; the line shows the window's rates.
    struct AudioWindow {
      std::uint64_t issued{}, submitted{}, consumed{};
      std::uint32_t underruns{};
    } audio_last{};
    auto audio_window_start = start;
    auto next_audio_line = start + std::chrono::seconds(5);
    while (guest_profile_running_.load(std::memory_order_relaxed)) {
      if (const auto now = std::chrono::steady_clock::now(); audio_ && now >= next_audio_line) {
        AudioWindow total{};
        std::size_t queued = 0, credits = 0;
        for (const auto& client : audio_->render_client_stats()) {
          total.issued += client.callbacks_issued;
          total.submitted += client.frames_submitted;
          total.consumed += client.frames_consumed;
          queued += client.queued_frames;
          credits += client.callback_credits;
        }
        total.underruns = audio_->underrun_count();
        const double seconds =
            std::chrono::duration<double>(now - audio_window_start).count();
        if (seconds > 0.0) {
          char line[256];
          std::snprintf(line, sizeof(line),
                        "[audio-rate] %.1fs: callbacks %.1f/s, frames submitted %.1f/s, consumed "
                        "%.1f/s (needed 187.5/s), underruns %u, queued %zu, credits %zu",
                        seconds, double(total.issued - audio_last.issued) / seconds,
                        double(total.submitted - audio_last.submitted) / seconds,
                        double(total.consumed - audio_last.consumed) / seconds,
                        total.underruns - audio_last.underruns, queued, credits);
          std::scoped_lock console_log_lock(console_log_mutex());
          std::cerr << line << std::endl;
        }
        audio_last = total;
        audio_window_start = now;
        next_audio_line = now + std::chrono::seconds(5);
      }
      // Every 30 s the profiles so far are written as they are at stop, so a
      // run that crashes keeps them (both AC6 crash runs lost theirs). Only
      // this thread touches the guest profiler while it runs; the wait
      // profile has its own lock. Each file is replaced whole, never left
      // half written.
      if (const auto now = std::chrono::steady_clock::now(); now >= next_snapshot) {
        next_snapshot = now + std::chrono::seconds(30);
        const auto replace = [](const std::filesystem::path& path, const auto& write) {
          try {
            auto partial = path;
            partial += ".partial";
            {
              std::ofstream out(partial, std::ios::binary | std::ios::trunc);
              write(out);
              if (!out) return;
            }
            std::error_code ec;
            std::filesystem::rename(partial, path, ec);
          } catch (...) {
            // Diagnostics must never take the session down.
          }
        };
        replace(guest_profile_path_, [this](std::ostream& out) { guest_profiler_.write(out); });
        replace(blocked_callers_path(), [this](std::ostream& out) { write_blocked_callers(out); });
        logging::write_host_profile_snapshot();
        if (kernel::wait_profile_enabled()) {
          const auto directory = logging::probe_log_directory();
          replace(directory.empty() ? std::filesystem::path("wait_profile.log")
                                    : std::filesystem::path(directory) / "wait_profile.log",
                  [](std::ostream& out) { out << kernel::wait_profile_report(); });
        }
      }
      const auto elapsed = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - start)
              .count());
      {
        // Racy reads of live register files, as in the stop report: a
        // diagnostic sample, never fed back into execution.
        const auto in_flight = in_flight_exports_.snapshot();
        std::scoped_lock lock(guest_thread_states_mutex_);
        for (const auto& [thread_id, guest_state] : guest_thread_states_) {
          auto [name, inserted] = names.try_emplace(thread_id);
          if (inserted && kernel_process_) {
            if (const auto* thread = kernel_process_->thread_manager().get_thread_ptr(thread_id))
              name->second = thread->name();
          }
          std::string blocked;
          std::uint32_t blocked_lr = 0;
          if (const auto it = in_flight.find(thread_id); it != in_flight.end()) {
            blocked_lr = static_cast<std::uint32_t>(it->second.lr);
            blocked = it->second.descriptor
                          ? it->second.descriptor->library + '!' + it->second.descriptor->name
                          : "export #" + std::to_string(it->second.ordinal);
          }
          guest_profiler_.record(elapsed, thread_id, name->second,
                                 static_cast<cpu::GuestAddress>(guest_state->cia), blocked, blocked_lr);
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
}

// blocked_callers.log: which guest call site each blocked thread sat in a
// kernel export from (the link register at the call), by guest function.
std::filesystem::path XenonSession::blocked_callers_path() const {
  const auto directory = logging::probe_log_directory();
  return directory.empty() ? std::filesystem::path("blocked_callers.log")
                           : std::filesystem::path(directory) / "blocked_callers.log";
}

void XenonSession::write_blocked_callers(std::ostream& out) const {
  guest_profiler_.write_blocked_callers(out, 6, [this](std::uint32_t lr) -> std::string {
    if (!loaded_xex_) return {};
    if (const auto* function = xbox::function_metadata_containing(loaded_xex_->image, lr)) {
      char text[16];
      std::snprintf(text, sizeof(text), "%08X", static_cast<std::uint32_t>(function->begin));
      return text;
    }
    return {};
  });
}

void XenonSession::stop_guest_profile() noexcept {
  stop_perf_watch();
  logging::stop_host_profile_and_report();
  logging::set_code_symbolizer({});
  if (kernel::wait_profile_enabled()) {
    kernel::set_wait_profile_enabled(false);
    try {
      const auto directory = logging::probe_log_directory();
      const auto path = directory.empty() ? std::filesystem::path("wait_profile.log")
                                          : std::filesystem::path(directory) / "wait_profile.log";
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      out << kernel::wait_profile_report();
    } catch (...) {
      // Diagnostics must never take the session down while it stops.
    }
  }
  guest_profile_running_.store(false);
  if (!guest_profile_thread_.joinable()) return;
  guest_profile_thread_.join();
  try {
    std::ofstream out(guest_profile_path_, std::ios::trunc);
    guest_profiler_.write(out);
    std::ofstream callers(blocked_callers_path(), std::ios::trunc);
    write_blocked_callers(callers);
    std::cout << "[XenonSession] guest profile: " << guest_profiler_.samples()
              << " samples written to " << guest_profile_path_ << std::endl;
  } catch (...) {
    // Diagnostics must never take the session down while it stops.
  }
}

}  // namespace xenon::core

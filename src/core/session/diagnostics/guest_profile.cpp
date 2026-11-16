#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "xenon/core/session.hpp"
#include "xenon/gpu/perf_stats.hpp"
#include "xenon/kernel/wait_profile.hpp"
#include "xenon/logging/host_sampler.hpp"
#include "xenon/logging/probe_log.hpp"

namespace xenon::core {

void XenonSession::start_guest_profile() {
  stop_guest_profile();
  // XENON_HOST_PROFILE: the host threads' own sampling profile, beside the
  // guest one (host_profile.log in the probe directory at stop).
  logging::start_host_profile_from_environment();
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
    std::unordered_map<std::uint32_t, std::string> names;
    while (guest_profile_running_.load(std::memory_order_relaxed)) {
      const auto elapsed = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - start)
              .count());
      {
        // Racy reads of live register files, as in the stop report: a
        // diagnostic sample, never fed back into execution.
        std::scoped_lock lock(in_flight_exports_mutex_);
        for (const auto& [thread_id, guest_state] : guest_thread_states_) {
          auto [name, inserted] = names.try_emplace(thread_id);
          if (inserted && kernel_process_) {
            if (const auto* thread = kernel_process_->thread_manager().get_thread_ptr(thread_id))
              name->second = thread->name();
          }
          std::string blocked;
          if (const auto it = in_flight_exports_.find(thread_id); it != in_flight_exports_.end()) {
            blocked = it->second.descriptor
                          ? it->second.descriptor->library + '!' + it->second.descriptor->name
                          : "export #" + std::to_string(it->second.ordinal);
          }
          guest_profiler_.record(elapsed, thread_id, name->second,
                                 static_cast<cpu::GuestAddress>(guest_state->cia), blocked);
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
}

void XenonSession::stop_guest_profile() noexcept {
  logging::stop_host_profile_and_report();
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
    std::cout << "[XenonSession] guest profile: " << guest_profiler_.samples()
              << " samples written to " << guest_profile_path_ << std::endl;
  } catch (...) {
    // Diagnostics must never take the session down while it stops.
  }
}

}  // namespace xenon::core

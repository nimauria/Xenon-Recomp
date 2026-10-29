#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>

#include "xenon/core/session.hpp"

namespace xenon::core {

void XenonSession::start_guest_profile() {
  stop_guest_profile();
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

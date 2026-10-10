#include <chrono>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

#include "xenon/core/session.hpp"

namespace xenon::core {

void XenonSession::sample_memory_watch(const WatchObserver& observer) {
  if (!memory_watch_.active() || !memory_) return;
  auto* address_space = memory_.get();
  const auto added = memory_watch_.sample(
      [address_space](cpu::GuestAddress address) -> std::optional<std::uint32_t> {
        try {
          return address_space->read32_be(address);
        } catch (const memory::MemoryFault&) {
          return std::nullopt;
        }
      },
      observer);
  if (added.empty() || !config_.enable_logging) return;
  // Logged after sample() released the watch lock so console and watch locks are
  // never held together.
  std::scoped_lock console_log_lock(console_log_mutex());
  for (const auto& record : added) {
    std::cout << "[XenonSession] " << GuestMemoryWatch::format(record) << std::endl;
  }
}

std::string XenonSession::describe_running_guest_threads() const {
  std::ostringstream out;
  out << "running=[";
  {
    std::scoped_lock lock(in_flight_exports_mutex_);
    bool first = true;
    for (const auto& [thread_id, guest_state] : guest_thread_states_) {
      if (in_flight_exports_.contains(thread_id)) continue;
      if (!first) out << ' ';
      first = false;
      out << 't' << thread_id << "@0x" << std::hex << std::uppercase << guest_state->cia
          << std::dec;
    }
  }
  out << ']';
  return out.str();
}

std::string XenonSession::describe_recent_exports(std::uint32_t thread_id,
                                                  std::size_t limit) const {
  if (thread_id == 0u || !export_trace_.enabled()) return {};
  const auto recent = export_trace_.recent_for_thread(thread_id, limit);
  std::ostringstream out;
  out << "recent=[";
  for (std::size_t i = 0; i < recent.size(); ++i) {
    if (i != 0u) out << ' ';
    const auto& trace = recent[i];
    if (!trace.name_view().empty()) {
      out << trace.name_view();
    } else {
      out << trace.ordinal;
    }
    out << "(lr=0x" << std::hex << std::uppercase << trace.lr << ",in_r3=0x" << trace.arguments[0]
        << ",in_r4=0x" << trace.arguments[1] << ",out_r3=0x" << trace.result_r3 << std::dec
        << ')';
  }
  out << ']';
  return out.str();
}

void XenonSession::start_memory_watch_poll() {
  stop_memory_watch_poll();
  if (!memory_watch_.active()) return;
  WatchObserver baseline{};
  baseline.phase = WatchPhase::Poll;
  baseline.note = "baseline";
  sample_memory_watch(baseline);
  if (config_.memory_watch_poll_ms == 0u) return;

  memory_watch_poll_running_.store(true);
  const auto period = std::chrono::milliseconds(config_.memory_watch_poll_ms);
  memory_watch_thread_ = std::thread([this, period] {
    while (memory_watch_poll_running_.load(std::memory_order_relaxed)) {
      WatchObserver observer{};
      observer.phase = WatchPhase::Poll;
      observer.describe = [this] { return describe_running_guest_threads(); };
      sample_memory_watch(observer);
      std::this_thread::sleep_for(period);
    }
  });
}

void XenonSession::stop_memory_watch_poll() noexcept {
  memory_watch_poll_running_.store(false);
  if (memory_watch_thread_.joinable()) memory_watch_thread_.join();
}

}  // namespace xenon::core

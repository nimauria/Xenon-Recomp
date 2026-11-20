#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

#include "xenon/cpu/types.hpp"

namespace xenon::core {

struct ExportTraceRecord {
  static constexpr std::size_t kLibraryCapacity = 32;
  static constexpr std::size_t kNameCapacity = 96;

  std::uint64_t sequence{};
  std::uint32_t thread_id{};
  std::array<char, kLibraryCapacity> library{};
  std::array<char, kNameCapacity> name{};
  std::uint32_t ordinal{};
  cpu::GuestAddress call_address{};
  std::uint64_t lr{};
  std::uint64_t ctr{};
  std::array<std::uint64_t, 8> arguments{};
  std::uint64_t result_r3{};
  bool handler_found{};
  bool handled{};
  bool success{};
  // When the call began (steady-clock nanoseconds) and how long it took; 0
  // when the caller did not time it.
  std::uint64_t start_ns{};
  std::uint64_t duration_ns{};

  [[nodiscard]] std::string_view library_view() const noexcept;
  [[nodiscard]] std::string_view name_view() const noexcept;
};

// A fixed-capacity, allocation-free-on-record diagnostics ring of completed
// kernel calls. It is on in every session (crash and stall reports read it),
// so recording must cost a game nothing measurable: each host thread writes
// its own lane without any lock or shared counter (AC6's audio mixer makes
// tens of thousands of IRQL calls a second, and one global mutex per call
// made a quarter of its time call overhead). A slot's sequence counter lets a
// reader skip a record being written. Snapshots merge the lanes by time and
// number the records oldest-first; they allocate, and run only for reports.
class ExportTrace {
 public:
  static constexpr std::size_t kDefaultCapacity = 512;
  // Records kept per host thread (at most the trace's capacity), and how
  // many threads get a lane of their own; any further thread writes a shared
  // lane under a lock.
  static constexpr std::size_t kMaxLaneCapacity = 512;
  static constexpr std::size_t kMaxLanes = 256;

  explicit ExportTrace(std::size_t capacity = kDefaultCapacity);
  ~ExportTrace();
  ExportTrace(const ExportTrace&) = delete;
  ExportTrace& operator=(const ExportTrace&) = delete;

  void set_enabled(bool enabled) noexcept;
  [[nodiscard]] bool enabled() const noexcept;
  [[nodiscard]] std::size_t capacity() const noexcept;

  void record(std::uint32_t thread_id, std::string_view library,
              std::string_view name, std::uint32_t ordinal,
              cpu::GuestAddress call_address, std::uint64_t lr,
              std::uint64_t ctr, std::span<const std::uint64_t, 8> arguments,
              std::uint64_t result_r3, bool handler_found, bool handled,
              bool success, std::uint64_t start_ns = 0, std::uint64_t duration_ns = 0);

  // Returned records are oldest-to-newest within the requested suffix.
  [[nodiscard]] std::vector<ExportTraceRecord> recent_global(
      std::size_t limit) const;
  [[nodiscard]] std::vector<ExportTraceRecord> recent_for_thread(
      std::uint32_t thread_id, std::size_t limit) const;
  void clear();

 private:
  struct Lane;
  template <std::size_t N>
  static void copy_text(std::array<char, N>& destination,
                        std::string_view source) noexcept;
  [[nodiscard]] Lane& lane_for_this_thread();
  // Every lane's records, oldest first, numbered from 1, at most capacity().
  [[nodiscard]] std::vector<ExportTraceRecord> snapshot() const;

  const std::uint64_t instance_id_;
  const std::size_t capacity_;
  const std::size_t lane_capacity_;
  std::atomic<bool> enabled_{false};
  mutable std::mutex lanes_mutex_{};
  std::vector<std::unique_ptr<Lane>> lanes_{};
  std::unique_ptr<Lane> shared_lane_{};  // threads past kMaxLanes, under lanes_mutex_
};

}  // namespace xenon::core

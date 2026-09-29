#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
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

  [[nodiscard]] std::string_view library_view() const noexcept;
  [[nodiscard]] std::string_view name_view() const noexcept;
};

// A fixed-capacity, allocation-free-on-record diagnostics ring. Snapshotting
// allocates by design, but is only performed for tests or after a fatal fault.
class ExportTrace {
 public:
  static constexpr std::size_t kDefaultCapacity = 512;

  explicit ExportTrace(std::size_t capacity = kDefaultCapacity);

  void set_enabled(bool enabled) noexcept;
  [[nodiscard]] bool enabled() const noexcept;
  [[nodiscard]] std::size_t capacity() const noexcept;

  void record(std::uint32_t thread_id, std::string_view library,
              std::string_view name, std::uint32_t ordinal,
              cpu::GuestAddress call_address, std::uint64_t lr,
              std::uint64_t ctr, std::span<const std::uint64_t, 8> arguments,
              std::uint64_t result_r3, bool handler_found, bool handled,
              bool success);

  // Returned records are oldest-to-newest within the requested suffix.
  [[nodiscard]] std::vector<ExportTraceRecord> recent_global(
      std::size_t limit) const;
  [[nodiscard]] std::vector<ExportTraceRecord> recent_for_thread(
      std::uint32_t thread_id, std::size_t limit) const;
  void clear();

 private:
  template <std::size_t N>
  static void copy_text(std::array<char, N>& destination,
                        std::string_view source) noexcept;
  [[nodiscard]] std::vector<ExportTraceRecord> snapshot_locked() const;

  std::atomic<bool> enabled_{false};
  mutable std::mutex mutex_{};
  std::vector<ExportTraceRecord> records_{};
  std::uint64_t next_sequence_{1};
  std::size_t next_index_{};
  std::size_t size_{};
};

}  // namespace xenon::core

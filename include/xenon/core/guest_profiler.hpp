#pragma once

#include <cstdint>
#include <map>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>

namespace xenon::core {

// Sampling profile of where guest threads spend wall time: each sample is a
// thread's current guest instruction, or the kernel export it is blocked in.
// Samples are bucketed per second since the profile started, so a timeline
// shows what a stage of a title waits on (loading, a kernel wait, a hot loop).
class GuestProfiler {
 public:
  // `export_name` empty: the thread was running guest code at `cia`.
  void record(std::uint64_t elapsed_ms, std::uint32_t thread_id, std::string_view thread_name,
              std::uint32_t cia, std::string_view export_name);

  // Per second and thread, the `top` most frequent locations with their share.
  void write(std::ostream& out, std::size_t top = 8) const;

  [[nodiscard]] std::uint64_t samples() const noexcept { return samples_; }

 private:
  struct Bucket {
    std::string thread_name;
    std::uint64_t total{};
    std::unordered_map<std::string, std::uint64_t> locations;
  };
  // (second, thread id) -> samples.
  std::map<std::pair<std::uint64_t, std::uint32_t>, Bucket> buckets_;
  std::uint64_t samples_{};
};

}  // namespace xenon::core

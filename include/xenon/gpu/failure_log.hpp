#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace xenon::gpu {

// One reason draws or resolves failed for, and how many times it happened.
struct GpuFailureReason {
  std::string message{};
  bool resolve{};
  std::uint64_t count{};
};

// Every distinct draw/resolve failure message, in first-seen order. A backend's
// error string holds only the latest failure and is replaced by later status,
// so a run that failed thousands of draws used to end with "last error: none".
// Thread-safe: the GPU thread records while a stop report reads. Bounded: once
// kMaxReasons distinct messages are kept, further new ones are only counted.
class GpuFailureLog {
 public:
  static constexpr std::size_t kMaxReasons = 32;

  // Returns how many times this reason has now happened (1 the first time),
  // or 0 when it is not kept because kMaxReasons was already reached.
  std::uint64_t record(std::string_view message, bool resolve);
  [[nodiscard]] std::vector<GpuFailureReason> reasons() const;
  // Failures whose message was not kept because kMaxReasons was reached.
  [[nodiscard]] std::uint64_t unlisted() const;

 private:
  mutable std::mutex mutex_{};
  std::vector<GpuFailureReason> reasons_{};
  std::uint64_t unlisted_{};
};

}  // namespace xenon::gpu

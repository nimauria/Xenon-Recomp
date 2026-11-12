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

// One kind of resolve - depth or color, the source render-target format, the
// destination texture format and the rectangle size - and how many of them
// wrote only zeros.
struct GpuResolveKind {
  bool depth{};
  std::uint8_t source_format{};
  std::uint8_t destination_format{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint64_t count{};
  std::uint64_t all_zero{};
  // Guest physical address the latest resolve of this kind wrote.
  std::uint32_t last_destination{};
  // Resolves whose data sat in one byte lane of each 4-byte texel only (an
  // alpha-only image is black), among those small enough to be scanned.
  std::uint64_t single_lane{};
};

// A pass that resolves to black succeeds like any other resolve, so a frame
// that comes out black with no failed draws gives no hint which pass went
// dark. This keeps every kind of resolve in first-seen order with its
// all-zero count, so the stop report shows where the image was lost.
// Thread-safe and bounded like GpuFailureLog.
class GpuResolveCensus {
 public:
  static constexpr std::size_t kMaxKinds = 64;

  // Counts one resolve. Returns true when this is the first time its kind
  // wrote only zeros.
  bool record(bool depth, std::uint8_t source_format, std::uint8_t destination_format,
              std::uint32_t width, std::uint32_t height, bool all_zero,
              std::uint32_t destination = 0u, bool single_lane = false);
  [[nodiscard]] std::vector<GpuResolveKind> kinds() const;
  // Resolves whose kind was not kept because kMaxKinds was reached.
  [[nodiscard]] std::uint64_t unlisted() const;

 private:
  mutable std::mutex mutex_{};
  std::vector<GpuResolveKind> kinds_{};
  std::uint64_t unlisted_{};
};

}  // namespace xenon::gpu

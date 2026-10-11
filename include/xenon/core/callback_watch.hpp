#pragma once

// Which guest callback a pump thread (GPU interrupts, audio render) is
// running, for how long, and how long its callbacks take. Always on: a pump
// that stops because a guest callback never returns must be visible in the
// stop report without any switch. On hardware an interrupt callback runs at
// raised IRQL and must not block, so one that runs for longer than a frame
// is itself evidence.
//
// One writer (the pump thread); any thread may read a snapshot. Reads are
// racy by design: a snapshot taken while a callback starts or ends may mix
// the two.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

namespace xenon::core {

class CallbackWatch {
 public:
  // Longer than one 60 Hz frame.
  static constexpr std::int64_t kSlowCallbackNs = 16'666'667;

  struct Snapshot {
    bool in_callback{};
    std::uint32_t entry{};
    const char* kind{};
    std::int64_t since_ns{};
    std::uint64_t calls{};
    std::uint64_t slow_calls{};
    std::int64_t longest_ns{};
    std::uint32_t longest_entry{};
  };

  static std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  // `kind` must have static storage duration ("vsync interrupt", ...).
  void begin(std::uint32_t entry, const char* kind, std::int64_t now) noexcept {
    entry_.store(entry, std::memory_order_relaxed);
    kind_.store(kind, std::memory_order_relaxed);
    since_ns_.store(now, std::memory_order_relaxed);
    in_callback_.store(true, std::memory_order_release);
  }

  // Returns the callback's duration.
  std::int64_t end(std::int64_t now) noexcept {
    const auto duration = now - since_ns_.load(std::memory_order_relaxed);
    in_callback_.store(false, std::memory_order_release);
    calls_.fetch_add(1, std::memory_order_relaxed);
    if (duration > kSlowCallbackNs) slow_calls_.fetch_add(1, std::memory_order_relaxed);
    if (duration > longest_ns_.load(std::memory_order_relaxed)) {
      longest_ns_.store(duration, std::memory_order_relaxed);
      longest_entry_.store(entry_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    return duration;
  }

  // One line for stop reports, such as "GPU pump: inside vsync interrupt
  // callback 0x82123456 for 5234 ms; 1200 callbacks, 3 longer than a frame,
  // longest 40.1 ms (0x82123456)".
  [[nodiscard]] static std::string describe(const char* pump, const Snapshot& s,
                                            std::int64_t now) {
    char line[256];
    std::string text = pump;
    if (s.in_callback) {
      std::snprintf(line, sizeof(line), ": inside %s callback 0x%08X for %lld ms",
                    s.kind ? s.kind : "guest", s.entry,
                    static_cast<long long>((now - s.since_ns) / 1'000'000));
    } else {
      std::snprintf(line, sizeof(line), ": idle");
    }
    text += line;
    std::snprintf(line, sizeof(line),
                  "; %llu callbacks, %llu longer than a frame, longest %.1f ms (0x%08X)",
                  static_cast<unsigned long long>(s.calls),
                  static_cast<unsigned long long>(s.slow_calls),
                  static_cast<double>(s.longest_ns) / 1e6, s.longest_entry);
    text += line;
    return text;
  }

  [[nodiscard]] Snapshot snapshot() const noexcept {
    Snapshot s{};
    s.in_callback = in_callback_.load(std::memory_order_acquire);
    s.entry = entry_.load(std::memory_order_relaxed);
    s.kind = kind_.load(std::memory_order_relaxed);
    s.since_ns = since_ns_.load(std::memory_order_relaxed);
    s.calls = calls_.load(std::memory_order_relaxed);
    s.slow_calls = slow_calls_.load(std::memory_order_relaxed);
    s.longest_ns = longest_ns_.load(std::memory_order_relaxed);
    s.longest_entry = longest_entry_.load(std::memory_order_relaxed);
    return s;
  }

 private:
  std::atomic<bool> in_callback_{false};
  std::atomic<std::uint32_t> entry_{0};
  std::atomic<const char*> kind_{nullptr};
  std::atomic<std::int64_t> since_ns_{0};
  std::atomic<std::uint64_t> calls_{0};
  std::atomic<std::uint64_t> slow_calls_{0};
  std::atomic<std::int64_t> longest_ns_{0};
  std::atomic<std::uint32_t> longest_entry_{0};
};

}  // namespace xenon::core

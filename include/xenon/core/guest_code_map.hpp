#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace xenon::core {

// Which guest function a host code address belongs to, for profiles of
// generated game code. A game module ships no debug symbols, so a host
// sampler would otherwise credit every sample to the module's nearest export.
//
// The map is built from the module's own compiled registry: every guest
// entry it resolves names the host function that runs it. The registered
// function is often only a wrapper that loads its guest entry and calls or
// jumps to a separate dispatch body (x86-64: `mov edx/r8d, <guest entry>`,
// then `call`/`jmp <body>`); the body is credited to that entry too.
class GuestCodeMap {
 public:
  struct Entry {
    std::uintptr_t host{};
    std::uint32_t guest{};
  };

  // `lookup(guest)` is the registry's host function for a guest entry, or 0.
  // Every guest word in [begin, end) is tried.
  [[nodiscard]] static GuestCodeMap build(
      const std::function<std::uintptr_t(std::uint32_t)>& lookup, std::uint32_t begin,
      std::uint32_t end);
  // From known pairs (tests, or a caller that already has the table).
  [[nodiscard]] static GuestCodeMap from_entries(std::vector<Entry> entries);

  // The guest function whose host code contains `address`: the entry whose
  // host function starts at the enclosing function's start (the host's
  // unwind table says where that is, on Windows), else the nearest entry at
  // or below the address. Nothing for an address outside every entry's span.
  [[nodiscard]] std::optional<std::uint32_t> guest_for(std::uintptr_t address) const;
  // "guest 823A73F8" for code the map covers; a fixed label for module code
  // between entries that belongs to no guest function; nothing outside.
  [[nodiscard]] std::optional<std::string> describe(std::uintptr_t address) const;

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

  // The body a wrapper at `host` calls or jumps to after loading `guest` as
  // an immediate (x86-64 encodings above), else nothing. Exposed for tests;
  // reads 40 code bytes at `host`.
  [[nodiscard]] static std::optional<std::uintptr_t> trampoline_target(std::uintptr_t host,
                                                                       std::uint32_t guest);

 private:
  std::vector<Entry> entries_{};  // sorted by host address, one per host address
  std::uintptr_t lowest_{};
  std::uintptr_t highest_{};  // the last entry's start plus kTailSpan
};

}  // namespace xenon::core

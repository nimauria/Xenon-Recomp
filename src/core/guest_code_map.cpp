#include "xenon/core/guest_code_map.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace xenon::core {
namespace {

// How far past the last entry's start an address may still belong to it.
constexpr std::uintptr_t kTailSpan = 1u << 20;

}  // namespace

std::optional<std::uintptr_t> GuestCodeMap::trampoline_target(std::uintptr_t host,
                                                              std::uint32_t guest) {
#if defined(_M_X64) || defined(__x86_64__)
  // The wrapper loads its own guest entry as a 32-bit immediate (into edx,
  // or r8d when the result is returned through a hidden pointer) and then
  // calls or jumps to the body: `mov edx, guest; jmp body`, or `push rbx;
  // sub rsp, 32; mov r8d, guest; mov rbx, rcx; call body; ...`. The
  // immediate must be the entry's own address, which an unrelated function
  // starting with a call would not carry.
  constexpr std::size_t kScan = 40;
  unsigned char code[kScan];
  std::memcpy(code, reinterpret_cast<const void*>(host), sizeof(code));
  for (std::size_t i = 1; i + 4u <= 24u; ++i) {
    const bool mov_edx = code[i - 1] == 0xBAu;
    const bool mov_r8d = i >= 2u && code[i - 2] == 0x41u && code[i - 1] == 0xB8u;
    if (!mov_edx && !mov_r8d) continue;
    std::uint32_t immediate = 0;
    std::memcpy(&immediate, code + i, 4);
    if (immediate != guest) continue;
    for (std::size_t j = i + 4u; j + 5u <= kScan && j < i + 4u + 12u; ++j) {
      if (code[j] != 0xE8u && code[j] != 0xE9u) continue;
      std::int32_t displacement = 0;
      std::memcpy(&displacement, code + j + 1, 4);
      return host + j + 5u + static_cast<std::intptr_t>(displacement);
    }
    return std::nullopt;
  }
  return std::nullopt;
#else
  (void)host;
  (void)guest;
  return std::nullopt;
#endif
}

GuestCodeMap GuestCodeMap::build(const std::function<std::uintptr_t(std::uint32_t)>& lookup,
                                 std::uint32_t begin, std::uint32_t end) {
  std::vector<Entry> direct;
  std::vector<Entry> bodies;
  for (std::uint64_t guest = begin & ~3u; guest + 4u <= end; guest += 4u) {
    const auto host = lookup(static_cast<std::uint32_t>(guest));
    if (!host) continue;
    direct.push_back({host, static_cast<std::uint32_t>(guest)});
    if (const auto body = trampoline_target(host, static_cast<std::uint32_t>(guest)))
      bodies.push_back({*body, static_cast<std::uint32_t>(guest)});
  }
  // A body reached only through a trampoline is credited to its entry; an
  // address that is itself registered keeps its own entry.
  direct.insert(direct.end(), bodies.begin(), bodies.end());
  return from_entries(std::move(direct));
}

GuestCodeMap GuestCodeMap::from_entries(std::vector<Entry> entries) {
  GuestCodeMap map;
  // Stable: for one host address the first pair wins (registered entries
  // were placed before trampoline bodies).
  std::stable_sort(entries.begin(), entries.end(),
                   [](const Entry& a, const Entry& b) { return a.host < b.host; });
  entries.erase(std::unique(entries.begin(), entries.end(),
                            [](const Entry& a, const Entry& b) { return a.host == b.host; }),
                entries.end());
  map.entries_ = std::move(entries);
  if (!map.entries_.empty()) {
    map.lowest_ = map.entries_.front().host;
    map.highest_ = map.entries_.back().host + kTailSpan;
  }
  return map;
}

std::optional<std::uint32_t> GuestCodeMap::guest_for(std::uintptr_t address) const {
  if (entries_.empty() || address < lowest_ || address >= highest_) return std::nullopt;
  const auto at_or_below = [&](std::uintptr_t value) -> const Entry* {
    auto it = std::upper_bound(entries_.begin(), entries_.end(), value,
                               [](std::uintptr_t v, const Entry& e) { return v < e.host; });
    if (it == entries_.begin()) return nullptr;
    return &*std::prev(it);
  };
#if defined(_WIN32) && defined(_M_X64)
  // The unwind table knows the enclosing function's start exactly; a leaf
  // function has no entry, and then the nearest start below stands.
  DWORD64 image_base = 0;
  if (const auto* function =
          RtlLookupFunctionEntry(static_cast<DWORD64>(address), &image_base, nullptr)) {
    const auto start = static_cast<std::uintptr_t>(image_base + function->BeginAddress);
    const auto* entry = at_or_below(start);
    if (entry && entry->host == start) return entry->guest;
    // A host function the registry does not name (a runtime helper the
    // module carries): not guest code.
    if (entry) return std::nullopt;
  }
#endif
  if (const auto* entry = at_or_below(address)) return entry->guest;
  return std::nullopt;
}

std::optional<std::string> GuestCodeMap::describe(std::uintptr_t address) const {
  if (entries_.empty() || address < lowest_ || address >= highest_) return std::nullopt;
  const auto guest = guest_for(address);
  // Inside the generated code but in a function no guest entry names (a
  // runtime helper the module carries): said so, rather than credited to
  // whatever export symbol happens to lie below it.
  if (!guest) return std::string("game module code outside any guest function");
  char text[32];
  std::snprintf(text, sizeof(text), "guest %08X", static_cast<unsigned>(*guest));
  return std::string(text);
}

}  // namespace xenon::core

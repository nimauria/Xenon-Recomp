// GuestCodeMap: host code addresses in a game module resolve to the guest
// function the registry runs there, including a body reached through a
// `mov edx, guest; jmp body` trampoline; code between entries and code
// outside the module are told apart.

#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "xenon/core/guest_code_map.hpp"

using xenon::core::GuestCodeMap;

namespace {

// Heap memory: no image's unwind table covers it, so lookups use the
// nearest-entry rule on every platform.
struct FakeModule {
  std::vector<unsigned char> bytes = std::vector<unsigned char>(0x4000, 0xCC);
  [[nodiscard]] std::uintptr_t at(std::size_t offset) const {
    return reinterpret_cast<std::uintptr_t>(bytes.data()) + offset;
  }
  void trampoline(std::size_t offset, std::uint32_t guest, std::size_t body) {
    auto* code = bytes.data() + offset;
    code[0] = 0xBA;
    std::memcpy(code + 1, &guest, 4);
    code[5] = 0xE9;
    const auto displacement = static_cast<std::int32_t>(
        static_cast<std::intptr_t>(body) - static_cast<std::intptr_t>(offset + 10));
    std::memcpy(code + 6, &displacement, 4);
  }
};

// What MSVC emits for a wrapper whose result goes through a hidden pointer:
// push rbx; sub rsp, 32; mov r8d, guest; mov rbx, rcx; call body; ...
void call_wrapper(FakeModule& module, std::size_t offset, std::uint32_t guest, std::size_t body) {
  const unsigned char prefix[] = {0x53, 0x48, 0x83, 0xEC, 0x20, 0x41, 0xB8};
  auto* code = module.bytes.data() + offset;
  std::memcpy(code, prefix, sizeof(prefix));
  std::memcpy(code + 7, &guest, 4);
  const unsigned char mov_rbx_rcx[] = {0x48, 0x8B, 0xD9};
  std::memcpy(code + 11, mov_rbx_rcx, 3);
  code[14] = 0xE8;
  const auto displacement = static_cast<std::int32_t>(static_cast<std::intptr_t>(body) -
                                                      static_cast<std::intptr_t>(offset + 19));
  std::memcpy(code + 15, &displacement, 4);
}

void test_trampoline_decoding() {
  FakeModule module;
  module.trampoline(0x100, 0x823A73F8u, 0x2000);
  call_wrapper(module, 0x300, 0x823A7400u, 0x2800);
  const auto target = GuestCodeMap::trampoline_target(module.at(0x100), 0x823A73F8u);
#if defined(_M_X64) || defined(__x86_64__)
  assert(target && *target == module.at(0x2000));
  const auto called = GuestCodeMap::trampoline_target(module.at(0x300), 0x823A7400u);
  assert(called && *called == module.at(0x2800));
  assert(!GuestCodeMap::trampoline_target(module.at(0x300), 0x823A73F8u));
  // The immediate must be the entry's own guest address.
  assert(!GuestCodeMap::trampoline_target(module.at(0x100), 0x823A7400u));
  // Anything else is a real body, not a trampoline.
  assert(!GuestCodeMap::trampoline_target(module.at(0x200), 0x823A73F8u));
#else
  assert(!target);
#endif
}

void test_build_and_resolve() {
  FakeModule module;
  // Guest 0x82000000: body at 0x0000. Guest 0x82000010: trampoline at 0x1000
  // to a body at 0x3000. Guest 0x82000020: body at 0x1800.
  module.trampoline(0x1000, 0x82000010u, 0x3000);
  const std::map<std::uint32_t, std::size_t> registry{
      {0x82000000u, 0x0000}, {0x82000010u, 0x1000}, {0x82000020u, 0x1800}};
  const auto map = GuestCodeMap::build(
      [&](std::uint32_t guest) -> std::uintptr_t {
        const auto it = registry.find(guest);
        return it == registry.end() ? 0u : module.at(it->second);
      },
      0x82000000u, 0x82000100u);
  assert(map.size() == 4u);  // three entries and one trampoline body

  assert(map.guest_for(module.at(0x0040)) == 0x82000000u);
  assert(map.guest_for(module.at(0x1004)) == 0x82000010u);
  assert(map.guest_for(module.at(0x1900)) == 0x82000020u);
  // The trampoline's body is credited to the trampoline's guest entry.
  assert(map.guest_for(module.at(0x3000)) == 0x82000010u);
  assert(map.guest_for(module.at(0x3abc)) == 0x82000010u);

  assert(map.describe(module.at(0x3abc)) == std::string("guest 82000010"));
  // Below the first entry: not game code.
  assert(!map.describe(module.at(0) - 1u));
}

void test_from_entries_prefers_the_first_pair_per_host_address() {
  const auto map = GuestCodeMap::from_entries(
      {{0x5000, 0x82000100u}, {0x1000, 0x82000000u}, {0x5000, 0x82000200u}});
  assert(map.size() == 2u);
  assert(map.guest_for(0x1000) == 0x82000000u);
  assert(map.guest_for(0x5004) == 0x82000100u);
  assert(!map.guest_for(0x0FFF));
  assert(GuestCodeMap{}.describe(0x1000) == std::nullopt);
}

}  // namespace

int main() {
  test_trampoline_decoding();
  test_build_and_resolve();
  test_from_entries_prefers_the_first_pair_per_host_address();
  return 0;
}

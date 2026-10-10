// Opt-in guest write trap (xenon/cpu/memory_port.hpp: set_write_traps(),
// XENON_WRITE_TRAP). A trapped 32-bit store is reported through the probe log
// from both store paths; other stores and other widths are not, and nothing
// is reported while no trap is set.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "xenon/cpu/memory_port.hpp"
#include "xenon/logging/probe_log.hpp"
#include "xenon/memory/address_space.hpp"

namespace {

constexpr const char* kTrapLog = "signal_write_trap_diag.log";

std::string read_trap_log() {
  std::ifstream in(kTrapLog, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

int main() {
  std::cout << "Testing the guest write trap...\n";
  const auto directory = std::filesystem::temp_directory_path() / "xenon_write_trap_tests";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  std::filesystem::current_path(directory);

  xenon::memory::AddressSpace memory;
  assert(memory.initialize());
  xenon::memory::GuestAddress base{};
  assert(memory.allocate(0x1000, 0x1000, xenon::memory::kReadWrite, false, base));
  const auto trapped = base + 0x40u;
  const auto neighbour = base + 0x44u;

  xenon::logging::set_probe_logs_enabled(true);

  // No trap set: nothing is reported.
  memory.write32_be(trapped, 0x11111111u);
  assert(!std::filesystem::exists(kTrapLog));

  const std::vector<xenon::cpu::GuestAddress> traps = {trapped};
  xenon::cpu::set_write_traps(traps);
  assert(xenon::cpu::is_write_trapped(trapped) && !xenon::cpu::is_write_trapped(neighbour));

  memory.write32_be(neighbour, 0x22222222u);
  memory.write16_be(trapped, 0x3333u);  // only 32-bit stores are trapped
  assert(!std::filesystem::exists(kTrapLog));

  memory.write32_be(trapped, 0x44444444u);  // AddressSpace store path
  {
    auto access = memory.access_context();  // generated-code store path
    access.write32_be(trapped, 0x55555555u);
  }
  const auto log = read_trap_log();
  char expected[64];
  std::snprintf(expected, sizeof(expected), "WRITE to guest 0x%08X value=0x44444444",
                static_cast<unsigned>(trapped));
  assert(log.find(expected) != std::string::npos);
  std::snprintf(expected, sizeof(expected), "WRITE to guest 0x%08X value=0x55555555",
                static_cast<unsigned>(trapped));
  assert(log.find(expected) != std::string::npos);
  std::snprintf(expected, sizeof(expected), "0x%08X", static_cast<unsigned>(neighbour));
  assert(log.find(expected) == std::string::npos && "an untrapped store is not reported");
  assert(memory.read32_be(trapped) == 0x55555555u && "the trap does not change the store");

  // Clearing the traps stops reporting.
  xenon::cpu::set_write_traps({});
  const auto size_before = std::filesystem::file_size(kTrapLog);
  memory.write32_be(trapped, 0x66666666u);
  assert(std::filesystem::file_size(kTrapLog) == size_before);

  xenon::logging::set_probe_logs_enabled(false);
  std::filesystem::current_path(std::filesystem::temp_directory_path());
  std::filesystem::remove_all(directory);
  std::cout << "All write trap tests passed!\n";
  return 0;
}

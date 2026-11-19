// Every failed guest virtual allocation is written to the run's memory.log,
// with the request and the reason, while successful ones write nothing.

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "xenon/logging/logger.hpp"
#include "xenon/memory/address_space.hpp"

namespace memory = xenon::memory;
using xenon::logging::Logger;

namespace {

std::vector<std::string> read_lines(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::vector<std::string> lines;
  for (std::string line; std::getline(in, line);) lines.push_back(line);
  return lines;
}

bool contains(const std::string& line, const std::string& text) {
  return line.find(text) != std::string::npos;
}

}  // namespace

int main() {
  const auto directory = std::filesystem::temp_directory_path() / "xenon_allocation_failure_log";
  std::filesystem::remove_all(directory);
  auto& logger = Logger::instance();
  logger.set_sink([](xenon::logging::Level, std::string_view, std::string_view) {});
  assert(logger.set_error_directory(directory.string()));

  memory::AddressSpace mem(memory::GuestTranslationMode::Compact);
  memory::GuestAddress address = 0;
  // Before initialization nothing can be allocated.
  assert(!mem.allocate(0x1000u, 0, memory::kReadWrite, false, address));
  assert(read_lines(directory / "memory.log").size() == 1u);
  assert(mem.initialize());

  // A successful allocation adds nothing.
  assert(mem.allocate(0x1000u, 0, memory::kReadWrite, false, address));
  assert(read_lines(directory / "memory.log").size() == 1u);

  assert(!mem.allocate(0u, 0, memory::kReadWrite, false, address));
  assert(!mem.allocate(0x1000u, 0, memory::kReadWrite, false, address, 0x3000u));
  assert(!mem.allocate(0x1000u, 0x3000u, memory::kReadWrite, true, address));
  assert(!mem.allocate(0xFFFF0000u, 0, memory::kReadWrite, false, address));

  const auto lines = read_lines(directory / "memory.log");
  for (const auto& line : lines) std::cout << line << "\n";
  assert(lines.size() == 5u);
  assert(contains(lines[0], " warning: guest allocation of 4096 bytes") &&
         contains(lines[0], "failed: the address space is not initialized"));
  assert(contains(lines[1], "guest allocation of 0 bytes") &&
         contains(lines[1], "failed: the size is zero"));
  assert(contains(lines[2], "page size 12288") &&
         contains(lines[2], "failed: no virtual region uses that page size"));
  assert(contains(lines[3], "alignment 0x3000") && contains(lines[3], "top-down") &&
         contains(lines[3], "failed: the alignment is not a power of two"));
  assert(contains(lines[4], "guest allocation of 4294901760 bytes") &&
         contains(lines[4], "bottom-up, commit") &&
         contains(lines[4], "failed: no free range is large enough"));

  logger.set_error_directory("");
  logger.set_sink({});
  std::filesystem::remove_all(directory);
  std::cout << "allocation failure log tests passed" << std::endl;
  return 0;
}

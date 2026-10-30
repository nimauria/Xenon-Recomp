// The runtime host reserves a large host stack for every thread it creates
// without an explicit size (each guest thread's host thread among them); see
// XENON_RUNTIME_HOST_STACK_RESERVE in runtime_host/CMakeLists.txt. Recompiled
// code needs several times the guest's stack, and Ace Combat 6 overflowed the
// 1 MiB default starting its campaign opening. This reads the reservation back
// out of the built executable's PE header so the setting cannot quietly go.

#include <cassert>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

template <typename T>
T read_le(const std::vector<char>& bytes, std::size_t offset) {
  assert(offset + sizeof(T) <= bytes.size());
  T value{};
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    value |= static_cast<T>(static_cast<unsigned char>(bytes[offset + i])) << (8u * i);
  }
  return value;
}

}  // namespace

int main(int argc, char** argv) {
  assert(argc == 3 && "usage: host_stack_tests <xenon_runtime_host.exe> <expected reserve>");
  std::ifstream in(argv[1], std::ios::binary);
  assert(in && "the runtime host executable must exist");
  const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
  const auto expected = std::stoull(argv[2]);

  assert(bytes.size() > 0x40u && bytes[0] == 'M' && bytes[1] == 'Z');
  const auto pe = read_le<std::uint32_t>(bytes, 0x3Cu);
  assert(read_le<std::uint32_t>(bytes, pe) == 0x00004550u);  // "PE\0\0"
  const auto optional_header = pe + 24u;
  assert(read_le<std::uint16_t>(bytes, optional_header) == 0x20Bu);  // PE32+
  // PE32+ optional header: SizeOfStackReserve at 72, SizeOfStackCommit at 80.
  const auto reserve = read_le<std::uint64_t>(bytes, optional_header + 72u);
  const auto commit = read_le<std::uint64_t>(bytes, optional_header + 80u);
  std::cout << "xenon_runtime_host stack: reserve " << reserve << ", commit " << commit << "\n";
  assert(reserve == expected);
  assert(commit < reserve && "only address space is reserved up front");
  std::cout << "runtime host stack tests passed" << std::endl;
  return 0;
}

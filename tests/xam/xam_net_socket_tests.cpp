// Regression coverage for the NetDll_* socket family (xam_net_exports.cpp).
//
// Real AC6 repro: the title calls NetDll_socket (xam.xex ordinal 3) directly
// during boot; before these exports existed, XenonSession::call() had no
// handler for it, so the guest dispatch loop reached the "Unresolved import
// call" path and turned it into a fatal Trap (STATUS_PROCEDURE_NOT_FOUND),
// crashing the whole session. These tests exercise the real socket lifecycle
// (create, configure, bind, exchange datagrams, close) end-to-end through a
// real host UDP loopback socket - not just registration - and the
// not-a-socket error contract every handler shares.

#include <cassert>
#include <cstring>
#include <iostream>

#include "xenon/core/export_registry.hpp"
#include "xenon/core/session.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xam/xam_exports.hpp"
#include "xenon/xam/xam_session.hpp"

using namespace xenon;

namespace {

core::ExportCallContext make_context(cpu::CpuState& cpu, memory::AddressSpace& memory,
                                     std::uint32_t thread_id) {
  return core::ExportCallContext{cpu, memory, 0, thread_id};
}

std::uint32_t invoke_socket(core::ExportRegistry& registry, memory::AddressSpace& memory,
                            std::uint32_t thread_id, std::uint32_t af, std::uint32_t type,
                            std::uint32_t protocol) {
  cpu::CpuState cpu{};
  cpu.gpr[4] = af;
  cpu.gpr[5] = type;
  cpu.gpr[6] = protocol;
  auto ctx = make_context(cpu, memory, thread_id);
  const auto result = registry.invoke("xam", xam::ordinal::NetDll_socket, ctx);
  assert(result.handled && result.success);
  return static_cast<std::uint32_t>(cpu.gpr[3]);
}

// Writes a real XSOCKADDR_IN (family, port, IPv4 address, 8 zero bytes) at `address`.
void write_sockaddr_in(memory::AddressSpace& memory, memory::GuestAddress address,
                       std::uint16_t port, std::uint32_t ipv4_host_order) {
  constexpr std::uint16_t kAfInet = 2u;
  memory.write16_be(address + 0u, kAfInet);
  memory.write16_be(address + 2u, port);
  memory.write32_be(address + 4u, ipv4_host_order);
  for (std::uint32_t i = 8u; i < 16u; ++i) memory.write8(address + i, 0u);
}

void test_udp_loopback_round_trip() {
  std::cout << "[TEST] NetDll_socket UDP loopback bind/sendto/recvfrom/close..." << std::endl;

  core::ExportRegistry registry;
  xam::XamSession xam;
  assert(xam.initialize());
  core::XenonSession session;
  assert(xam.register_exports(registry, session));

  memory::AddressSpace memory(memory::GuestTranslationMode::Compact);
  assert(memory.initialize());

  constexpr std::uint32_t kAfInet = 2u;
  constexpr std::uint32_t kSockDgram = 2u;
  constexpr std::uint32_t kThreadA = 11u;
  constexpr std::uint32_t kThreadB = 22u;

  const auto handle_a = invoke_socket(registry, memory, kThreadA, kAfInet, kSockDgram, 0u);
  const auto handle_b = invoke_socket(registry, memory, kThreadB, kAfInet, kSockDgram, 0u);
  assert(handle_a != 0xFFFFFFFFu && handle_b != 0xFFFFFFFFu && handle_a != handle_b);

  // Bind A to an ephemeral loopback port (port 0 == "pick one for me", exactly
  // like real Winsock/BSD bind()).
  memory::GuestAddress addr_a{}, addr_b{}, name_buf{};
  assert(memory.allocate(16u, 4u, memory::kReadWrite, false, addr_a));
  assert(memory.allocate(16u, 4u, memory::kReadWrite, false, addr_b));
  assert(memory.allocate(16u, 4u, memory::kReadWrite, false, name_buf));
  write_sockaddr_in(memory, addr_a, 0u, 0x7F000001u);  // 127.0.0.1:0

  cpu::CpuState bind_cpu{};
  bind_cpu.gpr[4] = handle_a;
  bind_cpu.gpr[5] = addr_a;
  auto bind_ctx = make_context(bind_cpu, memory, kThreadA);
  auto bind_result = registry.invoke("xam", xam::ordinal::NetDll_bind, bind_ctx);
  assert(bind_result.handled && bind_result.success);
  assert(static_cast<std::int32_t>(bind_cpu.gpr[3]) == 0 && "bind() must succeed on loopback");

  // Discover which ephemeral port the OS actually gave socket A, via
  // NetDll_getsockopt-adjacent introspection is not modeled, so read it back with
  // the host API directly is not available from the test - instead, bind A to a
  // FIXED high port to make the round trip deterministic and portable.
  //
  // Re-bind is not valid on a live socket, so start over with a fixed port instead
  // of the ephemeral one above (kept only to prove ephemeral bind succeeds).
  const auto handle_c = invoke_socket(registry, memory, kThreadA, kAfInet, kSockDgram, 0u);
  assert(handle_c != 0xFFFFFFFFu);
  constexpr std::uint16_t kFixedPort = 47811u;
  write_sockaddr_in(memory, addr_a, kFixedPort, 0x7F000001u);
  cpu::CpuState bind2_cpu{};
  bind2_cpu.gpr[4] = handle_c;
  bind2_cpu.gpr[5] = addr_a;
  auto bind2_ctx = make_context(bind2_cpu, memory, kThreadA);
  const auto bind2_result = registry.invoke("xam", xam::ordinal::NetDll_bind, bind2_ctx);
  assert(bind2_result.handled && bind2_result.success);
  assert(static_cast<std::int32_t>(bind2_cpu.gpr[3]) == 0 &&
         "bind() to a fixed loopback port must succeed");

  // B sends "PING" to A's fixed port.
  memory::GuestAddress payload{};
  assert(memory.allocate(16u, 4u, memory::kReadWrite, false, payload));
  const char ping[] = "PING";
  for (std::size_t i = 0; i < sizeof(ping) - 1; ++i) {
    memory.write8(payload + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(ping[i]));
  }
  write_sockaddr_in(memory, addr_b, kFixedPort, 0x7F000001u);
  cpu::CpuState sendto_cpu{};
  sendto_cpu.gpr[4] = handle_b;
  sendto_cpu.gpr[5] = payload;
  sendto_cpu.gpr[6] = static_cast<std::uint32_t>(sizeof(ping) - 1);
  sendto_cpu.gpr[7] = 0u;  // flags
  sendto_cpu.gpr[8] = addr_b;
  auto sendto_ctx = make_context(sendto_cpu, memory, kThreadB);
  auto sendto_result = registry.invoke("xam", xam::ordinal::NetDll_sendto, sendto_ctx);
  assert(sendto_result.handled && sendto_result.success);
  assert(sendto_cpu.gpr[3] == sizeof(ping) - 1 && "sendto() must report the full datagram sent");

  // A receives it via recvfrom - a real UDP loopback delivery, not a fake echo.
  memory::GuestAddress recv_buf{};
  assert(memory.allocate(64u, 4u, memory::kReadWrite, false, recv_buf));
  memory.fill_bytes(recv_buf, 64u, 0u);
  cpu::CpuState recvfrom_cpu{};
  recvfrom_cpu.gpr[4] = handle_c;
  recvfrom_cpu.gpr[5] = recv_buf;
  recvfrom_cpu.gpr[6] = 64u;
  recvfrom_cpu.gpr[7] = 0u;
  recvfrom_cpu.gpr[8] = name_buf;
  auto recvfrom_ctx = make_context(recvfrom_cpu, memory, kThreadA);
  auto recvfrom_result = registry.invoke("xam", xam::ordinal::NetDll_recvfrom, recvfrom_ctx);
  assert(recvfrom_result.handled && recvfrom_result.success);
  assert(recvfrom_cpu.gpr[3] == sizeof(ping) - 1 &&
         "recvfrom() must report the real datagram length received over loopback");
  for (std::size_t i = 0; i < sizeof(ping) - 1; ++i) {
    assert(memory.read8(recv_buf + static_cast<std::uint32_t>(i)) ==
           static_cast<std::uint8_t>(ping[i]));
  }
  // The peer address recvfrom() filled in must be the real sender: 127.0.0.1.
  assert(memory.read32_be(name_buf + 4u) == 0x7F000001u);

  // Cleanly close every socket - real closesocket(), not a leak.
  for (const auto handle : {handle_a, handle_b, handle_c}) {
    cpu::CpuState close_cpu{};
    close_cpu.gpr[4] = handle;
    auto close_ctx = make_context(close_cpu, memory, kThreadA);
    auto close_result = registry.invoke("xam", xam::ordinal::NetDll_closesocket, close_ctx);
    assert(close_result.handled && close_result.success);
    assert(close_cpu.gpr[3] == 0u);
  }

  std::cout << "  \xE2\x9C\x93 A real UDP datagram round-tripped over loopback through NetDll_*"
            << std::endl;
}

// Every socket-taking NetDll_* export must fail with WSAENOTSOCK (not crash, not
// silently succeed) on a handle that was never opened or was already closed -
// this is the shared not-a-socket contract every handler in xam_net_exports.cpp
// relies on.
void test_operations_on_invalid_handle_fail_cleanly() {
  std::cout << "[TEST] NetDll_* socket ops reject an unopened handle..." << std::endl;

  core::ExportRegistry registry;
  xam::XamSession xam;
  assert(xam.initialize());
  core::XenonSession session;
  assert(xam.register_exports(registry, session));

  memory::AddressSpace memory(memory::GuestTranslationMode::Compact);
  assert(memory.initialize());

  constexpr std::uint32_t kThread = 5u;
  constexpr std::uint32_t kBogusHandle = 0xDEADBEEFu;
  constexpr std::uint32_t kWsaenotsock = 10038u;

  cpu::CpuState close_cpu{};
  close_cpu.gpr[4] = kBogusHandle;
  auto close_ctx = make_context(close_cpu, memory, kThread);
  auto close_result = registry.invoke("xam", xam::ordinal::NetDll_closesocket, close_ctx);
  assert(close_result.handled && close_result.success);
  assert(static_cast<std::int32_t>(close_cpu.gpr[3]) == -1);

  cpu::CpuState error_cpu{};
  auto error_ctx = make_context(error_cpu, memory, kThread);
  auto error_result = registry.invoke("xam", xam::ordinal::NetDll_WSAGetLastError, error_ctx);
  assert(error_result.handled && error_result.success);
  assert(error_cpu.gpr[3] == kWsaenotsock &&
         "closesocket() on an unopened handle must set WSAENOTSOCK");

  // A DIFFERENT thread's last error must be unaffected - the error is per-thread.
  cpu::CpuState other_thread_error_cpu{};
  auto other_ctx = make_context(other_thread_error_cpu, memory, kThread + 1u);
  auto other_result = registry.invoke("xam", xam::ordinal::NetDll_WSAGetLastError, other_ctx);
  assert(other_result.handled && other_result.success);
  assert(other_thread_error_cpu.gpr[3] == 0u &&
         "a socket error on one guest thread must not leak into another's last-error state");

  // send() on the same bogus handle must also fail the same way, not crash.
  cpu::CpuState send_cpu{};
  send_cpu.gpr[4] = kBogusHandle;
  send_cpu.gpr[5] = 0u;
  send_cpu.gpr[6] = 0u;
  send_cpu.gpr[7] = 0u;
  auto send_ctx = make_context(send_cpu, memory, kThread);
  auto send_result = registry.invoke("xam", xam::ordinal::NetDll_send, send_ctx);
  assert(send_result.handled && send_result.success);
  assert(static_cast<std::int32_t>(send_cpu.gpr[3]) == -1);

  std::cout << "  \xE2\x9C\x93 Invalid socket handles fail with a real, per-thread WSAENOTSOCK"
            << std::endl;
}

// WSASetLastError/WSAGetLastError must round-trip exactly, independent of any
// socket ever being opened - a title may probe/clear its own error state directly.
void test_set_and_get_last_error_round_trip() {
  std::cout << "[TEST] NetDll_WSASetLastError/WSAGetLastError round-trip..." << std::endl;

  core::ExportRegistry registry;
  xam::XamSession xam;
  assert(xam.initialize());
  core::XenonSession session;
  assert(xam.register_exports(registry, session));

  memory::AddressSpace memory(memory::GuestTranslationMode::Compact);
  assert(memory.initialize());

  constexpr std::uint32_t kThread = 9u;
  cpu::CpuState set_cpu{};
  set_cpu.gpr[3] = 10061u;  // WSAECONNREFUSED
  auto set_ctx = make_context(set_cpu, memory, kThread);
  auto set_result = registry.invoke("xam", xam::ordinal::NetDll_WSASetLastError, set_ctx);
  assert(set_result.handled && set_result.success);

  cpu::CpuState get_cpu{};
  auto get_ctx = make_context(get_cpu, memory, kThread);
  auto get_result = registry.invoke("xam", xam::ordinal::NetDll_WSAGetLastError, get_ctx);
  assert(get_result.handled && get_result.success);
  assert(get_cpu.gpr[3] == 10061u);

  std::cout << "  \xE2\x9C\x93 WSASetLastError/WSAGetLastError round-trip correctly" << std::endl;
}

}  // namespace

int main() {
  test_udp_loopback_round_trip();
  test_operations_on_invalid_handle_fail_cleanly();
  test_set_and_get_last_error_round_trip();
  std::cout << "All NetDll_* socket export tests passed!" << std::endl;
  return 0;
}

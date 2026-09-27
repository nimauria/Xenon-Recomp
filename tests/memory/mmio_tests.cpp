#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "xenon/memory/address_space.hpp"

using namespace xenon::memory;
using namespace std::chrono_literals;

static bool faults(auto&& fn, FaultReason reason) {
  try {
    fn();
  } catch (const MemoryFault& e) {
    return e.reason() == reason;
  }
  return false;
}

int main() {
  AddressSpace memory;
  assert(memory.initialize());

  constexpr GuestAddress kNormal = 0x00600000u;
  constexpr GuestAddress kManagementPage = 0x00601000u;
  assert(memory.commit_fixed(kNormal, kBasePageSize, kReadWrite));
  memory.write32_be(kNormal, 0x11223344u);

  // Adding a large device catalogue must not put unrelated RAM on the slow
  // path. The hot page entry remains directly resolvable regardless of the
  // number of registered devices.
  {
    auto before = memory.access_context();
    xenon::cpu::MemoryAccessContext::PhysicalResolution resolution{};
    assert(before.resolve_physical_ram(kNormal, sizeof(std::uint32_t), false,
                                       alignof(std::uint32_t), resolution));
  }

  constexpr GuestAddress kCatalogueBase = 0x02000000u;
  constexpr std::uint32_t kDeviceCount = 256u;
  std::array<std::atomic<std::uint32_t>, kDeviceCount> registers{};
  for (std::uint32_t i = 0; i < kDeviceCount; ++i) {
    const auto base = kCatalogueBase + i * 2u * kBasePageSize;
    registers[i].store(0xA5000000u | i, std::memory_order_relaxed);
    assert(memory.add_mmio_range(
        base, kBasePageSize,
        [&, i](GuestAddress, std::uint32_t width) -> std::uint64_t {
          assert(width == 4u);
          return registers[i].load(std::memory_order_relaxed);
        },
        [&, i](GuestAddress, std::uint32_t width, std::uint64_t value) {
          assert(width == 4u);
          registers[i].store(static_cast<std::uint32_t>(value),
                             std::memory_order_relaxed);
        },
        "catalogue-device"));
  }

  {
    auto after = memory.access_context();
    xenon::cpu::MemoryAccessContext::PhysicalResolution resolution{};
    assert(after.resolve_physical_ram(kNormal, sizeof(std::uint32_t), false,
                                      alignof(std::uint32_t), resolution));
    assert(after.read32_be(kNormal) == 0x11223344u);
  }

  const auto last_device =
      kCatalogueBase + (kDeviceCount - 1u) * 2u * kBasePageSize;
  assert(memory.read32_be(last_device) ==
         (0xA5000000u | (kDeviceCount - 1u)));
  memory.write32_be(last_device, 0xCAFEBABEu);
  assert(registers.back().load(std::memory_order_relaxed) == 0xCAFEBABEu);

  // Sorted registration rejects overlap but accepts adjacent ranges, including
  // multiple independent handlers within the same 4 KiB slow page.
  constexpr GuestAddress kSubPage = 0x03000000u;
  assert(memory.commit_fixed(kSubPage, kBasePageSize, kReadWrite));
  memory.write32_be(kSubPage + 0x300u, 0xDEADBEEFu);
  assert(memory.add_mmio_range(
      kSubPage + 0x100u, 4u,
      [](GuestAddress, std::uint32_t width) -> std::uint64_t {
        assert(width == 4u);
        return 0x01020304u;
      },
      [](GuestAddress, std::uint32_t, std::uint64_t) {}, "subpage-a"));
  assert(memory.add_mmio_range(
      kSubPage + 0x104u, 4u,
      [](GuestAddress, std::uint32_t width) -> std::uint64_t {
        assert(width == 4u);
        return 0x05060708u;
      },
      [](GuestAddress, std::uint32_t, std::uint64_t) {}, "subpage-b"));
  assert(!memory.add_mmio_range(
      kSubPage + 0x102u, 4u,
      [](GuestAddress, std::uint32_t) -> std::uint64_t { return 0u; },
      [](GuestAddress, std::uint32_t, std::uint64_t) {}, "overlap"));
  assert(memory.read32_be(kSubPage + 0x100u) == 0x01020304u);
  assert(memory.read32_be(kSubPage + 0x104u) == 0x05060708u);
  // The whole page is correctly marked slow, but ordinary bytes outside the
  // device intervals still resolve to the underlying RAM on the cold path.
  {
    auto access = memory.access_context();
    xenon::cpu::MemoryAccessContext::PhysicalResolution resolution{};
    assert(!access.resolve_physical_ram(kSubPage + 0x300u,
                                        sizeof(std::uint32_t), false,
                                        alignof(std::uint32_t), resolution));
  }
  assert(memory.read32_be(kSubPage + 0x300u) == 0xDEADBEEFu);

  // Device handlers must execute without the global memory-management mutex.
  // Hold a callback open while another thread performs a cold mapping change;
  // the mapping must complete before the callback is released.
  constexpr GuestAddress kBlockingDevice = 0x04000000u;
  std::atomic<bool> handler_entered{false};
  std::atomic<bool> release_handler{false};
  std::atomic<bool> management_done{false};
  std::atomic<bool> management_ok{false};
  assert(memory.add_mmio_range(
      kBlockingDevice, kBasePageSize,
      [&](GuestAddress, std::uint32_t width) -> std::uint64_t {
        assert(width == 4u);
        handler_entered.store(true, std::memory_order_release);
        while (!release_handler.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        return 0x76543210u;
      },
      [](GuestAddress, std::uint32_t, std::uint64_t) {}, "blocking-device"));

  std::atomic<std::uint32_t> blocking_value{0u};
  std::thread reader([&] {
    blocking_value.store(memory.read32_be(kBlockingDevice),
                         std::memory_order_release);
  });
  while (!handler_entered.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  std::thread manager([&] {
    management_ok.store(
        memory.commit_fixed(kManagementPage, kBasePageSize, kReadWrite),
        std::memory_order_relaxed);
    management_done.store(true, std::memory_order_release);
  });

  const auto deadline = std::chrono::steady_clock::now() + 500ms;
  while (!management_done.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  const bool management_progressed_while_handler_blocked =
      management_done.load(std::memory_order_acquire);
  release_handler.store(true, std::memory_order_release);
  reader.join();
  manager.join();
  assert(management_progressed_while_handler_blocked);
  assert(management_ok.load(std::memory_order_relaxed));
  assert(blocking_value.load(std::memory_order_acquire) == 0x76543210u);

  // A callback may remove the MMIO catalogue containing itself. Snapshot-owned
  // handlers stay alive until the invocation returns, while later accesses see
  // the newly published table state.
  AddressSpace self_clearing;
  assert(self_clearing.initialize());
  constexpr GuestAddress kSelfClearing = 0xFFD20000u;
  std::atomic<std::uint32_t> self_calls{0u};
  assert(self_clearing.add_mmio_range(
      kSelfClearing, 4u,
      [&](GuestAddress, std::uint32_t width) -> std::uint64_t {
        assert(width == 4u);
        self_calls.fetch_add(1u, std::memory_order_relaxed);
        self_clearing.clear_mmio_ranges();
        return 0x89ABCDEFu;
      },
      [](GuestAddress, std::uint32_t, std::uint64_t) {}, "self-clearing"));
  assert(self_clearing.read32_be(kSelfClearing) == 0x89ABCDEFu);
  assert(self_calls.load(std::memory_order_relaxed) == 1u);
  assert(faults([&] { (void)self_clearing.read32_be(kSelfClearing); },
                FaultReason::Unmapped));

  // Range operations must also avoid holding the management mutex across MMIO
  // callbacks. Byte-visible behavior is preserved on the cold path.
  AddressSpace range_memory;
  assert(range_memory.initialize());
  constexpr GuestAddress kByteDevice = 0x05000000u;
  std::array<std::uint8_t, 16> device_bytes{};
  for (std::uint32_t i = 0; i < device_bytes.size(); ++i) {
    device_bytes[i] = static_cast<std::uint8_t>(0x40u + i);
  }
  assert(range_memory.add_mmio_range(
      kByteDevice, static_cast<std::uint32_t>(device_bytes.size()),
      [&](GuestAddress address, std::uint32_t width) -> std::uint64_t {
        assert(width == 1u);
        return device_bytes.at(address - kByteDevice);
      },
      [&](GuestAddress address, std::uint32_t width, std::uint64_t value) {
        assert(width == 1u);
        device_bytes.at(address - kByteDevice) =
            static_cast<std::uint8_t>(value);
      },
      "byte-device"));
  std::array<std::byte, 8> bytes{};
  range_memory.read_bytes(kByteDevice + 4u, bytes);
  for (std::uint32_t i = 0; i < bytes.size(); ++i) {
    assert(std::to_integer<std::uint8_t>(bytes[i]) == 0x44u + i);
  }
  const std::array<std::byte, 4> replacement{
      std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
  range_memory.write_bytes(kByteDevice + 6u, replacement);
  assert(device_bytes[6] == 0x11u && device_bytes[9] == 0x44u);

  return 0;
}

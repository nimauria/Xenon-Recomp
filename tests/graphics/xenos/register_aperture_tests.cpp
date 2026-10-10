// Xenos register aperture (0x7FC80000).
//
// Regression for Ace Combat 6: the title reads the display interrupt status from
// its vsync callback and publishes ring-buffer commands by storing the write
// pointer to CP_RB_WPTR. The range used to be plain RAM, so loads returned zero
// and stores never reached the command processor and the game waited on the GPU
// forever. These tests drive the aperture exactly as guest code does - through
// AddressSpace big-endian loads and stores.

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

#include "xenon/gpu/register_aperture.hpp"
#include "xenon/gpu/register_file.hpp"
#include "xenon/memory/address_space.hpp"

using namespace xenon;
using namespace xenon::gpu;

namespace {

constexpr memory::GuestAddress reg_address(std::uint32_t index) {
  return kXenosRegisterApertureBase + index * 4u;
}

struct Fixture {
  memory::AddressSpace memory;
  RegisterFile registers;
  std::vector<std::uint32_t> write_pointers;
  XenosRegisterAperture aperture{registers, [this](std::uint32_t value) {
                                   write_pointers.push_back(value);
                                 }};

  Fixture() { assert(aperture.attach(memory)); }
};

// The hardware reports these values regardless of what the guest last wrote.
void test_fixed_hardware_registers() {
  Fixture f;
  assert(f.memory.read32_be(reg_address(xenos_register::kRbEdramTiming)) == 0x08100748u);
  assert(f.memory.read32_be(reg_address(xenos_register::kRbBcControl)) == 0x0000200Eu);
  assert(f.memory.read32_be(reg_address(xenos_register::kD1ModeVCounter)) == 0x000002D0u);
  assert(f.memory.read32_be(reg_address(xenos_register::kD1ModeViewportSize)) == 0x050002D0u);
  // The vsync callback tests this for the vblank bit; zero (plain RAM) made it
  // conclude nothing had happened.
  assert(f.memory.read32_be(reg_address(xenos_register::kInterruptStatus)) == 1u);
  f.memory.write32_be(reg_address(xenos_register::kInterruptStatus), 0u);
  assert(f.memory.read32_be(reg_address(xenos_register::kInterruptStatus)) == 1u);
}

// A store to CP_RB_WPTR reaches the command processor hook with the written
// value, once per store, and reads back.
void test_write_pointer_store_reaches_hook() {
  Fixture f;
  f.memory.write32_be(reg_address(xenos_register::kCpRbWptr), 0x40u);
  f.memory.write32_be(reg_address(xenos_register::kCpRbWptr), 0x1C0u);
  assert((f.write_pointers == std::vector<std::uint32_t>{0x40u, 0x1C0u}));
  assert(f.memory.read32_be(reg_address(xenos_register::kCpRbWptr)) == 0x1C0u);
  assert(f.aperture.cp_write_pointer_updates() == 2u);
}

// Ordinary registers latch what was written, and share state with the register
// file the command processor uses.
void test_plain_registers_latch_and_share_the_register_file() {
  Fixture f;
  assert(f.memory.read32_be(reg_address(0x2000u)) == 0u);
  f.memory.write32_be(reg_address(0x2000u), 0xDEADBEEFu);
  assert(f.memory.read32_be(reg_address(0x2000u)) == 0xDEADBEEFu);
  assert(f.registers.read(0x2000u) == 0xDEADBEEFu);
  assert(f.write_pointers.empty() && "only CP_RB_WPTR feeds the write pointer");

  f.registers.write(0x2001u, 0x12345678u);  // e.g. a PM4 SET_CONSTANT
  assert(f.memory.read32_be(reg_address(0x2001u)) == 0x12345678u);
}

// Registers are addressed by (address & 0xFFFF) / 4 across the whole 64 KiB.
void test_addressing_covers_the_whole_aperture() {
  Fixture f;
  f.memory.write32_be(kXenosRegisterApertureBase + 0xFFFCu, 0xA5A5A5A5u);
  assert(f.memory.read32_be(kXenosRegisterApertureBase + 0xFFFCu) == 0xA5A5A5A5u);
  assert(f.registers.read(0x3FFFu) == 0xA5A5A5A5u);
}

// Narrow and wide accesses select byte lanes of the big-endian register value.
void test_narrow_and_wide_accesses() {
  Fixture f;
  const auto base = reg_address(0x2100u);
  f.memory.write32_be(base, 0x11223344u);
  f.memory.write32_be(base + 4u, 0x55667788u);
  assert(f.memory.read8(base) == 0x11u && f.memory.read8(base + 3u) == 0x44u);
  assert(f.memory.read16_be(base) == 0x1122u && f.memory.read16_be(base + 2u) == 0x3344u);
  assert(f.memory.read64_be(base) == 0x1122334455667788ull);

  f.memory.write8(base + 1u, 0xAAu);
  assert(f.registers.read(0x2100u) == 0x11AA3344u);
  f.memory.write16_be(base + 2u, 0xBEEFu);
  assert(f.registers.read(0x2100u) == 0x11AABEEFu);
  f.memory.write64_be(base, 0x0102030405060708ull);
  assert(f.registers.read(0x2100u) == 0x01020304u && f.registers.read(0x2101u) == 0x05060708u);
}

// Once the aperture object is gone, a guest access must not touch freed state.
void test_access_after_destruction_is_inert() {
  memory::AddressSpace memory;
  RegisterFile registers;
  {
    XenosRegisterAperture aperture(registers, {});
    assert(aperture.attach(memory));
  }
  assert(memory.read32_be(reg_address(0x2000u)) == 0u);
  memory.write32_be(reg_address(0x2000u), 7u);  // must not crash
}

// A second attach over the same range is rejected rather than silently stacked.
void test_range_cannot_be_attached_twice() {
  Fixture f;
  XenosRegisterAperture other(f.registers, {});
  assert(!other.attach(f.memory));
}

}  // namespace

int main() {
  std::cout << "Testing Xenos register aperture...\n";
  test_fixed_hardware_registers();
  test_write_pointer_store_reaches_hook();
  test_plain_registers_latch_and_share_the_register_file();
  test_addressing_covers_the_whole_aperture();
  test_narrow_and_wide_accesses();
  test_access_after_destruction_is_inert();
  test_range_cannot_be_attached_twice();
  std::cout << "All Xenos register aperture tests passed!\n";
  return 0;
}

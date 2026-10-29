// Native guest access (AddressSpace::set_native_access): the production
// default, where a guest access pays no shared atomic. These tests pin the
// native contract; memory_tests.cpp keeps covering strict access.
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

#include "xenon/memory/address_space.hpp"

using namespace xenon::memory;

namespace {

constexpr GuestAddress kPage = 0x00100000u;

void init_native(AddressSpace& mem) {
  assert(mem.initialize());
  mem.set_native_access(true);
  assert(mem.native_access());
  assert(mem.commit_fixed(kPage, 0x2000u, kReadWrite));
}

void test_native_store_publishes_its_page_when_an_epoch_is_requested() {
  AddressSpace mem;
  init_native(mem);
  auto& coherency = mem.coherency();
  const auto phys = mem.get_physical_address(kPage);
  const auto other_phys = mem.get_physical_address(kPage + 0x1000u);
  const auto before = coherency.current_epoch();

  auto access = mem.access_context();
  access.write32_be(kPage + 0x10u, 0x11223344u);
  // The store itself takes no epoch: the shared write counter is untouched
  // until a consumer asks for an epoch.
  assert(coherency.write_epoch_data()->load() == before);

  const auto after = coherency.current_epoch();
  assert(after > before);
  assert(coherency.range_changed_since(phys + 0x10u, 4u, before));
  assert(!coherency.range_changed_since(other_phys, 4u, before));
  assert(mem.read32_be(kPage + 0x10u) == 0x11223344u);

  // Exact history is not kept for native stores: consumers take the
  // page-level path for an interval that contains one.
  std::vector<DirtyPhysicalWrite> exact;
  assert(!coherency.collect_exact_dirty_writes(before, after, exact));
  std::vector<DirtyPhysicalRange> pages;
  coherency.collect_dirty_ranges(before, after, 0u, pages);
  assert(pages.size() == 1u && pages[0].address == (phys & ~0xFFFu) &&
         pages[0].size == 0x1000u);

  // Nothing dirty since: asking again does not mint an epoch.
  assert(coherency.current_epoch() == after);

  // A page already collected is reported again by its next store, whatever
  // the width or path of that store.
  access.write8(kPage + 0x20u, 7u);
  const auto again = coherency.current_epoch();
  assert(again > after);
  assert(coherency.range_changed_since(phys, 0x1000u, after));
  std::array<std::byte, 16> bytes{};
  bytes.fill(std::byte{0x5A});
  access.write_bytes(kPage + 0x1800u, bytes);
  assert(coherency.current_epoch() > again);
  assert(coherency.range_changed_since(other_phys, 0x1000u, again));
  assert(!coherency.range_changed_since(phys, 0x1000u, again));
}

void test_native_reservation_is_a_compare_and_swap_of_the_reserved_value() {
  AddressSpace mem;
  init_native(mem);
  mem.write32_be(kPage, 5u);
  auto a = mem.access_context();
  std::uint32_t value{};

  auto token = a.reserve32(kPage, value);
  assert(token != 0u && value == 5u);
  assert(a.store_conditional32(kPage, token, 6u));
  assert(mem.read32_be(kPage) == 6u);
  // stwcx. clears the reservation whether or not it stores.
  assert(!a.store_conditional32(kPage, token, 7u));
  assert(mem.read32_be(kPage) == 6u);

  // Another thread changing the word defeats the reservation.
  token = a.reserve32(kPage, value);
  std::thread([&] {
    auto b = mem.access_context();
    b.write32_be(kPage, 9u);
  }).join();
  assert(!a.store_conditional32(kPage, token, 8u));
  assert(mem.read32_be(kPage) == 9u);

  // A store that leaves the reserved value in place does not (the value
  // comparison of xenia/rexglue/XenonRecomp; strict access fails here).
  token = a.reserve32(kPage, value);
  std::thread([&] {
    auto b = mem.access_context();
    b.write32_be(kPage, 9u);
  }).join();
  assert(a.store_conditional32(kPage, token, 10u));
  assert(mem.read32_be(kPage) == 10u);

  // The reservation names one word: a conditional store elsewhere fails.
  token = a.reserve32(kPage, value);
  assert(!a.store_conditional32(kPage + 4u, token, 1u));
  assert(mem.read32_be(kPage + 4u) == 0u);

  // A newer reservation replaces the older one.
  const auto older = a.reserve32(kPage, value);
  const auto newer = a.reserve32(kPage + 8u, value);
  assert(!a.store_conditional32(kPage, older, 1u));
  assert(a.store_conditional32(kPage + 8u, newer, 2u) == false);  // taken by the failed stwcx.
  assert(mem.read32_be(kPage) == 10u && mem.read32_be(kPage + 8u) == 0u);

  // Cancelling clears it.
  token = a.reserve32(kPage, value);
  a.cancel_reservation(token);
  assert(!a.store_conditional32(kPage, token, 1u));

  // A reservation belongs to the thread that made it.
  token = a.reserve32(kPage, value);
  bool other_thread_stored = true;
  std::thread([&] {
    auto b = mem.access_context();
    other_thread_stored = b.store_conditional32(kPage, token, 1u);
  }).join();
  assert(!other_thread_stored);
  assert(mem.read32_be(kPage) == 10u);

  // Identity is the physical word: a store through the 0xA0000000 alias of
  // the reserved word succeeds.
  const auto alias = 0xA0000000u + mem.get_physical_address(kPage);
  token = a.reserve32(kPage, value);
  assert(a.store_conditional32(alias, token, 11u));
  assert(mem.read32_be(kPage) == 11u);

  // 64-bit reservations, and no width mixing.
  mem.write64_be(kPage + 0x100u, 0x1122334455667788ull);
  std::uint64_t value64{};
  auto token64 = a.reserve64(kPage + 0x100u, value64);
  assert(value64 == 0x1122334455667788ull);
  assert(a.store_conditional64(kPage + 0x100u, token64, 0x8877665544332211ull));
  assert(mem.read64_be(kPage + 0x100u) == 0x8877665544332211ull);
  token = a.reserve32(kPage + 0x100u, value);
  assert(!a.store_conditional64(kPage + 0x100u, token, 1ull));

  // A successful conditional store is a store: its page is reported.
  const auto since = mem.coherency().current_epoch();
  token = a.reserve32(kPage, value);
  assert(a.store_conditional32(kPage, token, 12u));
  assert(mem.coherency().current_epoch() > since);
  assert(mem.coherency().range_changed_since(mem.get_physical_address(kPage), 4u, since));
}

void test_native_reservations_make_an_atomic_increment() {
  AddressSpace mem;
  init_native(mem);
  constexpr int kThreads = 6;
  constexpr int kIncrements = 20000;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&] {
      auto access = mem.access_context();
      for (int i = 0; i < kIncrements; ++i) {
        for (;;) {
          std::uint32_t value{};
          const auto token = access.reserve32(kPage, value);
          // Unrelated stores to the same reservation granule, as a lock word's
          // neighbours get, must not break the increment.
          access.write32_be(kPage + 0x40u, static_cast<std::uint32_t>(i));
          if (access.store_conditional32(kPage, token, value + 1u)) break;
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();
  assert(mem.read32_be(kPage) == std::uint32_t{kThreads} * kIncrements);
}

// Device writers (GPU readback windows, write_physical) keep the strict
// bookkeeping in native mode: they publish exact epochs and journal entries.
// A native view handed to the readback window once crashed AC6's first
// resolve, because the window counts itself as an in-flight writer.
void test_device_writers_keep_exact_history_in_native_mode() {
  AddressSpace mem;
  init_native(mem);
  const auto phys = mem.get_physical_address(kPage);
  const std::array<std::byte, 8> data{std::byte{1}, std::byte{2}, std::byte{3},
                                      std::byte{4}, std::byte{5}, std::byte{6},
                                      std::byte{7}, std::byte{8}};
  const auto before = mem.coherency().current_epoch();
  std::uint64_t window_epoch = 0u;
  {
    auto window = mem.physical_write_window();
    assert(window);
    assert(window.write(phys + 0x40u, data, &window_epoch));
  }
  assert(window_epoch > before);
  std::uint64_t written_epoch = 0u;
  assert(mem.write_physical(phys + 0x80u, data, &written_epoch));
  assert(mem.fill_physical(phys + 0xC0u, 4u, std::byte{0x77}));
  const auto after = mem.coherency().current_epoch();
  std::vector<DirtyPhysicalWrite> exact;
  assert(mem.coherency().collect_exact_dirty_writes(before, after, exact));
  assert(exact.size() == 3u);
  assert(exact[0].epoch == window_epoch && exact[0].address == phys + 0x40u &&
         exact[0].size == data.size());
  assert(exact[1].epoch == written_epoch && exact[1].address == phys + 0x80u);
  assert(exact[2].address == phys + 0xC0u && exact[2].size == 4u);
  assert(mem.read32_be(kPage + 0x40u) == 0x01020304u);
  assert(mem.read32_be(kPage + 0xC0u) == 0x77777777u);

  // Mixed with native stores, device writes still land and both are seen.
  auto access = mem.access_context();
  access.write32_be(kPage + 0x100u, 0xABCDEF01u);
  {
    auto window = mem.physical_write_window();
    assert(window.write(phys + 0x140u, data));
  }
  const auto mixed = mem.coherency().current_epoch();
  assert(mem.coherency().range_changed_since(phys + 0x100u, 4u, after));
  assert(mem.coherency().range_changed_since(phys + 0x140u, 8u, after));
  assert(mixed > after);
}

// Regression (AC6 spent about two thirds of its startup CPU time in calls to
// out-of-line memory accessors that MSVC would not inline into generated
// functions): the native path translates inline. It must stay exactly the
// checked path's semantics - values in both byte orders, unaligned and
// page-crossing accesses, faults, dirty pages and executable generations.
void test_native_inline_accessors_keep_the_checked_semantics() {
  AddressSpace mem;
  init_native(mem);
  auto access = mem.access_context();
  assert(access.native_inline());

  access.write8(kPage + 1u, 0xA5u);
  access.write16_be(kPage + 2u, 0x1234u);
  access.write32_be(kPage + 4u, 0x89ABCDEFu);
  access.write64_be(kPage + 8u, 0x0102030405060708ull);
  assert(mem.read8(kPage + 1u) == 0xA5u);
  assert(mem.read16_be(kPage + 2u) == 0x1234u);
  assert(mem.read32_be(kPage + 4u) == 0x89ABCDEFu);
  assert(mem.read64_be(kPage + 8u) == 0x0102030405060708ull);
  assert(access.read8(kPage + 1u) == 0xA5u && access.read16_be(kPage + 2u) == 0x1234u &&
         access.read32_be(kPage + 4u) == 0x89ABCDEFu &&
         access.read64_be(kPage + 8u) == 0x0102030405060708ull);
  // Big-endian in guest memory: the first byte is the most significant.
  assert(mem.read8(kPage + 4u) == 0x89u && mem.read8(kPage + 15u) == 0x08u);

  access.write16_le(kPage + 0x20u, 0x1234u);
  access.write32_le(kPage + 0x24u, 0x89ABCDEFu);
  access.write64_le(kPage + 0x28u, 0x0102030405060708ull);
  assert(mem.read16_le(kPage + 0x20u) == 0x1234u && mem.read8(kPage + 0x20u) == 0x34u);
  assert(access.read32_le(kPage + 0x24u) == 0x89ABCDEFu);
  assert(access.read64_le(kPage + 0x28u) == 0x0102030405060708ull);

  xenon::cpu::Vector128 vector{};
  for (std::size_t i = 0; i < vector.bytes.size(); ++i)
    vector.bytes[i] = static_cast<std::uint8_t>(0x10u + i);
  access.write128(kPage + 0x40u, vector);
  assert(mem.read8(kPage + 0x40u) == 0x10u && mem.read8(kPage + 0x4Fu) == 0x1Fu);
  assert(access.read128(kPage + 0x40u).bytes == vector.bytes);

  // Unaligned and page-crossing accesses take the checked path and agree.
  access.write32_be(kPage + 0x61u, 0xCAFEBABEu);
  assert(access.read32_be(kPage + 0x61u) == 0xCAFEBABEu && mem.read8(kPage + 0x61u) == 0xCAu);
  access.write64_be(kPage + 0xFFCu, 0x1122334455667788ull);
  assert(mem.read32_be(kPage + 0xFFCu) == 0x11223344u &&
         mem.read32_be(kPage + 0x1000u) == 0x55667788u);
  assert(access.read64_be(kPage + 0xFFCu) == 0x1122334455667788ull);
  access.write128(kPage + 0xFF8u, vector);
  assert(access.read128(kPage + 0xFF8u).bytes == vector.bytes);

  // Faults come from the checked path exactly as before.
  constexpr GuestAddress kUnmapped = 0x00400000u;
  bool read_faulted = false, write_faulted = false;
  try { (void)access.read32_be(kUnmapped); } catch (const MemoryFault&) { read_faulted = true; }
  assert(read_faulted);
  assert(mem.protect(kPage + 0x1000u, 0x1000u, Protect::Read));
  try { access.write32_be(kPage + 0x1000u, 1u); } catch (const MemoryFault&) { write_faulted = true; }
  // The page still holds bytes 8..11 of the page-crossing vector store above.
  assert(write_faulted && mem.read32_be(kPage + 0x1000u) == 0x18191A1Bu);
  assert(access.read32_be(kPage + 0x1000u) == 0x18191A1Bu);

  // Every store width publishes its page.
  const auto phys = mem.get_physical_address(kPage);
  auto& coherency = mem.coherency();
  const auto publishes = [&](auto&& store) {
    const auto before = coherency.current_epoch();
    store();
    return coherency.current_epoch() > before &&
           coherency.range_changed_since(phys, 0x1000u, before);
  };
  assert(publishes([&] { access.write8(kPage + 0x80u, 1u); }));
  assert(publishes([&] { access.write16_be(kPage + 0x80u, 2u); }));
  assert(publishes([&] { access.write32_le(kPage + 0x80u, 3u); }));
  assert(publishes([&] { access.write64_be(kPage + 0x80u, 4u); }));
  assert(publishes([&] { access.write128(kPage + 0x80u, vector); }));

  // A store into executable code moves its page's generation, so translated
  // code for that page is revalidated.
  constexpr GuestAddress kCode = 0x00200000u;
  assert(mem.commit_fixed(kCode, 0x1000u, kReadWriteExecute));
  auto code_access = mem.access_context();
  const auto stamp = code_access.executable_page_stamp(kCode);
  assert(stamp.executable());
  code_access.write32_be(kCode + 0x10u, 0x60000000u);
  const auto after_word = code_access.executable_page_stamp(kCode);
  assert(after_word.executable() && after_word.generation != stamp.generation);
  code_access.write8(kCode + 0x20u, 0u);
  assert(code_access.executable_page_stamp(kCode).generation != after_word.generation);
}

void test_strict_access_never_takes_the_native_inline_path() {
  AddressSpace mem;
  assert(mem.initialize());
  assert(mem.commit_fixed(kPage, 0x1000u, kReadWrite));
  auto access = mem.access_context();
  assert(!access.native_inline());
  access.write32_be(kPage, 0x01020304u);
  assert(access.read32_be(kPage) == 0x01020304u);
}

void test_strict_access_stays_the_default() {
  AddressSpace mem;
  assert(mem.initialize());
  assert(!mem.native_access());
  assert(mem.commit_fixed(kPage, 0x1000u, kReadWrite));
  mem.write32_be(kPage, 5u);
  std::uint32_t value{};
  const auto token = mem.reserve32(kPage, value);
  // Strict: any store to the granule defeats the reservation, even one that
  // leaves the value unchanged.
  mem.write32_be(kPage, 5u);
  assert(!mem.store_conditional32(kPage, token, 6u));
}

}  // namespace

int main() {
  test_native_store_publishes_its_page_when_an_epoch_is_requested();
  test_native_reservation_is_a_compare_and_swap_of_the_reserved_value();
  test_native_reservations_make_an_atomic_increment();
  test_device_writers_keep_exact_history_in_native_mode();
  test_native_inline_accessors_keep_the_checked_semantics();
  test_strict_access_never_takes_the_native_inline_path();
  test_strict_access_stays_the_default();
  std::cout << "native access tests passed\n";
  return 0;
}

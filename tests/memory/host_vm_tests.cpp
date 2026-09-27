#include "xenon/memory/host_vm.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace vm = xenon::memory::host_vm;

int main() {
  const auto page = vm::page_size();
  const auto granularity = vm::allocation_granularity();
  assert(page >= 4096u);
  assert((page & (page - 1u)) == 0u);
  assert(granularity >= page);
  assert((granularity % page) == 0u);

  const auto capabilities = vm::capabilities();
  assert(capabilities.page_size == page);
  assert(capabilities.allocation_granularity == granularity);
  assert(capabilities.fixed_shared_mapping ==
         vm::supports_fixed_shared_mapping());
  assert(capabilities.fixed_shared_mapping_requires_page_views ==
         vm::fixed_shared_mapping_requires_page_views());
  if (capabilities.fixed_shared_mapping) {
    assert(capabilities.fixed_shared_mapping_granularity >= page);
    assert((capabilities.fixed_shared_mapping_granularity % page) == 0u);
    assert(capabilities.supports_fixed_mapping_granularity(page));
  } else {
    assert(capabilities.fixed_shared_mapping_granularity == 0u);
    assert(!capabilities.supports_fixed_mapping_granularity(page));
  }

  // Anonymous reserve / commit / protect / discard / decommit semantics.
  const auto anonymous_size = page * 2u;
  auto* reservation = static_cast<std::byte*>(vm::reserve(anonymous_size));
  assert(reservation != nullptr);
  assert(vm::commit(reservation, anonymous_size, vm::Protection::ReadWrite));
  reservation[0] = std::byte{0x5A};
  reservation[page] = std::byte{0xA5};

  assert(vm::protect(reservation, page, vm::Protection::Read));
  assert(reservation[0] == std::byte{0x5A});
  assert(vm::protect(reservation, page, vm::Protection::ReadWrite));

  assert(vm::discard(reservation, page, vm::Protection::ReadWrite));
  assert(reservation[0] == std::byte{0});
  assert(reservation[page] == std::byte{0xA5});

  assert(vm::decommit(reservation + page, page));
  assert(vm::commit(reservation + page, page, vm::Protection::ReadWrite));
  assert(reservation[page] == std::byte{0});
  vm::release(reservation, anonymous_size);

  // Shared objects must expose coherent aliases without leaking native handle
  // types into the common API.
  const auto shared_size = granularity;
  auto shared = vm::create_shared(shared_size);
  assert(shared.valid());
  assert(shared.size() == shared_size);

  auto moved = std::move(shared);
  assert(!shared.valid());
  assert(moved.valid());

  auto* view_a = static_cast<std::byte*>(
      vm::map_shared(moved, 0u, shared_size, vm::Protection::ReadWrite));
  auto* view_b = static_cast<std::byte*>(
      vm::map_shared(moved, 0u, shared_size, vm::Protection::ReadWrite));
  assert(view_a != nullptr);
  assert(view_b != nullptr);

  view_a[17] = std::byte{0x37};
  view_a[shared_size - 1u] = std::byte{0xC4};
  assert(view_b[17] == std::byte{0x37});
  assert(view_b[shared_size - 1u] == std::byte{0xC4});
  view_b[33] = std::byte{0x91};
  assert(view_a[33] == std::byte{0x91});

  assert(vm::map_shared(moved, shared_size, page,
                        vm::Protection::ReadWrite) == nullptr);
  assert(vm::unmap(view_a, shared_size));
  assert(vm::unmap(view_b, shared_size));

  // Direct-aperture support is explicitly optional per host backend. When the
  // backend advertises it, replacing part of an existing reservation with a
  // fixed shared alias must preserve identity and restoring that slice must
  // keep the enclosing address range reserved.
  if (vm::supports_fixed_shared_mapping()) {
    auto* aperture = static_cast<std::byte*>(
        vm::reserve_fixed_shared_mapping_region(page * 2u));
    assert(aperture != nullptr);
    assert(vm::map_shared_fixed(moved, aperture, 0u, page,
                                vm::Protection::ReadWrite));
    assert(vm::map_shared_fixed(moved, aperture + page, 0u, page,
                                vm::Protection::ReadWrite));
    aperture[9] = std::byte{0x6D};
    assert(aperture[page + 9] == std::byte{0x6D});
    assert(vm::restore_reservation(aperture + page, page));
    assert(vm::map_shared_fixed(moved, aperture + page, 0u, page,
                                vm::Protection::ReadWrite));
    assert(aperture[page + 9] == std::byte{0x6D});
    vm::release_fixed_shared_mapping_region(aperture, page * 2u);
  }

  return 0;
}

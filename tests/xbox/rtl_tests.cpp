#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "xenon/memory/address_space.hpp"
#include "xenon/xbox/rtl.hpp"
#include "xenon/xbox/xex_loader.hpp"

namespace {

using namespace xenon;

// ---------------------------------------------------------------------------
// Synthetic XEX header builder for tests.
// ---------------------------------------------------------------------------

class SyntheticXexBuilder {
 public:
  // Create a minimal valid XEX2 root header with optional-header table.
  // Returns the header bytes in guest-memory layout (big-endian).
  std::vector<std::byte> build() const {
    std::vector<std::byte> bytes;

    // Root header
    append_be32(bytes, 0x58455832u);  // magic = 'XEX2'
    append_be32(bytes, 0x00000000u);  // module_flags
    append_be32(bytes, 0x00000040u);  // header_size (64 bytes = minimal)
    append_be32(bytes, 0x00000000u);  // image_size
    append_be32(bytes, 0x00000000u);  // security_info_offset
    append_be32(bytes, static_cast<std::uint32_t>(entries_.size()));  // optional_header_count

    // Optional header table
    for (const auto& entry : entries_) {
      append_be32(bytes, entry.key);
      append_be32(bytes, entry.value);
    }

    // Pad to at least header_size
    while (bytes.size() < 0x40u) {
      bytes.push_back(std::byte{0});
    }

    return bytes;
  }

  // Add an optional header entry
  SyntheticXexBuilder& add_field(std::uint32_t key, std::uint32_t value) {
    entries_.push_back({key, value});
    return *this;
  }

 private:
  struct Entry {
    std::uint32_t key;
    std::uint32_t value;
  };

  std::vector<Entry> entries_;

  static void append_be32(std::vector<std::byte>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::byte>((value >> 24) & 0xFFu));
    bytes.push_back(static_cast<std::byte>((value >> 16) & 0xFFu));
    bytes.push_back(static_cast<std::byte>((value >> 8) & 0xFFu));
    bytes.push_back(static_cast<std::byte>(value & 0xFFu));
  }
};

// ---------------------------------------------------------------------------
// Test harness with in-memory MemoryPort
// ---------------------------------------------------------------------------

class RtlTestFixture {
 public:
  RtlTestFixture()
      : address_space_(std::make_shared<memory::AddressSpace>()),
        memory_port_(*address_space_) {
    assert(address_space_->initialize());
  }

  // Allocate guest memory and write synthetic XEX header
  memory::GuestAddress allocate_header(const std::vector<std::byte>& header_bytes) {
    memory::GuestAddress addr{};
    if (!address_space_->allocate(header_bytes.size(), 16, memory::kReadWrite,
                                  /*top_down=*/true, addr)) {
      return 0;
    }
    try {
      memory_port_.write_bytes(addr, header_bytes);
    } catch (const memory::MemoryFault&) {
      return 0;
    }
    return addr;
  }

  xbox::XexOptionalHeaderView find_header(memory::GuestAddress header_addr,
                                           std::uint32_t key) {
    xbox::XexOptionalHeaderView result{};
    xbox::find_xex_optional_header(memory_port_, header_addr, key, result);
    return result;
  }

  std::optional<std::uint32_t> resolve_field(memory::GuestAddress header_addr,
                                             std::uint32_t key) {
    return xbox::resolve_xex_optional_header_field(memory_port_, header_addr, key);
  }

  memory::AddressSpace& memory_port() noexcept { return memory_port_; }

 protected:
  std::shared_ptr<memory::AddressSpace> address_space_;
  memory::AddressSpace& memory_port_;
};

}  // namespace

// ---------------------------------------------------------------------------
// TESTS
// ---------------------------------------------------------------------------

void test_rtl_inline_value_field() {
  // Test case: key with low byte 0x00 (inline value)
  RtlTestFixture fixture;

  auto xex = SyntheticXexBuilder()
                 .add_field(0x00010100u, 0x82000000u)  // EntryPoint
                 .build();

  const auto header_addr = fixture.allocate_header(xex);
  assert(header_addr != 0);

  const auto result = fixture.resolve_field(header_addr, 0x00010100u);
  assert(result.has_value());
  assert(*result == 0x82000000u);

  std::cout << "[PASS] test_rtl_inline_value_field" << std::endl;
}

void test_rtl_inline_offset_field() {
  // Test case: key with low byte 0x01 (return pointer to value field)
  RtlTestFixture fixture;

  auto xex = SyntheticXexBuilder()
                 .add_field(0x00020401u, 0x12345678u)  // DefaultHeapSize (class 0x01)
                 .build();

  const auto header_addr = fixture.allocate_header(xex);
  assert(header_addr != 0);

  const auto result = fixture.resolve_field(header_addr, 0x00020401u);
  assert(result.has_value());

  // For class 0x01, the result should be a pointer to the value field
  // = header_addr + entry_offset + 4 (past the key)
  // entry_offset should be 0x18 (root header size)
  const auto expected_ptr = header_addr + 0x18u + 4u;
  assert(*result == expected_ptr);

  std::cout << "[PASS] test_rtl_inline_offset_field" << std::endl;
}

void test_rtl_absent_key() {
  // Test case: key not found
  RtlTestFixture fixture;

  auto xex = SyntheticXexBuilder()
                 .add_field(0x00010100u, 0x82000000u)
                 .build();

  const auto header_addr = fixture.allocate_header(xex);
  assert(header_addr != 0);

  // Query for a key that doesn't exist
  const auto result = fixture.resolve_field(header_addr, 0xFFFFFFFFu);
  assert(!result.has_value());

  std::cout << "[PASS] test_rtl_absent_key" << std::endl;
}

void test_rtl_null_header() {
  // Test case: null header pointer
  RtlTestFixture fixture;

  const auto result = fixture.resolve_field(0, 0x00010100u);
  assert(!result.has_value());

  std::cout << "[PASS] test_rtl_null_header" << std::endl;
}

void test_rtl_ac6_regression() {
  // Regression test for AC6: key 0x20401 (DefaultHeapSize, class 0x01)
  // Should return a guest pointer that can be dereferenced to get heap size
  RtlTestFixture fixture;

  const std::uint32_t expected_heap_size = 0x01000000u;  // 16 MiB (example)

  auto xex = SyntheticXexBuilder()
                 .add_field(0x00020401u, expected_heap_size)
                 .build();

  const auto header_addr = fixture.allocate_header(xex);
  assert(header_addr != 0);

  // Call RtlImageXexHeaderField semantics
  const auto ptr_result = fixture.resolve_field(header_addr, 0x00020401u);
  assert(ptr_result.has_value());
  const auto heap_size_ptr = *ptr_result;

  // AC6 dereferences this pointer
  try {
    auto& mem_port = fixture.memory_port();
    const auto dereferenced = mem_port.read32_be(heap_size_ptr);
    assert(dereferenced == expected_heap_size);
  } catch (const memory::MemoryFault&) {
    assert(false);  // Should not throw
  }

  std::cout << "[PASS] test_rtl_ac6_regression" << std::endl;
}

void test_rtl_invalid_header_count() {
  // Test case: absurdly large optional-header count (should be bounded)
  RtlTestFixture fixture;

  std::vector<std::byte> bad_header;
  auto append_be32 = [&](std::uint32_t v) {
    bad_header.push_back(static_cast<std::byte>((v >> 24) & 0xFFu));
    bad_header.push_back(static_cast<std::byte>((v >> 16) & 0xFFu));
    bad_header.push_back(static_cast<std::byte>((v >> 8) & 0xFFu));
    bad_header.push_back(static_cast<std::byte>(v & 0xFFu));
  };

  append_be32(0x58455832u);  // magic = 'XEX2'
  append_be32(0x00000000u);  // module_flags
  append_be32(0x00000040u);  // header_size
  append_be32(0x00000000u);  // image_size
  append_be32(0x00000000u);  // security_info_offset
  append_be32(0xFFFFFFFFu);  // optional_header_count (huge, should be bounded)

  const auto header_addr = fixture.allocate_header(bad_header);
  assert(header_addr != 0);

  // Should return 0 (not found) rather than crashing
  const auto result = fixture.resolve_field(header_addr, 0x00010100u);
  assert(!result.has_value());

  std::cout << "[PASS] test_rtl_invalid_header_count" << std::endl;
}

int main() {
  test_rtl_inline_value_field();
  test_rtl_inline_offset_field();
  test_rtl_absent_key();
  test_rtl_null_header();
  test_rtl_ac6_regression();
  test_rtl_invalid_header_count();

  std::cout << "All RTL tests passed!" << std::endl;
  return 0;
}

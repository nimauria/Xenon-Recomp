#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "xenon/cpu/memory_port.hpp"

namespace xenon::cpu {

// Tiny contiguous memory implementation used only to make the CPU executable
// and testable before the real Xbox 360 RAM/address-space subsystem exists.
// It intentionally does not model pages, aliases, MMIO or GPU visibility.
class FlatMemory final : public MemoryPort {
 public:
  explicit FlatMemory(std::size_t size, GuestAddress base = 0);

  [[nodiscard]] GuestAddress base() const noexcept { return base_; }
  [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

  std::uint8_t read8(GuestAddress address) override;
  std::uint16_t read16_be(GuestAddress address) override;
  std::uint32_t read32_be(GuestAddress address) override;
  std::uint64_t read64_be(GuestAddress address) override;
  Vector128 read128(GuestAddress address) override;

  void write8(GuestAddress address, std::uint8_t value) override;
  void write16_be(GuestAddress address, std::uint16_t value) override;
  void write32_be(GuestAddress address, std::uint32_t value) override;
  void write64_be(GuestAddress address, std::uint64_t value) override;
  void write128(GuestAddress address, const Vector128& value) override;

  std::uint16_t read16_le(GuestAddress address) override;
  std::uint32_t read32_le(GuestAddress address) override;
  std::uint64_t read64_le(GuestAddress address) override;
  void write16_le(GuestAddress address, std::uint16_t value) override;
  void write32_le(GuestAddress address, std::uint32_t value) override;
  void write64_le(GuestAddress address, std::uint64_t value) override;

  std::uint64_t reserve32(GuestAddress address, std::uint32_t& value) override;
  std::uint64_t reserve64(GuestAddress address, std::uint64_t& value) override;
  bool store_conditional32(GuestAddress address, std::uint64_t token,
                           std::uint32_t value) override;
  bool store_conditional64(GuestAddress address, std::uint64_t token,
                           std::uint64_t value) override;

  void barrier(BarrierKind kind) override;
  void zero_cache_block(GuestAddress address, std::uint32_t bytes) override;
  void instruction_cache_invalidate(GuestAddress address) override;

  [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return bytes_; }
  [[nodiscard]] std::vector<std::uint8_t>& data() noexcept { return bytes_; }

 private:
  [[nodiscard]] std::size_t offset(GuestAddress address, std::size_t width) const;
  void touched();

  GuestAddress base_{};
  std::vector<std::uint8_t> bytes_{};
  std::uint64_t generation_{1};
};

}  // namespace xenon::cpu

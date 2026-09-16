#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace xenon::gpu {

class Edram {
 public:
  static constexpr std::uint32_t kTileCount = 2048;
  static constexpr std::uint32_t kTileBytes = 5120;
  static constexpr std::uint32_t kSize = kTileCount * kTileBytes;  // 10 MiB.

  Edram();

  void reset() noexcept;

  [[nodiscard]] std::uint32_t wrap_address(std::uint64_t address) const noexcept {
    return static_cast<std::uint32_t>(address % kSize);
  }
  [[nodiscard]] std::uint32_t tile_address(std::uint32_t tile) const noexcept {
    return (tile % kTileCount) * kTileBytes;
  }

  [[nodiscard]] std::uint8_t read8(std::uint64_t address) const noexcept;
  void write8(std::uint64_t address, std::uint8_t value) noexcept;
  void read(std::uint64_t address, std::span<std::byte> out) const noexcept;
  void write(std::uint64_t address, std::span<const std::byte> data) noexcept;
  void clear(std::uint64_t address, std::uint32_t length,
             std::uint8_t value = 0) noexcept;

  [[nodiscard]] std::span<std::byte> bytes() noexcept { return bytes_; }
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }

 private:
  std::vector<std::byte> bytes_;
};

}  // namespace xenon::gpu

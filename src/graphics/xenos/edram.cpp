#include "xenon/gpu/edram.hpp"

#include <algorithm>

namespace xenon::gpu {

Edram::Edram() : bytes_(kSize) {}

void Edram::reset() noexcept {
  std::fill(bytes_.begin(), bytes_.end(), std::byte{0});
}

std::uint8_t Edram::read8(std::uint64_t address) const noexcept {
  return std::to_integer<std::uint8_t>(bytes_[wrap_address(address)]);
}

void Edram::write8(std::uint64_t address, std::uint8_t value) noexcept {
  bytes_[wrap_address(address)] = static_cast<std::byte>(value);
}

void Edram::read(std::uint64_t address, std::span<std::byte> out) const noexcept {
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = bytes_[wrap_address(address + i)];
  }
}

void Edram::write(std::uint64_t address, std::span<const std::byte> data) noexcept {
  for (std::size_t i = 0; i < data.size(); ++i) {
    bytes_[wrap_address(address + i)] = data[i];
  }
}

void Edram::clear(std::uint64_t address, std::uint32_t length,
                  std::uint8_t value) noexcept {
  for (std::uint32_t i = 0; i < length; ++i) {
    bytes_[wrap_address(address + i)] = static_cast<std::byte>(value);
  }
}

}  // namespace xenon::gpu

#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace xenon::cpu {

using GuestAddress = std::uint32_t;

struct alignas(16) Vector128 {
  // Canonical architectural byte order: bytes[0] is VMX byte element 0.
  // Host SIMD backends may freely transform this at register-allocation time,
  // but architectural state and debugger/save-state surfaces use this order.
  std::array<std::uint8_t, 16> bytes{};

  [[nodiscard]] std::uint8_t u8(std::size_t i) const noexcept { return bytes[i & 15u]; }
  void set_u8(std::size_t i, std::uint8_t v) noexcept { bytes[i & 15u] = v; }

  [[nodiscard]] std::uint16_t u16_be(std::size_t lane) const noexcept {
    const std::size_t p=(lane & 7u)*2u;
    return static_cast<std::uint16_t>((std::uint16_t(bytes[p])<<8)|bytes[p+1]);
  }
  [[nodiscard]] std::uint32_t u32_be(std::size_t lane) const noexcept {
    const std::size_t p=(lane & 3u)*4u;
    return (std::uint32_t(bytes[p])<<24)|(std::uint32_t(bytes[p+1])<<16)|
           (std::uint32_t(bytes[p+2])<<8)|std::uint32_t(bytes[p+3]);
  }
  void set_u16_be(std::size_t lane, std::uint16_t v) noexcept {
    const std::size_t p=(lane & 7u)*2u; bytes[p]=std::uint8_t(v>>8); bytes[p+1]=std::uint8_t(v);
  }
  void set_u32_be(std::size_t lane, std::uint32_t v) noexcept {
    const std::size_t p=(lane & 3u)*4u; bytes[p]=std::uint8_t(v>>24); bytes[p+1]=std::uint8_t(v>>16);
    bytes[p+2]=std::uint8_t(v>>8); bytes[p+3]=std::uint8_t(v);
  }
};
static_assert(sizeof(Vector128) == 16);
static_assert(alignof(Vector128) == 16);

template <typename T>
[[nodiscard]] constexpr T byte_swap(T value) noexcept {
  static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
  using U = std::make_unsigned_t<T>;
  U v = static_cast<U>(value);
  if constexpr (sizeof(U) == 1) {
    return value;
  } else if constexpr (sizeof(U) == 2) {
    v = static_cast<U>((v >> 8) | (v << 8));
  } else if constexpr (sizeof(U) == 4) {
    v = ((v & 0x000000FFu) << 24) |
        ((v & 0x0000FF00u) << 8) |
        ((v & 0x00FF0000u) >> 8) |
        ((v & 0xFF000000u) >> 24);
  } else if constexpr (sizeof(U) == 8) {
    v = ((v & 0x00000000000000FFull) << 56) |
        ((v & 0x000000000000FF00ull) << 40) |
        ((v & 0x0000000000FF0000ull) << 24) |
        ((v & 0x00000000FF000000ull) << 8) |
        ((v & 0x000000FF00000000ull) >> 8) |
        ((v & 0x0000FF0000000000ull) >> 24) |
        ((v & 0x00FF000000000000ull) >> 40) |
        ((v & 0xFF00000000000000ull) >> 56);
  }
  return static_cast<T>(v);
}

template <unsigned Bits, typename T>
[[nodiscard]] constexpr std::int64_t sign_extend(T value) noexcept {
  static_assert(Bits > 0 && Bits <= 64);
  const std::uint64_t raw = static_cast<std::uint64_t>(value);
  const std::uint64_t sign = std::uint64_t{1} << (Bits - 1);
  return static_cast<std::int64_t>((raw ^ sign) - sign);
}

[[nodiscard]] constexpr std::uint64_t rotate_left64(std::uint64_t v, unsigned s) noexcept {
  return std::rotl(v, static_cast<int>(s & 63));
}

[[nodiscard]] constexpr std::uint32_t rotate_left32(std::uint32_t v, unsigned s) noexcept {
  return std::rotl(v, static_cast<int>(s & 31));
}

}  // namespace xenon::cpu

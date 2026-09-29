#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "xenon/cpu/memory_port.hpp"
#include "xenon/cpu/state.hpp"

namespace xenon::xbox::format {

// The Xbox 360 kernel's printf-family formatter, implemented as native host code
// over guest arguments (no PowerPC is interpreted). It follows the documented
// Windows CRT printf/wprintf format-specification syntax, with the observable
// Xbox behaviours: %p prints eight uppercase hex digits, %n stores the running
// count through a guest pointer, a NULL %s prints "(null)", and %s/%c width is
// selected by the function (printf vs wprintf) inverted by S/C and forced by the
// h/l/w size prefixes.

// Source of variadic arguments. Every integer, pointer and floating-point
// argument occupies one 64-bit slot; a double is read as the slot's raw bits
// (the caller has stored it in the integer argument area for varargs).
class ArgumentSource {
 public:
  virtual ~ArgumentSource() = default;
  [[nodiscard]] virtual std::uint64_t next64() = 0;
  [[nodiscard]] std::uint32_t next32() { return static_cast<std::uint32_t>(next64()); }
};

// Arguments as a variadic call passes them: argument index 0..7 live in
// r3..r10, index 8 and above in 8-byte big-endian stack slots at
// r1 + 0x54 + 8 * (index - 8). `first_index` is the index of the first
// variadic argument (i.e. the number of fixed parameters).
class RegisterArgumentSource final : public ArgumentSource {
 public:
  RegisterArgumentSource(const cpu::CpuState& cpu, cpu::MemoryPort& memory,
                         std::uint32_t first_index)
      : cpu_(cpu), memory_(memory), index_(first_index) {}
  [[nodiscard]] std::uint64_t next64() override;

 private:
  const cpu::CpuState& cpu_;
  cpu::MemoryPort& memory_;
  std::uint32_t index_;
};

// Arguments as a `va_list` passes them to the v* variants: a guest pointer to an
// array of consecutive 8-byte big-endian slots.
class ArrayArgumentSource final : public ArgumentSource {
 public:
  ArrayArgumentSource(cpu::MemoryPort& memory, cpu::GuestAddress array)
      : memory_(memory), array_(array) {}
  [[nodiscard]] std::uint64_t next64() override;

 private:
  cpu::MemoryPort& memory_;
  cpu::GuestAddress array_;
  std::uint32_t index_{};
};

struct Result {
  // Characters produced, or -1 if the format is malformed or an output
  // character cannot be represented (a narrow function given a wide character
  // above 0xFF, or an unknown conversion).
  std::int32_t count{};
  // The produced text. Narrow functions hold one byte per char16_t (<= 0xFF).
  std::u16string text;
};

// Formats `format` (already read from guest memory) against `args`. `wide` is
// true for the wprintf family and only selects the default width of %s/%c.
// `memory` is used for %s/%S dereferences and %n stores.
[[nodiscard]] Result format(cpu::MemoryPort& memory, std::u16string_view format,
                            ArgumentSource& args, bool wide);

// Reads a NUL-terminated guest string. Narrow strings are one byte per char
// (Latin-1, so the result round-trips the raw bytes); wide strings are
// big-endian UTF-16. Reading stops at `limit` characters.
[[nodiscard]] std::u16string read_guest_string(cpu::MemoryPort& memory, cpu::GuestAddress address,
                                               bool wide, std::size_t limit = 1u << 20);

}  // namespace xenon::xbox::format

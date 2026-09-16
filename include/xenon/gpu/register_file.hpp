#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace xenon::gpu {

class RegisterFile {
 public:
  // The highest public Xenos register indices extend slightly beyond 0x5000.
  static constexpr std::size_t kRegisterCount = 0x5003;

  void reset() noexcept;
  [[nodiscard]] bool write(std::uint32_t index, std::uint32_t value) noexcept;
  [[nodiscard]] std::uint32_t read(std::uint32_t index) const noexcept;
  [[nodiscard]] bool valid(std::uint32_t index) const noexcept {
    return index < kRegisterCount;
  }
  [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
  [[nodiscard]] std::span<const std::uint32_t> values() const noexcept {
    return values_;
  }

 private:
  std::array<std::uint32_t, kRegisterCount> values_{};
  std::uint64_t generation_{};
};

}  // namespace xenon::gpu

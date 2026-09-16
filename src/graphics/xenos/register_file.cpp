#include "xenon/gpu/register_file.hpp"

#include <algorithm>

namespace xenon::gpu {

void RegisterFile::reset() noexcept {
  values_.fill(0);
  generation_ = 0;
}

bool RegisterFile::write(std::uint32_t index, std::uint32_t value) noexcept {
  if (!valid(index)) return false;
  values_[index] = value;
  ++generation_;
  return true;
}

std::uint32_t RegisterFile::read(std::uint32_t index) const noexcept {
  return valid(index) ? values_[index] : 0;
}

}  // namespace xenon::gpu

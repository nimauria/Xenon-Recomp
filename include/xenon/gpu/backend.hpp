#pragma once

#include "xenon/gpu/edram.hpp"
#include "xenon/gpu/ir.hpp"
#include "xenon/gpu/register_file.hpp"
#include "xenon/memory/address_space.hpp"

namespace xenon::gpu {

// Host graphics backends consume Xenon graphics IR, never PM4 directly. Vulkan
// and D3D12 implementations therefore remain replaceable without changing the
// Xenos frontend or Project Gracemeria.
class Backend {
 public:
  virtual ~Backend() = default;
  virtual void begin_submission(memory::AddressSpace& memory,
                                Edram& edram) = 0;
  virtual void consume(const ir::Command& command) = 0;
  virtual void end_submission() = 0;
};

class NullBackend final : public Backend {
 public:
  void begin_submission(memory::AddressSpace&, Edram&) override {
    command_count_ = 0;
  }
  void consume(const ir::Command&) override { ++command_count_; }
  void end_submission() override {}
  [[nodiscard]] std::size_t command_count() const noexcept { return command_count_; }

 private:
  std::size_t command_count_{};
};

}  // namespace xenon::gpu

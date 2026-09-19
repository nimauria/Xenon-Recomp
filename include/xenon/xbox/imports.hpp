#pragma once

#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "xenon/cpu/state.hpp"
#include "xenon/kernel/xbox_io_guest.hpp"
#include "xenon/memory/address_space.hpp"

namespace xenon::xbox {

struct ImportCallContext {
  cpu::CpuState& cpu;
  memory::AddressSpace& memory;
  kernel::xbox::GuestIoBridge& io;
};

using ImportThunk = void (*)(ImportCallContext& context);

struct ImportDescriptor {
  std::string module;
  std::string name;
  std::uint16_t ordinal{};
  ImportThunk thunk{};
};

class ImportRegistry {
 public:
  [[nodiscard]] bool register_import(ImportDescriptor descriptor);
  [[nodiscard]] const ImportDescriptor* resolve(std::string_view module,
                                                std::string_view name) const;
  [[nodiscard]] const ImportDescriptor* resolve(std::string_view module,
                                                std::uint16_t ordinal) const;
  [[nodiscard]] bool invoke(std::string_view module, std::string_view name,
                            ImportCallContext& context) const;
  [[nodiscard]] bool invoke(std::string_view module, std::uint16_t ordinal,
                            ImportCallContext& context) const;
  [[nodiscard]] std::vector<ImportDescriptor> enumerate(
      std::string_view module = {}) const;

 private:
  [[nodiscard]] static std::string normalize_module(std::string_view module);
  [[nodiscard]] static bool same_name(std::string_view a,
                                      std::string_view b) noexcept;

  mutable std::shared_mutex mutex_;
  std::vector<ImportDescriptor> imports_;
};

// Registers the xboxkrnl filesystem/import surface implemented by the
// Memory-v2-backed GuestIoBridge. Safe to call repeatedly on the same registry.
[[nodiscard]] bool register_xboxkrnl_io_imports(ImportRegistry& registry);

// PPC64/Xbox 360 ABI helper used by import thunks. Integer parameters 0..7 are
// r3..r10. Additional integer parameters occupy 8-byte argument slots beginning
// at r1+0x54; the 32-bit Xbox argument is stored big-endian at the slot start.
class PpcArgumentReader {
 public:
  PpcArgumentReader(cpu::CpuState& cpu, memory::AddressSpace& memory)
      : cpu_(cpu), memory_(memory) {}

  [[nodiscard]] std::optional<std::uint32_t> u32(std::size_t index) const noexcept;
  void set_u32_result(std::uint32_t value) noexcept { cpu_.gpr[3] = value; }

 private:
  cpu::CpuState& cpu_;
  memory::AddressSpace& memory_;
};

}  // namespace xenon::xbox

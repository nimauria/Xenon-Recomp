#pragma once

#include <span>
#include <string>
#include <string_view>

#include "xenon/cpu/ir.hpp"

namespace xenon::cpu::backend {

// Explicit static-AOT link supplied by function analysis/module activation.
// Merely having a constant guest target is not sufficient: callers only add a
// binding after proving the target is a safe native translation to call
// directly. Noncanonical returns are still propagated at runtime.
struct DirectCallBinding {
  GuestAddress guest_target{};
  std::string native_symbol{};
};

// Stable generated symbol used for a dispatchable alternate guest entry into
// a canonical compiled function/region. Kept in the backend API so registry
// generation and C++ emission cannot drift to different naming conventions.
[[nodiscard]] std::string alternate_entry_symbol(std::string_view function_name,
                                                 GuestAddress entry);

// Portable native AOT backend. It emits C++20 containing no guest decoder or
// PPC runtime dispatch. The host C++ compiler performs final x86-64/ARM64
// instruction selection and register allocation.
class CppAotBackend {
 public:
  [[nodiscard]] std::string emit_function(const ir::Block& block,
                                          std::string_view function_name) const;
  [[nodiscard]] std::string emit_translation_unit(const ir::Block& block,
                                                   std::string_view function_name) const;

  // Whole-function AOT path. Local direct branches are emitted as native host
  // control flow rather than returning to an instruction dispatcher.
  [[nodiscard]] std::string emit_function(
      const ir::Function& function, std::string_view function_name,
      std::span<const DirectCallBinding> direct_calls = {},
      std::span<const GuestAddress> alternate_entries = {}) const;
  [[nodiscard]] std::string emit_translation_unit(
      const ir::Function& function, std::string_view function_name,
      std::span<const DirectCallBinding> direct_calls = {},
      std::span<const GuestAddress> alternate_entries = {}) const;
};

}  // namespace xenon::cpu::backend

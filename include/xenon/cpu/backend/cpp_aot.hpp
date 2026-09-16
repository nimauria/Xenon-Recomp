#pragma once

#include <string>
#include <string_view>

#include "xenon/cpu/ir.hpp"

namespace xenon::cpu::backend {

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
  [[nodiscard]] std::string emit_function(const ir::Function& function,
                                          std::string_view function_name) const;
  [[nodiscard]] std::string emit_translation_unit(const ir::Function& function,
                                                   std::string_view function_name) const;
};

}  // namespace xenon::cpu::backend

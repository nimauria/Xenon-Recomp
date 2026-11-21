// Generates a guest function that saves its nonvolatile registers through the
// shared __savegprlr_28 helper, clobbers them, and leaves through a tail
// branch to __restgprlr_28 - the Xenon toolchain's prologue/epilogue shape -
// with both helpers bound directly to Xenon's native templates, for
// x86_64/register_helpers.cpp. Also records whether the emitted code calls
// them directly (no registry lookup, no Branch handed back to the caller).

#include <cstdint>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/function_compiler.hpp"

using namespace xenon::cpu;

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  constexpr GuestAddress kBase = 0x10000u;
  constexpr GuestAddress kSave = 0x30000u;
  constexpr GuestAddress kRestore = 0x30100u;
  const std::vector<std::uint32_t> words = {
      0x7D8802A6u,  // 0x10000 mflr r12
      0x4801FFFDu,  // 0x10004 bl __savegprlr_28 (0x30000)
      0x3B800000u,  // 0x10008 li r28,0
      0x3BA00000u,  // 0x1000C li r29,0
      0x3BC00000u,  // 0x10010 li r30,0
      0x3BE00000u,  // 0x10014 li r31,0
      0x480200E8u,  // 0x10018 b __restgprlr_28 (0x30100)
  };
  StaticFunctionCompiler compiler;
  const FunctionCodeRange range{kBase, words};
  auto result = compiler.compile_ranges(kBase, std::span(&range, 1u), {}, {});
  if (!result.ok) {
    std::cerr << "compile failed: " << result.error << "\n";
    return 3;
  }
  const std::vector<backend::DirectCallBinding> bindings = {
      {kSave, "xenon::recomp::runtime_helpers::save_gpr_lr_v2<28>",
       "xenon/recomp/runtime_helpers.hpp", true},
      {kRestore, "xenon::recomp::runtime_helpers::restore_gpr_lr_v2<28>",
       "xenon/recomp/runtime_helpers.hpp", true},
  };
  const auto source = backend::CppAotBackend{}.emit_function(result.function, "helper_frame", bindings);
  const bool direct_save =
      source.find("xenon::recomp::runtime_helpers::save_gpr_lr_v2<28>(context)") != std::string::npos;
  // A direct tail call: the helper is called here (its result returned,
  // after any promoted registers are reloaded from what it restored), and
  // the tail branch is not handed back to the caller as a Branch result.
  const bool direct_restore =
      source.find("xenon::recomp::runtime_helpers::restore_gpr_lr_v2<28>(context);") !=
          std::string::npos &&
      source.find("FlowReason::Branch," + std::to_string(kRestore) + "u") == std::string::npos;
  const bool any_lookup = source.find("lookup_compiled") != std::string::npos;

  // A tail branch to a routine that is not a leaf runtime helper stays a
  // Branch result for the caller's loop (guest functions tail-branching to
  // each other in a cycle must not recurse on the host stack).
  {
    const std::vector<backend::DirectCallBinding> plain = {{kRestore, "plain_restore_v2"}};
    const auto plain_source = backend::CppAotBackend{}.emit_function(result.function, "plain", plain);
    if (plain_source.find("return {FlowReason::Branch," + std::to_string(kRestore) + "u,0};") ==
            std::string::npos ||
        plain_source.find("return plain_restore_v2(context);") != std::string::npos) {
      std::cerr << "a tail branch to a non-leaf binding was called directly\n";
      return 5;
    }
  }
  // Malformed runtime bindings are refused.
  const auto rejected = [&](backend::DirectCallBinding binding) {
    try {
      (void)backend::CppAotBackend{}.emit_function(result.function, "bad",
                                                    std::span(&binding, 1u));
    } catch (const std::invalid_argument&) {
      return true;
    }
    return false;
  };
  if (!rejected({kSave, "ns::helper<x>", "xenon/recomp/runtime_helpers.hpp", true}) ||
      !rejected({kSave, "ns::helper<1>", "bad\"header.hpp", true}) ||
      !rejected({kSave, "leaf_without_header_v2", "", true}) ||
      !rejected({kSave, "ns::not_a_plain_symbol", "", false})) {
    std::cerr << "a malformed runtime binding was accepted\n";
    return 6;
  }
  std::ofstream out(argv[1]);
  if (!out) return 4;
  out << "#include \"xenon/cpu/aot_semantics.hpp\"\nusing namespace xenon::cpu;\n" << source << "\n";
  out << "extern const bool kDirectSave = " << (direct_save ? "true" : "false") << ";\n";
  out << "extern const bool kDirectRestore = " << (direct_restore ? "true" : "false") << ";\n";
  out << "extern const bool kAnyLookup = " << (any_lookup ? "true" : "false") << ";\n";
  return 0;
}

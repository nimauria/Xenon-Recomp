#include <algorithm>
#include <fstream>
#include <sstream>

#include "recomp/compilation/project_generation_internal.hpp"
#include "recomp/driver/driver_support.hpp"
#include "xenon/cpu/backend/cpp_aot.hpp"

namespace xenon::recomp::detail {

std::string runtime_helper_native_symbol(const analysis::RuntimeHelper& helper) {
  switch (helper.kind) {
    case analysis::RuntimeHelperKind::SetJmp: return "setjmp_v2";
    case analysis::RuntimeHelperKind::LongJmp: return "longjmp_v2";
    case analysis::RuntimeHelperKind::SaveGprLr:
      return "save_gpr_lr_v2<" + std::to_string(helper.register_start.value_or(14u)) + ">";
    case analysis::RuntimeHelperKind::RestoreGprLr:
      return "restore_gpr_lr_v2<" + std::to_string(helper.register_start.value_or(14u)) + ">";
    case analysis::RuntimeHelperKind::SaveFpr:
      return "save_fpr_v2<" + std::to_string(helper.register_start.value_or(14u)) + ">";
    case analysis::RuntimeHelperKind::RestoreFpr:
      return "restore_fpr_v2<" + std::to_string(helper.register_start.value_or(14u)) + ">";
    case analysis::RuntimeHelperKind::SaveVmx:
      return "save_vmx_v2<" + std::to_string(helper.register_start.value_or(14u)) + ">";
    case analysis::RuntimeHelperKind::RestoreVmx:
      return "restore_vmx_v2<" + std::to_string(helper.register_start.value_or(14u)) + ">";
    case analysis::RuntimeHelperKind::SaveVmx128:
      return "save_vmx128_v2<" + std::to_string(helper.register_start.value_or(64u)) + ">";
    case analysis::RuntimeHelperKind::RestoreVmx128:
      return "restore_vmx128_v2<" + std::to_string(helper.register_start.value_or(64u)) + ">";
  }
  return "setjmp_v2";
}

const char* native_replacement_function_name(analysis::NativeReplacementKind kind) {
  using analysis::NativeReplacementKind;
  switch (kind) {
    case NativeReplacementKind::Memcpy: return "memcpy_v2";
    case NativeReplacementKind::Memmove: return "memmove_v2";
    case NativeReplacementKind::Memset: return "memset_v2";
    case NativeReplacementKind::MemcpyChecked: return "memcpy_checked_v2";
    case NativeReplacementKind::MemmoveChecked: return "memmove_checked_v2";
    case NativeReplacementKind::Memcmp: return "memcmp_v2";
    case NativeReplacementKind::Strlen: return "strlen_v2";
    case NativeReplacementKind::Strncmp: return "strncmp_v2";
    case NativeReplacementKind::Strncpy: return "strncpy_v2";
    case NativeReplacementKind::Strchr: return "strchr_v2";
    case NativeReplacementKind::Strstr: return "strstr_v2";
    case NativeReplacementKind::Strrchr: return "strrchr_v2";
    case NativeReplacementKind::StrcpyChecked: return "strcpy_checked_v2";
    case NativeReplacementKind::HeapAllocate: return "heap_allocate_v2";
    case NativeReplacementKind::HeapFree: return "heap_free_v2";
    case NativeReplacementKind::HeapSize: return "heap_size_v2";
    case NativeReplacementKind::HeapReAllocate: return "heap_reallocate_v2";
    default: return nullptr;
  }
}

bool write_registry_header(const std::filesystem::path& generation_stage) {
  std::ofstream registry_header(generation_stage / "registry.hpp");
  registry_header << "#pragma once\n#include <cstdint>\n#include \"xenon/cpu/runtime.hpp\"\nnamespace xenon::recomp {\n"
                     "void bind_compiled_registry(xenon::cpu::ExecutionContext&) noexcept;\n"
                     "xenon::cpu::NativeCompiledEntry lookup_compiled(void*, xenon::cpu::ExecutionContext&, xenon::cpu::GuestAddress, xenon::cpu::CompiledLookupKind);\n"
                     // Part 1.9: setjmp/longjmp RuntimeHelper metadata, carried through to a
                     // real generated symbol a runtime/codegen consumer can read - see
                     // metadata.cpp for the values and docs/recomp/ANALYSIS_HINT_SCHEMA_V2.md for
                     // what actually trapping/restoring at these addresses still requires
                     // (a CPU V2 change out of scope for this pass).
                     "extern const bool kHasSetJmpAddress;\n"
                     "extern const std::uint32_t kSetJmpAddress;\n"
                     "extern const bool kHasLongJmpAddress;\n"
                     "extern const std::uint32_t kLongJmpAddress;\n"
                     "}\n";
  registry_header.close();
  return static_cast<bool>(registry_header);
}

bool write_registry_source(const std::filesystem::path& generation_stage, const AnalysisReport& report) {
  std::ostringstream registry_text;
  const bool has_native_replacements = std::any_of(
      report.functions.begin(), report.functions.end(),
      [](const auto& function) { return function.native_replacement.has_value(); });
  const bool has_runtime_helpers = report.hint_set_v2 &&
      !report.hint_set_v2->runtime_helpers.empty();
  registry_text << "// Generated compiled-function registry.\n#include \"registry.hpp\"\n"
                   "#include <cstddef>\n#include <cstdint>\n";
  if (has_runtime_helpers) {
    registry_text << "#include \"xenon/recomp/runtime_helpers.hpp\"\n";
  }
  if (has_native_replacements) {
    // A NativeReplacement hint (Part 1.10) means Xenon owns the
    // implementation at this address - the lookup_compiled() switch below
    // calls straight into xenon_recomp's own native_replacements.cpp
    // instead of a generated shard function.
    registry_text << "#include \"xenon/recomp/native_replacements.hpp\"\n";
  }
  // Function shards (functions/shard_*.cpp) define each compiled function at
  // global scope (see backend_cpp_aot.cpp's codegen - `using namespace
  // xenon::cpu;` then a bare `ExecutionResult <name>(...)`), so these
  // forward declarations must also be at global scope to name the same
  // symbol. Declaring them inside `namespace xenon::recomp` here was a real,
  // previously-undetected bug: a static-library-only build (xenon_game)
  // never needs to actually resolve these references (archiving doesn't
  // link), so the mismatched-namespace declarations silently built an
  // *unrelated*, always-undefined xenon::recomp::<name> symbol that only a
  // real caller linking a full executable/shared module would ever notice.
  // Native-replacement entries are excluded from every loop below: they have
  // no generated shard function to declare/reference, only a
  // lookup_compiled() case pointing at Xenon's own implementation.
  for (const auto& function : report.functions)
    if (function.compiled && !function.native_replacement)
      registry_text << "extern xenon::cpu::ExecutionResult " << function.name
                    << "(xenon::cpu::CpuState&, xenon::cpu::MemoryPort&, xenon::cpu::RuntimeServices&);\n";
  for (const auto& function : report.functions)
    if (function.compiled && !function.native_replacement)
      registry_text << "extern xenon::cpu::ExecutionResult " << function.name
                    << "_v2(xenon::cpu::ExecutionContext&);\n";
  for (const auto& entry : report.entries) {
    if (entry.kind != GuestEntryKind::AlternateBlock) continue;
    const auto owner = std::find_if(report.functions.begin(), report.functions.end(), [&](const auto& function) {
      return function.guest_start == entry.owner_function && function.compiled &&
             !function.native_replacement;
    });
    if (owner != report.functions.end())
      registry_text << "extern xenon::cpu::ExecutionResult "
                    << cpu::backend::alternate_entry_symbol(owner->name, entry.address)
                    << "(xenon::cpu::ExecutionContext&);\n";
  }
  const auto legacy_compiled_count = std::count_if(
      report.functions.begin(), report.functions.end(),
      [](const auto& function) { return function.compiled && !function.native_replacement; });
  registry_text << "namespace xenon::recomp {\n"
                   "using CompiledFunction = xenon::cpu::ExecutionResult (*)(xenon::cpu::CpuState&, xenon::cpu::MemoryPort&, xenon::cpu::RuntimeServices&);\n"
                   "using CompiledFunctionV2 = xenon::cpu::NativeCompiledEntry;\n"
                   "struct CompiledEntry { std::uint32_t guest_start; CompiledFunction function; };\n"
                << "const CompiledEntry kCompiledFunctions["
                << std::max<std::size_t>(1u, static_cast<std::size_t>(legacy_compiled_count))
                << "] = {\n";
  // registry_text is a single ostringstream shared by every section below
  // (the compiled-function table, the lookup_compiled() switch, and the
  // trailing kCompiledFunctionCount declaration). std::hex/std::dec are
  // *sticky* stream-formatting state, not per-call flags: a previous bug
  // here applied std::hex to emit each guest_start address in the loop
  // below, then wrote kCompiledFunctionCount immediately after with no
  // reset, so the count itself was emitted as an invalid (or, for a count
  // like 16 -> "10", silently wrong-but-valid) hex literal. Every guest
  // address destined for this stream is now formatted through hex_string(),
  // which uses its own throwaway ostringstream, so registry_text itself
  // never enters hex mode and no later decimal write can inherit stale
  // formatting state.
  for (const auto& function : report.functions)
    if (function.compiled && !function.native_replacement)
      registry_text << "  {0x" << hex_string(function.guest_start) << ", &" << function.name << "},\n";
  registry_text << "};\nconst std::size_t kCompiledFunctionCount = " << legacy_compiled_count << ";\n}\n";
  registry_text << "namespace xenon::recomp {\n"
                   "CompiledFunctionV2 lookup_compiled(void*, xenon::cpu::ExecutionContext&, xenon::cpu::GuestAddress target, xenon::cpu::CompiledLookupKind) {\n"
                   "  switch (target) {\n";
  if (report.hint_set_v2) {
    for (const auto& helper :
         analysis::expand_runtime_helpers(report.hint_set_v2->runtime_helpers)) {
      registry_text << "    case 0x" << hex_string(helper.address)
                    << ": return &xenon::recomp::runtime_helpers::"
                    << runtime_helper_native_symbol(helper) << ";\n";
    }
  }
  for (const auto& function : report.functions) {
    if (function.native_replacement) {
      const auto* native_name = native_replacement_function_name(*function.native_replacement);
      if (native_name != nullptr) {
        registry_text << "    case 0x" << hex_string(function.guest_start)
                      << ": return &xenon::recomp::native_replacements::" << native_name << ";\n";
      }
      continue;
    }
    if (function.compiled)
      registry_text << "    case 0x" << hex_string(function.guest_start) << ": return &" << function.name << "_v2;\n";
  }
  for (const auto& entry : report.entries) {
    if (entry.kind != GuestEntryKind::AlternateBlock) continue;
    const auto owner = std::find_if(report.functions.begin(), report.functions.end(), [&](const auto& function) {
      return function.guest_start == entry.owner_function && function.compiled &&
             !function.native_replacement;
    });
    if (owner == report.functions.end()) continue;
    registry_text << "    case 0x" << hex_string(entry.address) << ": return &"
                  << cpu::backend::alternate_entry_symbol(owner->name, entry.address) << ";\n";
  }
  registry_text << "    default: return nullptr;\n  }\n}\n"
                   "void bind_compiled_registry(xenon::cpu::ExecutionContext& context) noexcept {\n"
                   "  context.compiled_registry = nullptr;\n"
                   "  context.compiled_lookup = &lookup_compiled;\n"
                   "}\n}\n";
  std::ofstream registry(generation_stage / "registry.cpp");
  registry << registry_text.str();
  registry.close();
  return static_cast<bool>(registry);
}

}  // namespace xenon::recomp::detail

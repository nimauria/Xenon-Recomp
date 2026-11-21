// Generates register save/restore helper scenarios twice for
// x86_64/register_spills.cpp: the helper-call version (plain, bindings
// without Spill info, so the helpers are called) and the inlined version
// (promoted, bindings with Spill info, so the spill's loads and stores are
// emitted on the locals). The runner runs both from identical state and
// memory and requires identical outcomes. Here, the promoted source must
// contain no call to the helper symbol at all.

#include <cstdint>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/function_compiler.hpp"
#include "xenon/recomp/runtime_helpers.hpp"

using namespace xenon::cpu;
namespace helpers = xenon::recomp::runtime_helpers;
using Spill = backend::DirectCallBinding::Spill;

namespace {

enum Family { kGpr = 0, kFpr = 1, kVmx = 2, kVmx128 = 3 };
// Shape of the guest function around the helper.
enum Shape {
  kSave = 0,  // [mflr r12;] bl save; blr
  kRestore,   // b restore
  kFrame,     // mflr r12; bl save; li r<first>..r31; b restore
  kCondCall,  // mflr r12; beql save; blr          (GPR/LR only)
  kCondTail,  // beq restore                       (GPR/LR only)
};

struct Scenario {
  std::string name;
  Family family;
  Shape shape;
  unsigned first;
};

constexpr GuestAddress kSaveTarget = 0x30000u;
constexpr GuestAddress kRestoreTarget = 0x30100u;
constexpr GuestAddress kBase = 0x30800u;

std::uint32_t branch(GuestAddress from, GuestAddress to, bool link) {
  return 0x48000000u | (static_cast<std::uint32_t>(to - from) & 0x03FFFFFCu) | (link ? 1u : 0u);
}
std::uint32_t cond_branch(GuestAddress from, GuestAddress to, bool link) {
  return 0x41820000u | (static_cast<std::uint32_t>(to - from) & 0xFFFCu) | (link ? 1u : 0u);
}
constexpr std::uint32_t kMflrR12 = 0x7D8802A6u;
constexpr std::uint32_t kBlr = 0x4E800020u;
std::uint32_t li(unsigned reg, unsigned value) { return 0x38000000u | (reg << 21) | value; }

std::vector<std::uint32_t> words_for(const Scenario& s) {
  const bool gpr = s.family == kGpr;
  std::vector<std::uint32_t> w;
  GuestAddress at = kBase;
  const auto push = [&](std::uint32_t word) {
    w.push_back(word);
    at += 4u;
  };
  switch (s.shape) {
    case kSave:
      if (gpr) push(kMflrR12);
      push(branch(at, kSaveTarget, true));
      push(kBlr);
      break;
    case kRestore: push(branch(at, kRestoreTarget, false)); break;
    case kFrame:
      push(kMflrR12);
      push(branch(at, kSaveTarget, true));
      for (unsigned r = s.first; r <= 31u; ++r) push(li(r, 0x55u + r));
      push(branch(at, kRestoreTarget, false));
      break;
    case kCondCall:
      push(kMflrR12);
      push(cond_branch(at, kSaveTarget, true));
      push(kBlr);
      break;
    case kCondTail: push(cond_branch(at, kRestoreTarget, false)); break;
  }
  return w;
}

std::string symbol_for(Family f, bool save, unsigned first) {
  static const char* const names[2][4] = {
      {"restore_gpr_lr_v2", "restore_fpr_v2", "restore_vmx_v2", "restore_vmx128_v2"},
      {"save_gpr_lr_v2", "save_fpr_v2", "save_vmx_v2", "save_vmx128_v2"}};
  return std::string("xenon::recomp::runtime_helpers::") + names[save][f] + "<" +
         std::to_string(first) + ">";
}

backend::DirectCallBinding binding_for(const Scenario& s, bool save, bool with_spill) {
  backend::DirectCallBinding b{save ? kSaveTarget : kRestoreTarget,
                               symbol_for(s.family, save, s.first),
                               "xenon/recomp/runtime_helpers.hpp", true};
  if (!with_spill) return b;
  static const Spill spills[2][4] = {
      {Spill::RestoreGprLr, Spill::RestoreFpr, Spill::RestoreVmx, Spill::RestoreVmx128},
      {Spill::SaveGprLr, Spill::SaveFpr, Spill::SaveVmx, Spill::SaveVmx128}};
  b.spill = spills[save][s.family];
  b.spill_first = s.first;
  switch (s.family) {
    case kGpr:
      b.spill_offset = helpers::kGprLrBaseOffset;
      b.spill_link_offset = helpers::kGprLrLrOffset;
      break;
    case kFpr: b.spill_offset = helpers::kFprBaseOffset; break;
    case kVmx: b.spill_offset = helpers::kVmxBaseOffset; break;
    case kVmx128: b.spill_offset = helpers::kVmx128BaseOffset; break;
  }
  return b;
}

std::vector<Scenario> scenarios() {
  std::vector<Scenario> all;
  const auto add = [&](std::string name, Family f, Shape s, unsigned first) {
    all.push_back({std::move(name), f, s, first});
  };
  for (const unsigned n : {14u, 20u, 28u, 31u}) {
    const auto tag = std::to_string(n);
    add("gpr_save_" + tag, kGpr, kSave, n);
    add("gpr_restore_" + tag, kGpr, kRestore, n);
    add("gpr_frame_" + tag, kGpr, kFrame, n);
  }
  add("gpr_cond_call", kGpr, kCondCall, 25u);
  add("gpr_cond_tail", kGpr, kCondTail, 25u);
  for (const unsigned n : {14u, 18u, 31u}) {
    add("fpr_save_" + std::to_string(n), kFpr, kSave, n);
    add("fpr_restore_" + std::to_string(n), kFpr, kRestore, n);
  }
  for (const unsigned n : {14u, 22u, 31u}) {
    add("vmx_save_" + std::to_string(n), kVmx, kSave, n);
    add("vmx_restore_" + std::to_string(n), kVmx, kRestore, n);
  }
  for (const unsigned n : {64u, 100u, 127u}) {
    add("vmx128_save_" + std::to_string(n), kVmx128, kSave, n);
    add("vmx128_restore_" + std::to_string(n), kVmx128, kRestore, n);
  }
  return all;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  std::ofstream out(argv[1]);
  if (!out) return 3;
  const auto all = scenarios();
  out << "#include \"xenon/cpu/aot_semantics.hpp\"\nusing namespace xenon::cpu;\n";
  std::string table;
  std::string declarations;
  for (const auto& s : all) {
    const auto words = words_for(s);
    StaticFunctionCompiler compiler;
    const FunctionCodeRange range{kBase, words};
    auto result = compiler.compile_ranges(kBase, std::span(&range, 1u), {}, {});
    if (!result.ok) {
      std::cerr << "compile failed for " << s.name << ": " << result.error << "\n";
      return 4;
    }
    const bool saves = s.shape == kSave || s.shape == kFrame || s.shape == kCondCall;
    const bool restores = s.shape == kRestore || s.shape == kFrame || s.shape == kCondTail;
    for (const bool inlined : {false, true}) {
      std::vector<backend::DirectCallBinding> bindings;
      if (saves) bindings.push_back(binding_for(s, true, inlined));
      if (restores) bindings.push_back(binding_for(s, false, inlined));
      const auto name = s.name + (inlined ? "_inline" : "_call");
      const auto source =
          backend::CppAotBackend(inlined).emit_function(result.function, name, bindings);
      for (const auto& binding : bindings) {
        const bool calls = source.find(binding.native_symbol) != std::string::npos;
        if (inlined == calls) {
          std::cerr << s.name
                    << (inlined ? ": the inlined function still calls "
                                : ": the call version does not call ")
                    << binding.native_symbol << "\n";
          return 5;
        }
      }
      out << source << "\n";
    }
    table += "  {\"" + s.name + "\", &" + s.name + "_call_v2, &" + s.name + "_inline_v2, " +
             std::to_string(static_cast<int>(s.family)) + ", " +
             std::to_string(static_cast<int>(s.shape)) + ", " + std::to_string(s.first) + "u},\n";
  }
  out << "struct SpillCase {\n  const char* name;\n"
         "  ExecutionResult (*call)(ExecutionContext&);\n"
         "  ExecutionResult (*inlined)(ExecutionContext&);\n"
         "  int family;\n  int shape;\n  unsigned first;\n};\n";
  out << "extern const SpillCase kSpillCases[] = {\n" << table << "};\n";
  out << "extern const unsigned kSpillCaseCount = " << all.size() << "u;\n";
  return 0;
}

#pragma once

// Private header shared by the C++ AOT emitter's translation units in
// src/cpu/codegen/emission/. Every file in this directory is hashed into the
// generated-source cache identity (XENON_GRAPH_CODEGEN_PRODUCER).

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/decoder.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <optional>
#include <sstream>
#include <unordered_set>
#include <stdexcept>
#include <string>


namespace xenon::cpu::backend::emission {
using ir::Instruction;
using ir::Op;
using ir::Type;

inline std::string v(ir::ValueId id) { return "v" + std::to_string(id); }
const char* ctype(Type t);
inline std::string arg(const Instruction&i,unsigned n){if(n>=i.args.size())return "{}";return v(i.args[n]);}
inline std::string decl(const Instruction&i,const std::string&e){return std::string("  [[maybe_unused]] ")+ctype(i.type)+" "+v(i.result)+" = "+e+";\n";}

// Register promotion for the function being emitted (CppAotBackend's
// constructor). Set by emit_function(const ir::Function&) for the length of
// one function's emission on this thread; inactive for the single-block path.
struct RegisterPromotion {
  bool active{};
};
inline thread_local RegisterPromotion t_promotion{};

// The C++ lvalue holding a guest register: its local while promoted, else
// the CpuState field.
inline std::string gpr_ref(unsigned n) {
  return t_promotion.active ? "g" + std::to_string(n & 31u) : "state.gpr[" + std::to_string(n & 31u) + "]";
}
inline std::string fpr_ref(unsigned n) {
  return t_promotion.active ? "fp" + std::to_string(n & 31u) : "state.fpr_bits[" + std::to_string(n & 31u) + "]";
}
inline std::string vr_ref(unsigned n) {
  return t_promotion.active ? "vr" + std::to_string(n & 127u) : "state.vr[" + std::to_string(n & 127u) + "]";
}
// Around anything that reads or writes CpuState itself (calls, hooks,
// traps, system calls, string loads/stores, a taken safepoint): promoted
// registers are written back before it and reloaded after. Empty when not
// promoting.
inline const char* flush_registers() { return t_promotion.active ? "xenon_flush(); " : ""; }
inline const char* reload_registers() { return t_promotion.active ? "xenon_reload(); " : ""; }

// Emits the C++ statement(s) for one non-control-flow IR instruction
// (instruction_emission.cpp).
std::string emit_one(const Instruction&i, const std::vector<Type>& value_types);

}  // namespace xenon::cpu::backend::emission

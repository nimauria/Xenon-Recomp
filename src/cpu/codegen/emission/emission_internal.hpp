#pragma once

// Private header shared by the C++ AOT emitter's translation units in
// src/cpu/codegen/emission/. Every file in this directory is hashed into the
// generated-source cache identity (XENON_GRAPH_CODEGEN_PRODUCER).

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/decoder.hpp"

#include <algorithm>
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

// Emits the C++ statement(s) for one non-control-flow IR instruction
// (instruction_emission.cpp).
std::string emit_one(const Instruction&i, const std::vector<Type>& value_types);

}  // namespace xenon::cpu::backend::emission

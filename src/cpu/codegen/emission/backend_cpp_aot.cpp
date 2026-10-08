#include "xenon/cpu/backend/cpp_aot.hpp"

#include "cpu/codegen/emission/emission_internal.hpp"

namespace xenon::cpu::backend {

std::string alternate_entry_symbol(std::string_view function_name, GuestAddress entry) {
  std::ostringstream out;
  out << function_name << "_entry_" << std::hex << std::uppercase << entry << "_v2";
  return out.str();
}

namespace {
using namespace emission;

std::optional<std::uint64_t> constant_value(const ir::Block& block, ir::ValueId id) {
  for (const auto& insn : block.instructions) {
    if (insn.result == id && insn.op == Op::Constant) return insn.imm0;
  }
  return std::nullopt;
}

std::string local_label(GuestAddress address) {
  std::ostringstream o;
  o << "L_" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << address;
  return o.str();
}

bool has_guest_source(const Instruction& instruction) noexcept {
  return instruction.guest_opcode.valid();
}

void emit_guest_pc(std::ostringstream& out, const Instruction& instruction,
                   std::optional<GuestAddress>& current_guest,
                   std::optional<GuestAddress>& materialized_guest) {
  if (!has_guest_source(instruction)) return;
  current_guest = instruction.guest_address;
  const auto effect = ir::effects(instruction.op);
  const bool boundary = ir::has_effect(effect, ir::Effect::MayFault) ||
                        ir::has_effect(effect, ir::Effect::Call) ||
                        ir::has_effect(effect, ir::Effect::Trap) ||
                        ir::has_effect(effect, ir::Effect::ControlFlow) ||
                        ir::has_effect(effect, ir::Effect::Barrier) ||
                        ir::has_effect(effect, ir::Effect::Synchronization);
  if (!boundary || materialized_guest == current_guest) return;
  materialized_guest = current_guest;
  out << "  state.cia=" << *current_guest << "u;\n";
  out << "  state.nia=" << static_cast<GuestAddress>(*current_guest + 4u)
      << "u;\n";
}

void emit_exit_pc(std::ostringstream& out, std::optional<GuestAddress> current_guest,
                  std::optional<GuestAddress>& materialized_guest) {
  if (!current_guest || materialized_guest == current_guest) return;
  materialized_guest = current_guest;
  out << "  state.cia=" << *current_guest << "u;\n";
}

std::optional<GuestAddress> local_fallthrough(const ir::Block& block) {
  for (const auto& edge : block.successors) {
    if (edge.local && edge.kind == ir::EdgeKind::Fallthrough) return edge.target;
  }
  return std::nullopt;
}

bool is_guaranteed_terminal(const ir::Block& block) {
  for (const auto& insn : block.instructions) {
    if (insn.op == Op::Branch || insn.op == Op::Return || insn.op == Op::Syscall)
      return true;
    if (insn.op == Op::Barrier && insn.imm0 == 4u) return true;
    if ((insn.op == Op::BranchIf || insn.op == Op::BranchIndirect) &&
        insn.imm0 == 0u && !insn.args.empty()) {
      const auto condition = constant_value(block, insn.args[0]);
      if (condition && *condition != 0u) return true;
    }
  }
  return false;
}

bool valid_cpp_identifier(std::string_view symbol) {
  if (symbol.empty()) return false;
  const auto first = static_cast<unsigned char>(symbol.front());
  if (!(std::isalpha(first) || symbol.front() == '_')) return false;
  for (const char c : symbol) {
    const auto ch = static_cast<unsigned char>(c);
    if (!(std::isalnum(ch) || c == '_')) return false;
  }
  return true;
}

const DirectCallBinding* find_direct_call(
    GuestAddress target, std::span<const DirectCallBinding> direct_calls) {
  for (const auto& binding : direct_calls)
    if (binding.guest_target == target) return &binding;
  return nullptr;
}

// Every call site used to inline a switch over all of the function's local
// labels to resume a guest longjmp. That made generated code O(call sites x
// labels) - about a fifth of every shard - and gave each function a dense
// goto web that MSVC optimizes super-linearly. Call sites now hand the result
// to one shared per-function block (emitted by emit_function()) with the
// identical behaviour: local targets resume with cia/nia set, anything else
// is returned to the caller.
constexpr std::string_view kLongJumpLabel = "xenon_longjump";
constexpr std::string_view kLongJumpResult = "xenon_longjump_rr";
constexpr std::string_view kLocalBranchLabel = "xenon_local_branch";
constexpr std::string_view kLocalBranchTarget = "xenon_branch_target";

std::string is_local_symbol(std::string_view local_dispatch_symbol) {
  return std::string(local_dispatch_symbol) + "_is_local";
}

void emit_longjump_dispatch(std::ostringstream& o, std::string_view rr,
                            std::string_view indent = "  ") {
  o << indent << "if(" << rr << ".reason==FlowReason::LongJump) { " << kLongJumpResult
    << "=" << rr << "; goto " << kLongJumpLabel << "; }\n";
}

void emit_call_result_check(std::ostringstream& o, std::string_view rr,
                            GuestAddress expected_return,
                            std::string_view indent = "  ") {
  emit_longjump_dispatch(o, rr, indent);
  o << indent << "if(" << rr << ".reason==FlowReason::Return) { if("
    << rr << ".next_address!=" << expected_return << "u) return " << rr
    << "; } else if(" << rr << ".reason!=FlowReason::Fallthrough) return "
    << rr << ";\n";
}

std::string emit_one_in_function(const Instruction& i,
                                 const std::vector<Type>& value_types,
                                 const ir::Block& block,
                                 const std::unordered_set<GuestAddress>& local_targets,
                                 std::span<const DirectCallBinding> direct_calls,
                                 std::string_view local_dispatch_symbol) {
  std::ostringstream o;
  if (i.op == Op::Branch) {
    const auto target = i.args.empty() ? std::optional<std::uint64_t>{} : constant_value(block, i.args[0]);
    if (target) {
      const auto guest_target = static_cast<GuestAddress>(*target);
      o << "  state.nia=" << guest_target << "u;\n";
      if (local_targets.contains(guest_target)) {
        o << "  goto " << local_label(guest_target) << ";\n";
      } else {
        o << "  return {FlowReason::Branch," << guest_target << "u,0};\n";
      }
      return o.str();
    }
  }

  if (i.op == Op::Call && !i.args.empty()) {
    const auto target = constant_value(block, i.args[0]);
    if (target) {
      const auto guest_target = static_cast<GuestAddress>(*target);
      const auto expected_return = static_cast<GuestAddress>(i.guest_address + 4u);
      if (local_targets.contains(guest_target)) {
        o << "  { auto rr=" << local_dispatch_symbol << "(context," << guest_target << "u);\n";
        emit_call_result_check(o, "rr", expected_return, "    ");
        o << "  }\n";
        return o.str();
      }
      if (const auto* binding = find_direct_call(guest_target, direct_calls)) {
        o << "  { auto rr=" << binding->native_symbol << "(context);\n";
        emit_call_result_check(o, "rr", expected_return, "    ");
        o << "  }\n";
        return o.str();
      }
      o << "  { ExecutionResult rr{}; if(auto* native=context.lookup_compiled("
        << guest_target << "u,CompiledLookupKind::Call)) rr=native(context); "
        << "else { auto fallback=context.try_dynamic_fallback(" << guest_target
        << "u,CompiledLookupKind::Call); rr=fallback.handled ? fallback.result : runtime.call("
        << guest_target << "u,state,memory); }\n";
      emit_call_result_check(o, "rr", expected_return, "    ");
      o << "  }\n";
      return o.str();
    }
  }

  if (i.op == Op::BranchIf && i.imm0 == 1 && i.args.size() >= 2) {
    const auto target = constant_value(block, i.args[1]);
    if (target) {
      const auto guest_target = static_cast<GuestAddress>(*target);
      const auto expected_return = static_cast<GuestAddress>(i.guest_address + 4u);
      o << "  if(" << arg(i,0) << ") {\n";
      if (local_targets.contains(guest_target)) {
        o << "    auto rr=" << local_dispatch_symbol << "(context," << guest_target << "u);\n";
      } else if (const auto* binding = find_direct_call(guest_target, direct_calls)) {
        o << "    auto rr=" << binding->native_symbol << "(context);\n";
      } else {
        o << "    ExecutionResult rr{}; if(auto* native=context.lookup_compiled("
          << guest_target << "u,CompiledLookupKind::Call)) rr=native(context); "
          << "else { auto fallback=context.try_dynamic_fallback(" << guest_target
          << "u,CompiledLookupKind::Call); rr=fallback.handled ? fallback.result : runtime.call("
          << guest_target << "u,state,memory); }\n";
      }
      emit_call_result_check(o, "rr", expected_return, "    ");
      o << "  }\n";
      return o.str();
    }
  }

  if (i.op == Op::BranchIf && i.imm0 == 0 && i.args.size() >= 2) {
    const auto target = constant_value(block, i.args[1]);
    if (target) {
      const auto guest_target = static_cast<GuestAddress>(*target);
      o << "  if(" << arg(i,0) << ") { state.nia=" << guest_target << "u; ";
      if (local_targets.contains(guest_target)) {
        o << "goto " << local_label(guest_target) << "; }\n";
      } else {
        o << "return {FlowReason::Branch," << guest_target << "u,0}; }\n";
      }
      return o.str();
    }
  }

  if (i.op == Op::CallIndirect ||
      (i.op == Op::BranchIndirect && i.imm0 != 0u)) {
    const auto target_arg = i.op == Op::CallIndirect ? arg(i,0) : arg(i,1);
    const auto condition = i.op == Op::CallIndirect ? std::string{} : arg(i,0);
    const auto expected_return = static_cast<GuestAddress>(i.guest_address + 4u);
    if (!condition.empty()) o << "  if(" << condition << ") {\n";
    const auto indent = condition.empty() ? std::string("  ") : std::string("    ");
    o << indent << "const auto raw_target=static_cast<GuestAddress>(" << target_arg << ");\n";
    o << indent << "const auto guest_target=static_cast<GuestAddress>(raw_target & ~3u);\n";
    o << indent << "ExecutionResult rr{}; bool handled_local=false; if("
      << is_local_symbol(local_dispatch_symbol) << "(guest_target)) { rr="
      << local_dispatch_symbol << "(context,guest_target); handled_local=true; }\n";
    o << indent << "if(!handled_local) { if(auto* native=context.lookup_compiled(guest_target,CompiledLookupKind::Call)) rr=native(context); else { auto fallback=context.try_dynamic_fallback(guest_target,CompiledLookupKind::Call); rr=fallback.handled ? fallback.result : runtime.call(raw_target,state,memory); } }\n";
    emit_call_result_check(o, "rr", expected_return, indent);
    if (!condition.empty()) o << "  }\n";
    return o.str();
  }

  if (i.op == Op::BranchIndirect && i.imm0 == 0u && i.imm1 != 1u) {
    o << "  if(" << arg(i,0) << ") { const auto guest_target=static_cast<GuestAddress>("
      << arg(i,1) << " & ~3ull); if(" << is_local_symbol(local_dispatch_symbol)
      << "(guest_target)) { " << kLocalBranchTarget << "=guest_target; goto "
      << kLocalBranchLabel << "; } if(auto* native=context.lookup_compiled(guest_target,CompiledLookupKind::Branch)) return native(context); auto fallback=context.try_dynamic_fallback(guest_target,CompiledLookupKind::Branch); if(fallback.handled) return fallback.result; state.nia=guest_target; return {FlowReason::Branch,guest_target,0}; }\n";
    return o.str();
  }

  return emit_one(i, value_types);
}

} // namespace

std::string CppAotBackend::emit_function(const ir::Block& block,std::string_view name) const {
  std::ostringstream o;
  o<<"ExecutionResult "<<name<<"_v2([[maybe_unused]] ExecutionContext& context) {\n";
  o<<"  auto& state = context.state;\n";
  o<<"  auto& memory = context.memory;\n";
  o<<"  auto& runtime = context.runtime;\n";
  o<<"  auto& memory_access = context.memory_access;\n";
  std::vector<Type> value_types;
  for (const auto& i : block.instructions) if (i.result != ir::kNoValue) { if (value_types.size() <= i.result) value_types.resize(i.result + 1, Type::Void); value_types[i.result] = i.type; }
  std::optional<GuestAddress> current_guest;
  std::optional<GuestAddress> materialized_guest;
  if (block.instructions.empty() || !has_guest_source(block.instructions.front())) {
    o<<"  state.cia="<<block.guest_address<<"u;\n";
    o<<"  state.nia="<<static_cast<GuestAddress>(block.guest_address+4u)<<"u;\n";
  }
  for(const auto&i:block.instructions){
    emit_guest_pc(o,i,current_guest,materialized_guest);
    o<<emit_one(i,value_types);
  }
  emit_exit_pc(o,current_guest,materialized_guest);
  // Final CIA is an architectural property of the guest block, not of the
  // last IR node that survived optimization.  An optimizer may fold the last
  // guest instruction while preserving its state effect in earlier IR - so
  // this must still run even when emit_exit_pc() already materialized
  // something. But when nothing was folded, emit_exit_pc() above already
  // materialized this EXACT address (the last real guest instruction is
  // always at block.end_address-4u), and re-emitting the identical
  // "state.cia=<same value>u;" a second time is pure dead-code bloat, not a
  // second, distinct fact - guard on materialized_guest exactly as
  // emit_guest_pc()/emit_exit_pc() already do for every other redundant case.
  if (block.end_address && block.end_address >= block.guest_address + 4u) {
    const auto final_cia = static_cast<GuestAddress>(block.end_address - 4u);
    if (materialized_guest != final_cia) {
      materialized_guest = final_cia;
      o << "  state.cia=" << final_cia << "u;\n";
    }
  }
  GuestAddress fallthrough = block.end_address;
  if (!fallthrough) {
    fallthrough = current_guest ? static_cast<GuestAddress>(*current_guest + 4u)
                                : static_cast<GuestAddress>(block.guest_address + 4u);
  }
  o<<"  state.nia="<<fallthrough<<"u;\n";
  o<<"  return {FlowReason::Fallthrough,"<<fallthrough<<"u,0};\n}\n";
  o<<"ExecutionResult "<<name<<"([[maybe_unused]] CpuState& state, [[maybe_unused]] MemoryPort& memory, [[maybe_unused]] RuntimeServices& runtime) {\n";
  o<<"  ExecutionContext context(state, memory, runtime);\n";
  o<<"  return "<<name<<"_v2(context);\n}\n";
  return o.str();
}

std::string CppAotBackend::emit_translation_unit(const ir::Block& block,std::string_view name) const {
  std::ostringstream o;
  o<<"#include <bit>\n#include <cmath>\n#include <cfenv>\n#include <cstdint>\n#include <limits>\n";
  o<<"#include \"xenon/cpu/aot_semantics.hpp\"\n\n";
  o<<"using namespace xenon::cpu;\n";
  o<<emit_function(block,name);
  return o.str();
}


std::string CppAotBackend::emit_function(
    const ir::Function& function, std::string_view name,
    std::span<const DirectCallBinding> direct_calls,
    std::span<const GuestAddress> alternate_entries) const {
  if (function.blocks.empty()) throw std::runtime_error("CppAotBackend: empty function");

  std::unordered_set<GuestAddress> local_targets;
  for (const auto& block : function.blocks) local_targets.insert(block.guest_address);
  for (const auto& block : function.blocks) {
    for (const auto& edge : block.successors) {
      if ((edge.kind == ir::EdgeKind::Branch || edge.kind == ir::EdgeKind::Fallthrough) &&
          edge.local && !local_targets.contains(edge.target)) {
        throw std::logic_error(
            "CppAotBackend: local control-flow edge has no materialized basic block");
      }
    }
  }

  std::ostringstream o;
  std::unordered_set<std::string> declared_symbols;
  for (const auto& binding : direct_calls) {
    if (!valid_cpp_identifier(binding.native_symbol))
      throw std::invalid_argument("CppAotBackend: invalid direct-call native symbol");
    if (declared_symbols.insert(binding.native_symbol).second)
      o << "ExecutionResult " << binding.native_symbol << "(ExecutionContext&);\n";
  }
  const std::string dispatch_symbol = std::string(name) + "_dispatch_v2";
  // Deterministic switch order (the set's iteration order is not).
  std::vector<GuestAddress> sorted_targets(local_targets.begin(), local_targets.end());
  std::sort(sorted_targets.begin(), sorted_targets.end());

  // Emit the blocks first so the shared long-jump / local-branch blocks and
  // the is_local() helper are only emitted for functions that use them.
  std::ostringstream body;
  for (const auto& block : function.blocks) {
    auto& o = body;
    o << local_label(block.guest_address) << ": {\n";

    std::vector<Type> value_types;
    for (const auto& i : block.instructions) {
      if (i.result != ir::kNoValue) {
        if (value_types.size() <= i.result) value_types.resize(i.result + 1, Type::Void);
        value_types[i.result] = i.type;
      }
    }

    std::optional<GuestAddress> current_guest;
    std::optional<GuestAddress> materialized_guest;
    if (block.instructions.empty() || !has_guest_source(block.instructions.front())) {
      o << "  state.cia=" << block.guest_address << "u;\n";
      o << "  state.nia=" << static_cast<GuestAddress>(block.guest_address + 4u) << "u;\n";
    }
    for (const auto& i : block.instructions) {
      emit_guest_pc(o, i, current_guest, materialized_guest);
      o << emit_one_in_function(i, value_types, block, local_targets, direct_calls, dispatch_symbol);
    }

    if (!is_guaranteed_terminal(block)) {
      emit_exit_pc(o, current_guest, materialized_guest);
      // See emit_function(const ir::Block&, ...)'s identical guard: only
      // re-emit "state.cia=" here if it differs from what emit_exit_pc()
      // (or the per-instruction loop above) already materialized - otherwise
      // this duplicates the exact same statement for the common case where
      // the optimizer folded nothing.
      if (block.end_address && block.end_address >= block.guest_address + 4u) {
        const auto final_cia = static_cast<GuestAddress>(block.end_address - 4u);
        if (materialized_guest != final_cia) {
          materialized_guest = final_cia;
          o << "  state.cia=" << final_cia << "u;\n";
        }
      }
      if (const auto fallthrough = local_fallthrough(block)) {
        o << "  state.nia=" << *fallthrough << "u;\n";
        o << "  goto " << local_label(*fallthrough) << ";\n";
      } else {
        const auto next = block.end_address
                              ? block.end_address
                              : (current_guest ? static_cast<GuestAddress>(*current_guest + 4u)
                                               : static_cast<GuestAddress>(block.guest_address + 4u));
        o << "  state.nia=" << next << "u;\n";
        o << "  return {FlowReason::Fallthrough," << next << "u,0};\n";
      }
    } else {
      // Keep the generated C++ well-formed even for IR terminals represented
      // as an always-true conditional (for example canonical blr). The host
      // compiler removes this unreachable fallback.
      const auto next = block.end_address
                            ? block.end_address
                            : (current_guest ? static_cast<GuestAddress>(*current_guest + 4u)
                                             : static_cast<GuestAddress>(block.guest_address + 4u));
      o << "  return {FlowReason::Fallthrough," << next << "u,0};\n";
    }
    o << "}\n";
  }
  const auto body_text = body.str();
  const auto uses = [&](std::string_view token) {
    return body_text.find(token) != std::string::npos;
  };
  const bool uses_longjump = uses("goto " + std::string(kLongJumpLabel) + ";");
  const bool uses_local_branch = uses("goto " + std::string(kLocalBranchLabel) + ";");
  const bool uses_is_local = uses(is_local_symbol(dispatch_symbol) + "(");

  if (uses_is_local) {
    o << "static bool " << is_local_symbol(dispatch_symbol) << "(GuestAddress target) {\n"
      << "  switch(target) {\n";
    for (const auto target : sorted_targets) o << "    case " << target << "u:\n";
    o << "      return true;\n    default: return false;\n  }\n}\n";
  }
  o << "static ExecutionResult " << dispatch_symbol << "([[maybe_unused]] ExecutionContext& context, GuestAddress entry) {\n";
  o << "  auto& state = context.state;\n";
  o << "  auto& memory = context.memory;\n";
  o << "  auto& runtime = context.runtime;\n";
  o << "  auto& memory_access = context.memory_access;\n";
  if (uses_longjump) o << "  ExecutionResult " << kLongJumpResult << "{};\n";
  if (uses_local_branch) o << "  GuestAddress " << kLocalBranchTarget << "{};\n";
  o << "  switch(entry) {\n";
  for (const auto target : sorted_targets)
    o << "    case " << target << "u: goto " << local_label(target) << ";\n";
  o << "    default: return {FlowReason::Trap,entry,0x80000003u};\n  }\n";
  o << body_text;
  if (uses_longjump) {
    o << kLongJumpLabel << ":\n  switch(" << kLongJumpResult << ".next_address) {\n";
    for (const auto target : sorted_targets) {
      o << "    case " << target << "u: state.cia=" << target << "u; state.nia="
        << static_cast<GuestAddress>(target + 4u) << "u; goto " << local_label(target)
        << ";\n";
    }
    o << "    default: return " << kLongJumpResult << ";\n  }\n";
  }
  if (uses_local_branch) {
    o << kLocalBranchLabel << ":\n  switch(" << kLocalBranchTarget << ") {\n";
    for (const auto target : sorted_targets) {
      o << "    case " << target << "u: state.nia=" << target << "u; goto "
        << local_label(target) << ";\n";
    }
    o << "    default: return {FlowReason::Trap," << kLocalBranchTarget
      << ",0x80000003u};\n  }\n";
  }
  o << "}\n";
  o << "ExecutionResult " << name << "_v2([[maybe_unused]] ExecutionContext& context) {\n";
  o << "  return " << dispatch_symbol << "(context," << function.guest_address << "u);\n}\n";
  for (const auto entry : alternate_entries) {
    if (entry == function.guest_address) continue;
    if (!local_targets.contains(entry))
      throw std::invalid_argument("CppAotBackend: alternate entry has no local basic block");
    o << "ExecutionResult " << alternate_entry_symbol(name, entry)
      << "([[maybe_unused]] ExecutionContext& context) {\n";
    o << "  return " << dispatch_symbol << "(context," << entry << "u);\n}\n";
  }
  o << "ExecutionResult " << name
    << "([[maybe_unused]] CpuState& state, [[maybe_unused]] MemoryPort& memory, "
       "[[maybe_unused]] RuntimeServices& runtime) {\n";
  o << "  ExecutionContext context(state, memory, runtime);\n";
  o << "  return " << name << "_v2(context);\n}\n";
  return o.str();
}

std::vector<GuestAddress> CppAotBackend::static_call_targets(
    const ir::Function& function) {
  std::unordered_set<GuestAddress> local_targets;
  for (const auto& block : function.blocks) local_targets.insert(block.guest_address);
  std::vector<GuestAddress> targets;
  for (const auto& block : function.blocks) {
    for (const auto& i : block.instructions) {
      std::optional<std::uint64_t> target;
      if (i.op == Op::Call && !i.args.empty()) {
        target = constant_value(block, i.args[0]);
      } else if (i.op == Op::BranchIf && i.imm0 == 1 && i.args.size() >= 2) {
        target = constant_value(block, i.args[1]);
      }
      if (!target) continue;
      const auto guest_target = static_cast<GuestAddress>(*target);
      if (!local_targets.contains(guest_target)) targets.push_back(guest_target);
    }
  }
  std::sort(targets.begin(), targets.end());
  targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
  return targets;
}

std::string CppAotBackend::emit_translation_unit(
    const ir::Function& function, std::string_view name,
    std::span<const DirectCallBinding> direct_calls,
    std::span<const GuestAddress> alternate_entries) const {
  std::ostringstream o;
  o << "#include <bit>\n#include <cmath>\n#include <cfenv>\n#include <cstdint>\n#include <limits>\n";
  o << "#include \"xenon/cpu/aot_semantics.hpp\"\n\n";
  o << "using namespace xenon::cpu;\n";
  o << emit_function(function, name, direct_calls, alternate_entries);
  return o.str();
}

} // namespace xenon::cpu::backend

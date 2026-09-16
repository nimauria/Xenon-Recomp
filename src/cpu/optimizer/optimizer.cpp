#include "xenon/cpu/optimizer.hpp"

#include <optional>
#include <unordered_map>

namespace xenon::cpu::ir {
namespace {

struct ConstantValue { Type type{}; std::uint64_t bits{}; };

std::optional<std::uint64_t> eval_binary(Op op, std::uint64_t a, std::uint64_t b, Type t) {
  const unsigned width = t==Type::I1?1:t==Type::I8?8:t==Type::I16?16:t==Type::I32?32:64;
  const std::uint64_t mask = width==64 ? ~0ull : ((1ull<<width)-1ull);
  a &= mask; b &= mask;
  switch(op){
    case Op::Add:return (a+b)&mask; case Op::Sub:return (a-b)&mask; case Op::Mul:return (a*b)&mask;
    case Op::And:return a&b; case Op::Or:return a|b; case Op::Xor:return a^b;
    case Op::Shl:return (b>=width)?0:((a<<b)&mask);
    case Op::ShrLogical:return (b>=width)?0:(a>>b);
    case Op::CompareEq:return a==b; case Op::CompareNe:return a!=b;
    case Op::CompareUlt:return a<b; case Op::CompareUgt:return a>b;
    default:return std::nullopt;
  }
}

}  // namespace

OptimizationStats Optimizer::run(Block& block) const {
  OptimizationStats stats{};
  std::unordered_map<ValueId,ConstantValue> constants;
  for(auto& insn:block.instructions){
    if(insn.op==Op::Constant && insn.result!=kNoValue){ constants[insn.result]={insn.type,insn.imm0}; continue; }
    if(insn.result==kNoValue) continue;
    if(insn.args.size()==1 && insn.op==Op::Not){
      auto it=constants.find(insn.args[0]); if(it!=constants.end()){
        std::uint64_t mask=insn.type==Type::I1?1:insn.type==Type::I8?0xFF:insn.type==Type::I16?0xFFFF:insn.type==Type::I32?0xFFFFFFFFull:~0ull;
        insn.op=Op::Constant; insn.args.clear(); insn.imm0=(~it->second.bits)&mask; constants[insn.result]={insn.type,insn.imm0}; ++stats.constants_folded;
      }
      continue;
    }
    if(insn.args.size()==2){
      auto a=constants.find(insn.args[0]), b=constants.find(insn.args[1]);
      if(a!=constants.end()&&b!=constants.end()){
        if(auto v=eval_binary(insn.op,a->second.bits,b->second.bits,insn.type)){
          insn.op=Op::Constant; insn.args.clear(); insn.imm0=*v; insn.imm1=0; constants[insn.result]={insn.type,*v}; ++stats.constants_folded;
        }
      }
    }
  }
  return stats;
}

OptimizationStats Optimizer::run(Function& function) const {
  OptimizationStats out{};
  for(auto& b:function.blocks){ auto s=run(b); out.constants_folded+=s.constants_folded; out.instructions_removed+=s.instructions_removed; }
  return out;
}

}  // namespace xenon::cpu::ir

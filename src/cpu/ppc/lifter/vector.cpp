#include "xenon/cpu/lifter.hpp"

#include <string_view>

namespace xenon::cpu {
namespace {
using ir::Op;
using ir::Type;
using ir::ValueId;

bool extended(InstructionFormat f) {
  return f == InstructionFormat::VX128 || f == InstructionFormat::VX128_1 ||
         f == InstructionFormat::VX128_2 || f == InstructionFormat::VX128_3 ||
         f == InstructionFormat::VX128_4 || f == InstructionFormat::VX128_5 ||
         f == InstructionFormat::VX128_R || f == InstructionFormat::VX128_P;
}
unsigned vd(const DecodedInstruction& i) { return extended(i.info->format) ? i.vx128_vd() : i.vd5(); }
unsigned va(const DecodedInstruction& i) {
  switch(i.info->format) {
    case InstructionFormat::VX128: case InstructionFormat::VX128_2:
    case InstructionFormat::VX128_5: case InstructionFormat::VX128_R:
      return i.vx128_va();
    default: return i.va5();
  }
}
unsigned vb(const DecodedInstruction& i) {
  switch(i.info->format) {
    case InstructionFormat::VX128: case InstructionFormat::VX128_2:
    case InstructionFormat::VX128_3: case InstructionFormat::VX128_4:
    case InstructionFormat::VX128_5: case InstructionFormat::VX128_R:
    case InstructionFormat::VX128_P:
      return i.vx128_vb();
    default: return i.vb5();
  }
}
unsigned vc(const DecodedInstruction& i) {
  if (i.info->format == InstructionFormat::VX128_2) return (i.word >> 8) & 7u;
  return i.vc5();
}

ValueId vread(ir::Builder& b,unsigned r){ return b.read_vector(r); }
void vwrite(ir::Builder& b,unsigned r,ValueId v){ b.write_vector(r,v); }

bool starts(std::string_view m,std::string_view p){ return m.starts_with(p); }

// Compact semantic tag derived from mnemonic family. The exact guest mnemonic
// remains attached to the IR instruction for the static vector-expansion pass.
std::uint64_t vector_tag(std::string_view m) {
  std::uint64_t h=1469598103934665603ull;
  for(char c:m){ h^=static_cast<unsigned char>(c); h*=1099511628211ull; }
  return h;
}

ValueId unary(ir::Builder& b, Op op, const DecodedInstruction& i, unsigned src) {
  auto a=vread(b,src); const ValueId x[]={a};
  return b.emit(op,Type::V128,x,vector_tag(i.mnemonic()),0,&i);
}
ValueId binary(ir::Builder& b, Op op, const DecodedInstruction& i,unsigned a_reg,unsigned b_reg) {
  auto a=vread(b,a_reg), c=vread(b,b_reg); const ValueId x[]={a,c};
  return b.emit(op,Type::V128,x,vector_tag(i.mnemonic()),0,&i);
}
ValueId ternary(ir::Builder& b, Op op, const DecodedInstruction& i,unsigned a_reg,unsigned b_reg,unsigned c_reg) {
  auto a=vread(b,a_reg), c=vread(b,b_reg), d=vread(b,c_reg); const ValueId x[]={a,c,d};
  return b.emit(op,Type::V128,x,vector_tag(i.mnemonic()),0,&i);
}

} // namespace

bool Lifter::lift_vector(const DecodedInstruction& i, ir::Builder& b) const {
  const auto m=i.mnemonic(); const unsigned d=vd(i);

  // Address-derived permutation vectors are vector ops even though they consume GPRs.
  if(m=="lvsl" || m=="lvsl128" || m=="lvsr" || m=="lvsr128") {
    auto base=i.ra()?b.read_gpr(i.ra()):b.constant_i64(0); auto idx=b.read_gpr(i.rb());
    const ValueId x[]={base,idx};
    auto v=b.emit((m.starts_with("lvsl"))?Op::VMakeLoadShiftLeft:Op::VMakeLoadShiftRight,
                  Type::V128,x,vector_tag(m),0,&i); vwrite(b,d,v); return true;
  }

  // Full VSCR state is retained; these transfers are kept explicit rather than
  // reducing VSCR to SAT-only state.
  // mfvscr/mtvscr are classified as Control and are lowered there.

  // Floating vector arithmetic, including Xbox VMX128 forms.
  if(starts(m,"vaddfp") || starts(m,"vsubfp") || starts(m,"vmulfp128")) {
    Op op=starts(m,"vadd")?Op::VAdd:(starts(m,"vsub")?Op::VSub:Op::VMul);
    vwrite(b,d,binary(b,op,i,va(i),vb(i))); return true;
  }
  if(starts(m,"vmaddfp") || starts(m,"vmaddcfp") || starts(m,"vnmsubfp")) {
    ValueId r{};
    if(i.info->format==InstructionFormat::VA) r=ternary(b,Op::VMadd,i,va(i),vb(i),vc(i));
    else {
      auto old=vread(b,d), a=vread(b,va(i)), c=vread(b,vb(i)); const ValueId x[]={old,a,c};
      r=b.emit(Op::VMadd,Type::V128,x,vector_tag(m),0,&i);
    }
    vwrite(b,d,r); return true;
  }
  if(starts(m,"vmax") || starts(m,"vmin")) {
    vwrite(b,d,binary(b,starts(m,"vmax")?Op::VMax:Op::VMin,i,va(i),vb(i))); return true;
  }
  if(starts(m,"vrefp") || starts(m,"vrsqrtefp") || starts(m,"vexptefp") || starts(m,"vlogefp")) {
    vwrite(b,d,unary(b,Op::VEstimate,i,vb(i))); return true;
  }
  if(starts(m,"vrfi")) { vwrite(b,d,unary(b,Op::VConvert,i,vb(i))); return true; }
  if(starts(m,"vmsum3fp") || starts(m,"vmsum4fp")) {
    auto old=vread(b,d), a=vread(b,va(i)), c=vread(b,vb(i)); const ValueId x[]={old,a,c};
    vwrite(b,d,b.emit(Op::VDot,Type::V128,x,vector_tag(m),0,&i)); return true;
  }

  // Bitwise operations are lane-independent and translate directly.
  if(starts(m,"vandc")) { vwrite(b,d,binary(b,Op::VAndNot,i,va(i),vb(i))); return true; }
  if(starts(m,"vand"))  { vwrite(b,d,binary(b,Op::VAnd,i,va(i),vb(i))); return true; }
  if(starts(m,"vor"))   { vwrite(b,d,binary(b,Op::VOr,i,va(i),vb(i))); return true; }
  if(starts(m,"vxor"))  { vwrite(b,d,binary(b,Op::VXor,i,va(i),vb(i))); return true; }
  if(starts(m,"vnor"))  { vwrite(b,d,binary(b,Op::VNot,i,va(i),vb(i))); return true; }
  if(starts(m,"vsel")) {
    ValueId r{};
    if(i.info->format==InstructionFormat::VA) r=ternary(b,Op::VSelect,i,va(i),vb(i),vc(i));
    else { auto old=vread(b,d),a=vread(b,va(i)),c=vread(b,vb(i)); const ValueId x[]={old,a,c}; r=b.emit(Op::VSelect,Type::V128,x,vector_tag(m),0,&i); }
    vwrite(b,d,r); return true;
  }

  if(starts(m,"vcmp")) {
    auto r=binary(b,Op::VCompare,i,va(i),vb(i)); vwrite(b,d,r);
    // VC / VX128_R Rc updates CR6 with all/none-equal semantics.
    if(i.rc()) { const ValueId a[]={r}; b.emit(Op::VUpdateCR6,Type::Void,a,vector_tag(m),0,&i); }
    return true;
  }

  // Integer vector arithmetic/saturating arithmetic/averages/min/max/multiply/sums.
  if(starts(m,"vadd") || starts(m,"vsub")) {
    vwrite(b,d,binary(b,starts(m,"vadd")?Op::VAdd:Op::VSub,i,va(i),vb(i))); return true;
  }
  if(starts(m,"vavg")) { vwrite(b,d,binary(b,Op::VAvg,i,va(i),vb(i))); return true; }
  if(starts(m,"vmhadd") || starts(m,"vmhradd") || starts(m,"vmladd")) {
    vwrite(b,d,ternary(b,Op::VMadd,i,va(i),vb(i),vc(i))); return true;
  }
  if(starts(m,"vmule") || starts(m,"vmulo")) {
    vwrite(b,d,binary(b,Op::VMultiplyEvenOdd,i,va(i),vb(i))); return true;
  }
  if(starts(m,"vmul")) { vwrite(b,d,binary(b,Op::VMul,i,va(i),vb(i))); return true; }
  if(starts(m,"vsum") || starts(m,"vmsum")) {
    ValueId r{};
    if(i.info->format==InstructionFormat::VA) r=ternary(b,Op::VSum,i,va(i),vb(i),vc(i));
    else r=binary(b,Op::VSum,i,va(i),vb(i));
    vwrite(b,d,r); return true;
  }

  if(starts(m,"vcfs") || starts(m,"vcfu") || starts(m,"vcts") || starts(m,"vctu") ||
     starts(m,"vcfps") || starts(m,"vcfpu") || starts(m,"vcsx") || starts(m,"vcux")) {
    vwrite(b,d,unary(b,Op::VConvert,i,vb(i))); return true;
  }

  if(starts(m,"vmrgh") || starts(m,"vmrgl")) {
    vwrite(b,d,binary(b,Op::VMerge,i,va(i),vb(i))); return true;
  }
  if(starts(m,"vpermwi")) { vwrite(b,d,unary(b,Op::VPermute,i,vb(i))); return true; }
  if(starts(m,"vperm")) {
    if(i.info->format==InstructionFormat::VA || i.info->format==InstructionFormat::VX128_2)
      vwrite(b,d,ternary(b,Op::VPermute,i,va(i),vb(i),vc(i)));
    else vwrite(b,d,unary(b,Op::VPermute,i,vb(i)));
    return true;
  }

  if(starts(m,"vpk")) { vwrite(b,d,binary(b,Op::VPack,i,va(i),vb(i))); return true; }
  if(starts(m,"vupk")) { vwrite(b,d,unary(b,Op::VUnpack,i,vb(i))); return true; }

  if(starts(m,"vsplt")) { vwrite(b,d,unary(b,Op::VSplat,i,vb(i))); return true; }
  if(starts(m,"vrlimi")) {
    auto old=vread(b,d),src=vread(b,vb(i)); const ValueId x[]={old,src};
    vwrite(b,d,b.emit(Op::VRotateInsert,Type::V128,x,vector_tag(m),i.word,&i)); return true;
  }
  if(starts(m,"vrl")) { vwrite(b,d,binary(b,Op::VRotate,i,va(i),vb(i))); return true; }
  if(starts(m,"vsl") || starts(m,"vsr")) {
    if(starts(m,"vsldoi")) {
      auto a=vread(b,va(i)),c=vread(b,vb(i)); const ValueId x[]={a,c};
      vwrite(b,d,b.emit(Op::VShiftDouble,Type::V128,x,vector_tag(m),i.word,&i));
    } else vwrite(b,d,binary(b,Op::VShift,i,va(i),vb(i)));
    return true;
  }

  return false;
}

} // namespace xenon::cpu

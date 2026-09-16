#include "xenon/cpu/lifter.hpp"

#include <string_view>

namespace xenon::cpu {
namespace {
using ir::Op;
using ir::Type;
using ir::ValueId;

ValueId binary(ir::Builder& b, Op op, ValueId a, ValueId c, Type t = Type::I1) {
  const ValueId args[] = {a, c};
  return b.emit(op, t, args);
}
ValueId logical_not(ir::Builder& b, ValueId v) {
  const ValueId a[] = {v}; return b.emit(Op::Not, Type::I1, a);
}

// BO is represented in the natural 5-bit integer extracted from the instruction.
// These tests match the architectural BO[0..4] rules after accounting for PPC's
// MSB-first documentation numbering (and are cross-checked against Xenia's emitter).
ValueId branch_condition(const DecodedInstruction& i, ir::Builder& b, bool use_ctr) {
  const unsigned bo = i.bo();
  ValueId ctr_ok = ir::kNoValue;
  ValueId cond_ok = ir::kNoValue;

  // BO[2] == 1 -> do not decrement/test CTR. In the extracted integer this is bit 2.
  if (use_ctr && (bo & 0b00100u) == 0) {
    auto ctr = b.read_ctr(); auto one = b.constant_i64(1);
    const ValueId sub[] = {ctr, one}; ctr = b.emit(Op::Sub, Type::I64, sub); b.write_ctr(ctr);
    auto zero = b.constant_i64(0); const ValueId cmp[] = {ctr, zero};
    const bool want_zero = (bo & 0b00010u) != 0;
    ctr_ok = b.emit(want_zero ? Op::CompareEq : Op::CompareNe, Type::I1, cmp);
  }

  // BO[0] == 1 -> ignore CR. In extracted integer this is bit 4.
  if ((bo & 0b10000u) == 0) {
    auto cr = b.emit(Op::ReadCRBit, Type::I1, {}, i.bi());
    const bool want_true = (bo & 0b01000u) != 0;
    cond_ok = want_true ? cr : logical_not(b, cr);
  }

  if (ctr_ok != ir::kNoValue && cond_ok != ir::kNoValue)
    return binary(b, Op::And, ctr_ok, cond_ok);
  if (ctr_ok != ir::kNoValue) return ctr_ok;
  if (cond_ok != ir::kNoValue) return cond_ok;
  return b.constant_i1(true);
}

}  // namespace

bool Lifter::lift_control(const DecodedInstruction& i, ir::Builder& b) const {
  const auto m = i.mnemonic();

  if (m == "bx") {
    const auto target = b.constant_i64(i.direct_branch_target());
    if (i.lk()) b.write_lr(b.constant_i64(static_cast<std::uint64_t>(i.address + 4u)));
    const ValueId args[] = {target};
    b.emit(i.lk() ? Op::Call : Op::Branch, Type::Void, args, 0, 0, &i);
    return true;
  }

  if (m == "bcx") {
    auto cond = branch_condition(i,b,true);
    auto target = b.constant_i64(i.direct_branch_target());
    // LR is updated regardless of whether the branch is taken.
    if (i.lk()) b.write_lr(b.constant_i64(static_cast<std::uint64_t>(i.address + 4u)));
    const ValueId a[] = {cond,target};
    b.emit(Op::BranchIf,Type::Void,a,i.lk()?1u:0u,0,&i);
    return true;
  }

  if (m == "bclrx") {
    // Target is the old LR; capture it before LK updates LR.
    auto target=b.read_lr(); auto cond=branch_condition(i,b,true);
    if(i.lk()) b.write_lr(b.constant_i64(static_cast<std::uint64_t>(i.address+4u)));
    const ValueId a[]={cond,target};
    b.emit(Op::BranchIndirect,Type::Void,a,i.lk()?1u:0u,1u/*LR*/, &i);
    return true;
  }

  if (m == "bcctrx") {
    auto target=b.read_ctr(); auto cond=branch_condition(i,b,false);
    if(i.lk()) b.write_lr(b.constant_i64(static_cast<std::uint64_t>(i.address+4u)));
    const ValueId a[]={cond,target};
    b.emit(Op::BranchIndirect,Type::Void,a,i.lk()?1u:0u,2u/*CTR*/, &i);
    return true;
  }

  if (m == "crand" || m == "crandc" || m == "creqv" || m == "crnand" ||
      m == "crnor" || m == "cror" || m == "crorc" || m == "crxor") {
    const unsigned bt = (i.word >> 21) & 31u;
    const unsigned ba = (i.word >> 16) & 31u;
    const unsigned bb = (i.word >> 11) & 31u;
    auto va = b.emit(Op::ReadCRBit, Type::I1, {}, ba);
    auto vb = b.emit(Op::ReadCRBit, Type::I1, {}, bb);
    ValueId r{};
    if (m == "crand") r = binary(b, Op::And, va, vb);
    else if (m == "crandc") r = binary(b, Op::And, va, logical_not(b,vb));
    else if (m == "creqv") { auto x=binary(b,Op::Xor,va,vb); r=logical_not(b,x); }
    else if (m == "crnand") { auto x=binary(b,Op::And,va,vb); r=logical_not(b,x); }
    else if (m == "crnor") { auto x=binary(b,Op::Or,va,vb); r=logical_not(b,x); }
    else if (m == "cror") r = binary(b, Op::Or, va, vb);
    else if (m == "crorc") r = binary(b, Op::Or, va, logical_not(b,vb));
    else r = binary(b, Op::Xor, va, vb);
    const ValueId out[] = {r}; b.emit(Op::WriteCRBit, Type::Void, out, bt);
    return true;
  }

  if (m == "mcrf") {
    const unsigned bf = (i.word >> 23) & 7u;
    const unsigned bfa = (i.word >> 18) & 7u;
    auto v = b.emit(Op::ReadCRField, Type::I8, {}, bfa);
    const ValueId a[] = {v}; b.emit(Op::WriteCRField, Type::Void, a, bf);
    return true;
  }

  if (m == "mfcr") {
    auto v = b.emit(Op::ReadCR, Type::I32); const ValueId a[] = {v};
    b.write_gpr(i.rt(), b.emit(Op::ZeroExtend, Type::I64, a)); return true;
  }
  if (m == "mtcrf") {
    auto src=b.read_gpr(i.rs()); const ValueId a[]={src};
    const unsigned fxm=((i.word>>12)&0xFFu);
    b.emit(Op::MoveCRFields,Type::Void,a,fxm,0,&i); return true;
  }
  if (m == "mcrxr") {
    b.emit(Op::MoveXERToCR,Type::Void,{},i.crfd(),0,&i); return true;
  }

  if (m == "mfspr") {
    const auto spr = i.spr(); ValueId v{}; bool i32=false;
    switch (spr) {
      case 1: v=b.emit(Op::ReadXER,Type::I32); i32=true; break;
      case 8: v=b.emit(Op::ReadLR,Type::I64); break;
      case 9: v=b.emit(Op::ReadCTR,Type::I64); break;
      case 256: v=b.emit(Op::ReadVRSAVE,Type::I32); i32=true; break;
      case 268: v=b.emit(Op::ReadTimeBase,Type::I64); break;
      case 269: { auto tb=b.emit(Op::ReadTimeBase,Type::I64); auto sh=b.constant_i64(32); const ValueId a[]={tb,sh}; v=b.emit(Op::ShrLogical,Type::I64,a); break; }
      case 287: v=b.emit(Op::ReadPVR,Type::I32); i32=true; break;
      default: v=b.emit(Op::ReadSPR,Type::I64,{},spr,0,&i); break;
    }
    if(i32){ const ValueId a[]={v}; v=b.emit(Op::ZeroExtend,Type::I64,a); }
    b.write_gpr(i.rt(),v); return true;
  }

  if (m == "mtspr") {
    const auto spr=i.spr(); auto v=b.read_gpr(i.rs()); const ValueId a[]={v};
    if(spr==1){ auto n=b.emit(Op::Truncate,Type::I32,a); const ValueId z[]={n}; b.emit(Op::WriteXER,Type::Void,z); return true; }
    if(spr==8){ b.emit(Op::WriteLR,Type::Void,a); return true; }
    if(spr==9){ b.emit(Op::WriteCTR,Type::Void,a); return true; }
    if(spr==256){ auto n=b.emit(Op::Truncate,Type::I32,a); const ValueId z[]={n}; b.emit(Op::WriteVRSAVE,Type::Void,z); return true; }
    b.emit(Op::WriteSPR,Type::Void,a,spr,0,&i); return true;
  }

  if (m == "mftb") {
    auto tb=b.emit(Op::ReadTimeBase,Type::I64);
    if(i.spr()==269){ auto sh=b.constant_i64(32); const ValueId a[]={tb,sh}; tb=b.emit(Op::ShrLogical,Type::I64,a); }
    b.write_gpr(i.rt(),tb); return true;
  }
  if (m == "mfmsr") { b.write_gpr(i.rt(),b.emit(Op::ReadMSR,Type::I64)); return true; }
  if (m == "mtmsr" || m == "mtmsrd") { auto v=b.read_gpr(i.rs()); const ValueId a[]={v}; b.emit(Op::WriteMSR,Type::Void,a,(i.word>>16)&1u,m == "mtmsrd" ? 1u : 0u,&i); return true; }

  if (m == "sc") { b.emit(Op::Syscall,Type::Void,{},(i.word>>5)&0x7Fu,0,&i); return true; }
  if (m == "td" || m == "tdi" || m == "tw" || m == "twi") {
    // Trap predicate is expanded by the trap lowering pass with signed and unsigned comparisons.
    b.emit(Op::Trap,Type::Void,{},(i.word>>21)&31u,i.word,&i); return true;
  }

  if (m == "mffsx") {
    auto fps=b.emit(Op::ReadFPSCR,Type::I32); const ValueId a[]={fps};
    auto raw=b.emit(Op::ZeroExtend,Type::I64,a);
    // Dedicated marker documents the architected transfer for backends that
    // want to preserve undefined/reserved-bit policy centrally.
    const ValueId x[]={raw}; auto out=b.emit(Op::MoveFPSCRToFPRBits,Type::I64,x,0,0,&i);
    b.write_fpr_bits(i.frt(),out);
    if(i.rc()) b.emit(Op::UpdateCR1FromFPSCR,Type::Void,{},0,0,&i);
    return true;
  }
  if (m == "mcrfs") {
    b.emit(Op::MoveFPSCRFieldToCR,Type::Void,{},i.crfd(),i.crfs(),&i); return true;
  }
  if (m == "mtfsb0x" || m == "mtfsb1x") {
    b.emit(Op::SetFPSCRBit,Type::Void,{},i.rt(),m=="mtfsb1x"?1u:0u,&i);
    if(i.rc()) b.emit(Op::UpdateCR1FromFPSCR,Type::Void,{},0,0,&i);
    return true;
  }
  if (m == "mtfsfix") {
    const unsigned imm=(i.word>>12)&0xFu;
    b.emit(Op::WriteFPSCRFieldImmediate,Type::Void,{},i.crfd(),imm,&i);
    if(i.rc()) b.emit(Op::UpdateCR1FromFPSCR,Type::Void,{},0,0,&i);
    return true;
  }
  if (m == "mtfsfx") {
    auto src=b.read_fpr_bits(i.rb()); const ValueId a[]={src};
    const unsigned fm=(i.word>>17)&0xFFu;
    b.emit(Op::WriteFPSCRFields,Type::Void,a,fm,0,&i);
    if(i.rc()) b.emit(Op::UpdateCR1FromFPSCR,Type::Void,{},0,0,&i);
    return true;
  }
  if (m == "mfvscr") {
    auto v=b.emit(Op::MoveVSCRToVector,Type::V128,{},0,0,&i); b.write_vector(i.vd5(),v); return true;
  }
  if (m == "mtvscr") {
    auto v=b.read_vector(i.vb5()); const ValueId a[]={v}; b.emit(Op::MoveVectorToVSCR,Type::Void,a,0,0,&i); return true;
  }

  return false;
}

}  // namespace xenon::cpu

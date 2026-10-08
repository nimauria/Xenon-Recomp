#include "xenon/cpu/lifter.hpp"

#include <string_view>

namespace xenon::cpu {
namespace {
using ir::Op;
using ir::Type;
using ir::ValueId;

ValueId f64(ir::Builder& b, unsigned reg) {
  auto bits = b.read_fpr_bits(reg); const ValueId a[] = {bits};
  return b.emit(Op::Bitcast, Type::F64, a);
}

void store_f64(ir::Builder& b, unsigned reg, ValueId v) {
  const ValueId a[] = {v}; auto bits = b.emit(Op::Bitcast, Type::I64, a);
  b.write_fpr_bits(reg, bits);
}

void update_status(ir::Builder& b, ValueId result, const DecodedInstruction& i,
                   std::uint64_t operation_class = 0) {
  const ValueId a[] = {result};
  b.emit(Op::FUpdateStatus, Type::Void, a, operation_class, i.rc() ? 1u : 0u, &i);
  if (i.rc()) b.emit(Op::UpdateCR1FromFPSCR, Type::Void, {}, 0, 0, &i);
}

ValueId maybe_single(ir::Builder& b, ValueId v, bool single) {
  if (!single) return v;
  // The final single-precision rounding is architecturally part of the guest
  // operation and must contribute FI/FR/XX. FRoundSingle keeps it inside the
  // FPSCR-aware AOT semantic path rather than using an untracked host cast.
  const ValueId a[] = {v};
  return b.emit(Op::FRoundSingle, Type::F64, a);
}

} // namespace

bool Lifter::lift_fpu(const DecodedInstruction& i, ir::Builder& b) const {
  const auto m = i.mnemonic();
  const bool single = m.ends_with("sx");

  if (m == "faddx" || m == "faddsx" || m == "fsubx" || m == "fsubsx" ||
      m == "fdivx" || m == "fdivsx") {
    auto a = f64(b, i.ra()); auto c = f64(b, i.rb());
    const ValueId args[] = {a,c};
    const Op op = m.starts_with("fadd") ? Op::FAdd : (m.starts_with("fsub") ? Op::FSub : Op::FDiv);
    auto r = b.emit(op, Type::F64, args, single ? 1u : 0u, 0, &i);
    r = maybe_single(b,r,single); store_f64(b,i.frt(),r); update_status(b,r,i);
    return true;
  }
  if (m == "fmulx" || m == "fmulsx") {
    auto a=f64(b,i.ra()); auto c=f64(b,i.frc()); const ValueId args[]={a,c};
    auto r=b.emit(Op::FMul,Type::F64,args,single?1u:0u,0,&i); r=maybe_single(b,r,single);
    store_f64(b,i.frt(),r); update_status(b,r,i); return true;
  }

  if (m == "fmaddx" || m == "fmaddsx" || m == "fmsubx" || m == "fmsubsx" ||
      m == "fnmaddx" || m == "fnmaddsx" || m == "fnmsubx" || m == "fnmsubsx") {
    auto a=f64(b,i.ra()); auto c=f64(b,i.frc()); auto addend=f64(b,i.rb());
    const ValueId args[]={a,c,addend};
    // imm0: bit0 subtract addend, bit1 negate final result, bit2 single precision.
    std::uint64_t flags = (m.starts_with("fmsub") || m.starts_with("fnmsub")) ? 1u : 0u;
    if (m.starts_with("fnm")) flags |= 2u;
    if (single) flags |= 4u;
    auto r=b.emit(Op::FFma,Type::F64,args,flags,0,&i); r=maybe_single(b,r,single);
    store_f64(b,i.frt(),r); update_status(b,r,i); return true;
  }

  if (m == "fsqrtx" || m == "fsqrtsx") {
    auto a=f64(b,i.rb()); const ValueId args[]={a};
    auto r=b.emit(Op::FSqrt,Type::F64,args,single?1u:0u,0,&i); r=maybe_single(b,r,single);
    store_f64(b,i.frt(),r); update_status(b,r,i); return true;
  }
  if (m == "fresx" || m == "frsqrtex") {
    auto a=f64(b,i.rb()); const ValueId args[]={a};
    auto r=b.emit(m=="fresx"?Op::FReciprocalEstimate:Op::FReciprocalSqrtEstimate,
                  Type::F64,args,0,0,&i);
    store_f64(b,i.frt(),r); update_status(b,r,i); return true;
  }
  if (m == "fselx") {
    auto a=f64(b,i.ra()); auto yes=f64(b,i.frc()); auto no=f64(b,i.rb());
    const ValueId args[]={a,yes,no}; auto r=b.emit(Op::FSelect,Type::F64,args,0,0,&i);
    store_f64(b,i.frt(),r);
    // fsel does not alter FPSCR; Rc only snapshots FPSCR summaries into CR1.
    if(i.rc()) b.emit(Op::UpdateCR1FromFPSCR,Type::Void,{},0,0,&i);
    return true;
  }

  if (m == "fabsx" || m == "fnegx" || m == "fnabsx" || m == "fmrx") {
    auto a=f64(b,i.rb()); ValueId r=a;
    if (m == "fabsx" || m == "fnabsx") { const ValueId x[]={r}; r=b.emit(Op::FAbs,Type::F64,x,0,0,&i); }
    if (m == "fnegx" || m == "fnabsx") { const ValueId x[]={r}; r=b.emit(Op::FNeg,Type::F64,x,0,0,&i); }
    store_f64(b,i.frt(),r);
    // Move/sign instructions never modify FPSCR.
    if(i.rc()) b.emit(Op::UpdateCR1FromFPSCR,Type::Void,{},0,0,&i);
    return true;
  }

  if (m == "frspx") {
    auto a=f64(b,i.rb()); const ValueId x[]={a}; auto r=b.emit(Op::FRoundSingle,Type::F64,x,0,0,&i);
    store_f64(b,i.frt(),r); update_status(b,r,i); return true;
  }
  if (m == "fcfidx") {
    auto raw=b.read_fpr_bits(i.rb()); const ValueId x[]={raw};
    auto r=b.emit(Op::FConvertFromI64,Type::F64,x,1u/*signed*/,64u,&i);
    store_f64(b,i.frt(),r); update_status(b,r,i); return true;
  }
  if (m == "fctidx" || m == "fctidzx" || m == "fctiwx" || m == "fctiwzx") {
    auto a=f64(b,i.rb()); const ValueId x[]={a};
    const bool to_zero=m=="fctidzx"||m=="fctiwzx";
    const unsigned width=(m=="fctiwx"||m=="fctiwzx")?32u:64u;
    // Result is the architected integer bit pattern placed in the FPR.
    auto raw=b.emit(Op::FConvertToI64,Type::I64,x,to_zero?1u:0u,width,&i);
    b.write_fpr_bits(i.frt(),raw);
    // The status pass receives the original FP value and conversion metadata.
    b.emit(Op::FUpdateStatus,Type::Void,x,0x100u|width,to_zero?1u:0u,&i);
    if(i.rc()) b.emit(Op::UpdateCR1FromFPSCR,Type::Void,{},0,0,&i);
    return true;
  }

  if (m == "fcmpu" || m == "fcmpo") {
    auto a=f64(b,i.ra()); auto c=f64(b,i.rb()); const ValueId x[]={a,c};
    auto field=b.emit(Op::FCompare,Type::I8,x,m=="fcmpo"?1u:0u,0,&i);
    const ValueId out[]={field}; b.emit(Op::WriteCRField,Type::Void,out,i.crfd(),0,&i);
    b.emit(Op::FUpdateStatus,Type::Void,x,m=="fcmpo"?0x201u:0x200u,0,&i);
    return true;
  }

  // Full FPSCR is architectural state. Straightforward transfers are direct;
  // field-masked operations remain canonical static semantic nodes so reserved
  // and sticky-bit behavior is not approximated.
  if (m == "mffsx") {
    auto fps=b.emit(Op::ReadFPSCR,Type::I32); const ValueId x[]={fps};
    auto raw=b.emit(Op::ZeroExtend,Type::I64,x); b.write_fpr_bits(i.frt(),raw);
    if(i.rc()) b.emit(Op::UpdateCR1FromFPSCR,Type::Void,{},0,0,&i);
    return true;
  }
  // FPSCR transfer/control instructions are classified as Control and lowered there.

  return false;
}

} // namespace xenon::cpu

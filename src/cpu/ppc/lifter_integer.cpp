#include "xenon/cpu/lifter.hpp"

#include <array>
#include <string_view>

namespace xenon::cpu {
namespace {

using ir::Op;
using ir::Type;
using ir::ValueId;

ValueId emit_binary(ir::Builder& b, Op op, ValueId a, ValueId c, Type t = Type::I64) {
  const ValueId args[] = {a, c};
  return b.emit(op, t, args);
}

void emit_cr0(ir::Builder& b, ValueId v) {
  const ValueId args[] = {v};
  b.emit(Op::UpdateCR0Signed, Type::Void, args);
}

void emit_ca_from_add(ir::Builder& b, ValueId a, ValueId /*c*/, ValueId sum) {
  // carry = sum <u a (valid for two-input unsigned addition)
  const ValueId args[] = {sum, a};
  auto carry = b.emit(Op::CompareUlt, Type::I1, args);
  const ValueId ca_args[] = {carry};
  b.emit(Op::SetXerCA, Type::Void, ca_args);
}

void emit_overflow_add(ir::Builder& b, ValueId a, ValueId c, ValueId sum) {
  auto axr = emit_binary(b, Op::Xor, a, sum);
  auto bxr = emit_binary(b, Op::Xor, c, sum);
  auto both = emit_binary(b, Op::And, axr, bxr);
  auto sign = b.constant_i64(0x8000000000000000ull);
  auto masked = emit_binary(b, Op::And, both, sign);
  auto zero = b.constant_i64(0);
  const ValueId cmp_args[] = {masked, zero};
  auto ov = b.emit(Op::CompareNe, Type::I1, cmp_args);
  const ValueId ov_args[] = {ov};
  b.emit(Op::SetXerOverflowSticky, Type::Void, ov_args);
}

void emit_overflow_sub(ir::Builder& b, ValueId a, ValueId c, ValueId result) {
  // result = a - c; overflow if signs(a,c) differ and sign(result) differs from a.
  auto axc = emit_binary(b, Op::Xor, a, c);
  auto axr = emit_binary(b, Op::Xor, a, result);
  auto both = emit_binary(b, Op::And, axc, axr);
  auto sign = b.constant_i64(0x8000000000000000ull);
  auto masked = emit_binary(b, Op::And, both, sign);
  auto zero = b.constant_i64(0);
  const ValueId cmp_args[] = {masked, zero};
  auto ov = b.emit(Op::CompareNe, Type::I1, cmp_args);
  const ValueId ov_args[] = {ov};
  b.emit(Op::SetXerOverflowSticky, Type::Void, ov_args);
}

ValueId read_xer_ca(ir::Builder& b) {
  auto xer=b.emit(Op::ReadXER,Type::I32); auto mask=b.constant_i32(0x20000000u);
  const ValueId a[]={xer,mask}; auto v=b.emit(Op::And,Type::I32,a); auto zero=b.constant_i32(0);
  const ValueId c[]={v,zero}; return b.emit(Op::CompareNe,Type::I1,c);
}

void set_ca3(ir::Builder& b, ValueId a, ValueId c, ValueId carry) {
  const ValueId args[]={a,c,carry}; auto out=b.emit(Op::CarryOut,Type::I1,args,64,0);
  const ValueId x[]={out}; b.emit(Op::SetXerCA,Type::Void,x);
}

void set_ov3(ir::Builder& b, ValueId a, ValueId c, ValueId carry, unsigned variant=0) {
  const ValueId args[]={a,c,carry}; auto out=b.emit(Op::SignedOverflow,Type::I1,args,64,variant);
  const ValueId x[]={out}; b.emit(Op::SetXerOverflowSticky,Type::Void,x);
}

std::uint64_t mask64(unsigned mb, unsigned me) {
  mb &= 63u;
  me &= 63u;
  std::uint64_t mask = 0;
  auto set_arch_bit = [&mask](unsigned p) { mask |= (1ull << (63u - p)); };
  if (mb <= me) {
    for (unsigned p = mb; p <= me; ++p) set_arch_bit(p);
  } else {
    for (unsigned p = mb; p < 64; ++p) set_arch_bit(p);
    for (unsigned p = 0; p <= me; ++p) set_arch_bit(p);
  }
  return mask;
}

std::uint32_t mask32(unsigned mb, unsigned me) {
  mb &= 31u;
  me &= 31u;
  std::uint32_t mask = 0;
  auto set_arch_bit = [&mask](unsigned p) { mask |= (1u << (31u - p)); };
  if (mb <= me) {
    for (unsigned p = mb; p <= me; ++p) set_arch_bit(p);
  } else {
    for (unsigned p = mb; p < 32; ++p) set_arch_bit(p);
    for (unsigned p = 0; p <= me; ++p) set_arch_bit(p);
  }
  return mask;
}

}  // namespace

bool Lifter::lift_integer(const DecodedInstruction& i, ir::Builder& b) const {
  const auto m = i.mnemonic();

  auto read_ra = [&] { return b.read_gpr(i.ra()); };
  auto read_rb = [&] { return b.read_gpr(i.rb()); };
  auto write_rt = [&](ValueId v) { b.write_gpr(i.rt(), v); };

  if (m == "addi" || m == "addis") {
    ValueId a = i.ra() ? read_ra() : b.constant_i64(0);
    std::int64_t imm = i.simm16();
    if (m == "addis") imm <<= 16;
    auto c = b.constant_i64(static_cast<std::uint64_t>(imm));
    write_rt(emit_binary(b, Op::Add, a, c));
    return true;
  }
  if (m == "addx" || m == "addcx") {
    auto a = read_ra(); auto c = read_rb();
    auto r = emit_binary(b, Op::Add, a, c);
    write_rt(r);
    if (m == "addcx") emit_ca_from_add(b, a, c, r);
    if (i.oe()) emit_overflow_add(b, a, c, r);
    if (i.rc()) emit_cr0(b, r);
    return true;
  }
  if (m == "addic" || m == "addicx") {
    auto a = read_ra();
    auto c = b.constant_i64(static_cast<std::uint64_t>(static_cast<std::int64_t>(i.simm16())));
    auto r = emit_binary(b, Op::Add, a, c);
    write_rt(r);
    emit_ca_from_add(b, a, c, r);
    if (m == "addicx") emit_cr0(b, r);
    return true;
  }
  if (m == "addex" || m == "addmex" || m == "addzex") {
    auto a=read_ra(); auto ca=read_xer_ca(b); ValueId c{};
    if(m=="addex") c=read_rb();
    else if(m=="addmex") c=b.constant_i64(~0ull);
    else c=b.constant_i64(0);
    const ValueId args[]={a,c,ca}; auto r=b.emit(Op::AddCarry,Type::I64,args,0,0,&i);
    write_rt(r); set_ca3(b,a,c,ca); if(i.oe()) set_ov3(b,a,c,ca); if(i.rc()) emit_cr0(b,r);
    return true;
  }

  if (m == "subfex" || m == "subfmex" || m == "subfzex") {
    // subfe = RB + ~RA + CA; subfme = -1 + ~RA + CA; subfze = 0 + ~RA + CA.
    auto ra=read_ra(); const ValueId na[]={ra}; auto nra=b.emit(Op::Not,Type::I64,na);
    auto ca=read_xer_ca(b); ValueId lhs{};
    if(m=="subfex") lhs=read_rb();
    else if(m=="subfmex") lhs=b.constant_i64(~0ull);
    else lhs=b.constant_i64(0);
    const ValueId args[]={lhs,nra,ca}; auto r=b.emit(Op::AddCarry,Type::I64,args,0,0,&i);
    write_rt(r); set_ca3(b,lhs,nra,ca); if(i.oe()) set_ov3(b,lhs,nra,ca,1); if(i.rc()) emit_cr0(b,r);
    return true;
  }

  if (m == "subfx" || m == "subfcx") {
    // subf RT,RA,RB => RB - RA.
    auto a = read_rb(); auto c = read_ra();
    auto r = emit_binary(b, Op::Sub, a, c);
    write_rt(r);
    if (m == "subfcx") {
      const ValueId args[] = {a, c};
      auto ca = b.emit(Op::CompareUgt, Type::I1, args); // no-borrow iff a >= c, fixed below
      const ValueId eq_args[] = {a, c};
      auto eq = b.emit(Op::CompareEq, Type::I1, eq_args);
      const ValueId or_args[] = {ca, eq};
      ca = b.emit(Op::Or, Type::I1, or_args);
      const ValueId ca_args[] = {ca};
      b.emit(Op::SetXerCA, Type::Void, ca_args);
    }
    if (i.oe()) emit_overflow_sub(b, a, c, r);
    if (i.rc()) emit_cr0(b, r);
    return true;
  }
  if (m == "subficx") {
    auto a = b.constant_i64(static_cast<std::uint64_t>(static_cast<std::int64_t>(i.simm16())));
    auto c = read_ra();
    auto r = emit_binary(b, Op::Sub, a, c);
    write_rt(r);
    const ValueId gt_args[] = {a, c};
    auto gt = b.emit(Op::CompareUgt, Type::I1, gt_args);
    const ValueId eq_args[] = {a, c};
    auto eq = b.emit(Op::CompareEq, Type::I1, eq_args);
    const ValueId or_args[] = {gt, eq};
    auto ca = b.emit(Op::Or, Type::I1, or_args);
    const ValueId ca_args[] = {ca};
    b.emit(Op::SetXerCA, Type::Void, ca_args);
    return true;
  }
  if (m == "mulli") {
    auto a=read_ra(); auto c=b.constant_i64(static_cast<std::uint64_t>(static_cast<std::int64_t>(i.simm16())));
    write_rt(emit_binary(b,Op::Mul,a,c)); return true;
  }
  if (m == "mulldx" || m == "mullwx") {
    const unsigned width=m=="mullwx"?32u:64u; auto a=read_ra(), c=read_rb();
    if(width==32){ const ValueId aa[]={a},cc[]={c}; a=b.emit(Op::Truncate,Type::I32,aa); c=b.emit(Op::Truncate,Type::I32,cc); }
    const ValueId args[]={a,c}; auto r=b.emit(Op::Mul,width==32?Type::I32:Type::I64,args,0,0,&i);
    if(width==32){ const ValueId rr[]={r}; r=b.emit(Op::SignExtend,Type::I64,rr); }
    write_rt(r);
    if(i.oe()){ const ValueId ovargs[]={a,c}; auto ov=b.emit(Op::MulOverflowSigned,Type::I1,ovargs,width,0,&i); const ValueId z[]={ov}; b.emit(Op::SetXerOverflowSticky,Type::Void,z); }
    if(i.rc()) emit_cr0(b,r);
    return true;
  }
  if (m == "mulhdx" || m == "mulhdux" || m == "mulhwx" || m == "mulhwux") {
    const bool word=m.find("hw")!=std::string_view::npos; const bool uns=m.find("hdu")!=std::string_view::npos || m.find("hwu")!=std::string_view::npos;
    auto a=read_ra(),c=read_rb(); Type t=word?Type::I32:Type::I64;
    if(word){ const ValueId aa[]={a},cc[]={c}; a=b.emit(Op::Truncate,t,aa); c=b.emit(Op::Truncate,t,cc); }
    const ValueId args[]={a,c}; auto r=b.emit(uns?Op::MulHighUnsigned:Op::MulHighSigned,t,args,0,0,&i);
    if(word){ const ValueId rr[]={r}; r=b.emit(uns?Op::ZeroExtend:Op::SignExtend,Type::I64,rr); }
    write_rt(r); if(i.rc()) emit_cr0(b,r); return true;
  }
  if (m == "divdx" || m == "divdux" || m == "divwx" || m == "divwux") {
    const bool word=m.find("w")!=std::string_view::npos; const bool uns=m.find("du")!=std::string_view::npos || m.find("wu")!=std::string_view::npos;
    auto a=read_ra(),c=read_rb(); Type t=word?Type::I32:Type::I64;
    if(word){ const ValueId aa[]={a},cc[]={c}; a=b.emit(Op::Truncate,t,aa); c=b.emit(Op::Truncate,t,cc); }
    const ValueId args[]={a,c}; auto r=b.emit(uns?Op::DivUnsigned:Op::DivSigned,t,args,word?32u:64u,i.oe()?1u:0u,&i);
    if(word){ const ValueId rr[]={r}; r=b.emit(uns?Op::ZeroExtend:Op::SignExtend,Type::I64,rr); }
    write_rt(r);
    if(i.oe()){ auto ov=b.emit(Op::DivOverflow,Type::I1,args,word?32u:64u,uns?1u:0u,&i); const ValueId z[]={ov}; b.emit(Op::SetXerOverflowSticky,Type::Void,z); }
    if(i.rc()) emit_cr0(b,r);
    return true;
  }

  if (m == "negx") {
    auto a = read_ra();
    const ValueId args[] = {a};
    auto r = b.emit(Op::Neg, Type::I64, args);
    write_rt(r);
    if (i.oe()) {
      auto minv = b.constant_i64(0x8000000000000000ull);
      const ValueId cmp[] = {a, minv};
      auto ov = b.emit(Op::CompareEq, Type::I1, cmp);
      const ValueId oa[] = {ov};
      b.emit(Op::SetXerOverflowSticky, Type::Void, oa);
    }
    if (i.rc()) emit_cr0(b, r);
    return true;
  }

  if (m == "andx" || m == "andcx" || m == "orx" || m == "orcx" ||
      m == "xorx" || m == "eqvx" || m == "nandx" || m == "norx") {
    auto a = b.read_gpr(i.rs()); auto c = read_rb();
    ValueId r{};
    if (m == "andx") r = emit_binary(b, Op::And, a, c);
    else if (m == "andcx") {
      const ValueId na[] = {c}; auto nc = b.emit(Op::Not, Type::I64, na);
      r = emit_binary(b, Op::And, a, nc);
    } else if (m == "orx") r = emit_binary(b, Op::Or, a, c);
    else if (m == "orcx") {
      const ValueId na[] = {c}; auto nc = b.emit(Op::Not, Type::I64, na);
      r = emit_binary(b, Op::Or, a, nc);
    } else if (m == "xorx") r = emit_binary(b, Op::Xor, a, c);
    else if (m == "eqvx") {
      auto x = emit_binary(b, Op::Xor, a, c);
      const ValueId na[] = {x}; r = b.emit(Op::Not, Type::I64, na);
    } else if (m == "nandx") {
      auto x = emit_binary(b, Op::And, a, c);
      const ValueId na[] = {x}; r = b.emit(Op::Not, Type::I64, na);
    } else {
      auto x = emit_binary(b, Op::Or, a, c);
      const ValueId na[] = {x}; r = b.emit(Op::Not, Type::I64, na);
    }
    b.write_gpr(i.ra(), r);
    if (i.rc()) emit_cr0(b, r);
    return true;
  }

  if (m == "ori" || m == "oris" || m == "xori" || m == "xoris" ||
      m == "andix" || m == "andisx") {
    auto a = b.read_gpr(i.rs());
    std::uint64_t imm = i.uimm16();
    if (m == "oris" || m == "xoris" || m == "andisx") imm <<= 16;
    auto c = b.constant_i64(imm);
    Op op = (m == "ori" || m == "oris") ? Op::Or :
            (m == "xori" || m == "xoris") ? Op::Xor : Op::And;
    auto r = emit_binary(b, op, a, c);
    b.write_gpr(i.ra(), r);
    if (m == "andix" || m == "andisx") emit_cr0(b, r);
    return true;
  }

  if (m == "extsbx" || m == "extshx" || m == "extswx") {
    auto a = b.read_gpr(i.rs());
    const ValueId aa[] = {a};
    auto narrowed = b.emit(Op::Truncate,
        m == "extsbx" ? Type::I8 : (m == "extshx" ? Type::I16 : Type::I32), aa);
    const ValueId na[] = {narrowed};
    auto r = b.emit(Op::SignExtend, Type::I64, na);
    b.write_gpr(i.ra(), r);
    if (i.rc()) emit_cr0(b, r);
    return true;
  }

  if (m == "cntlzdx" || m == "cntlzwx") {
    auto src = b.read_gpr(i.rs());
    Type t = m == "cntlzwx" ? Type::I32 : Type::I64;
    if (t == Type::I32) { const ValueId a[] = {src}; src = b.emit(Op::Truncate, Type::I32, a); }
    const ValueId a[] = {src}; auto r = b.emit(Op::CountLeadingZeros, t, a, 0, 0, &i);
    if (t == Type::I32) { const ValueId z[] = {r}; r = b.emit(Op::ZeroExtend, Type::I64, z); }
    b.write_gpr(i.ra(), r); if (i.rc()) emit_cr0(b, r); return true;
  }

  if (m == "cmp" || m == "cmpl" || m == "cmpi" || m == "cmpli") {
    ValueId a{}; ValueId c{};
    bool logical = (m == "cmpl" || m == "cmpli");
    bool is_imm = (m == "cmpi" || m == "cmpli");
    const bool word = ((i.word >> 21) & 1u) == 0; // L=0 => word, L=1 => doubleword.
    a = read_ra();
    if (is_imm) {
      c = b.constant_i64(logical ? i.uimm16()
                                 : static_cast<std::uint64_t>(static_cast<std::int64_t>(i.simm16())));
    } else c = read_rb();
    if (word) {
      const ValueId av[] = {a}; const ValueId cv[] = {c};
      a = b.emit(Op::Truncate, Type::I32, av);
      c = b.emit(Op::Truncate, Type::I32, cv);
    }
    const ValueId pair[] = {a, c};
    auto lt = b.emit(logical ? Op::CompareUlt : Op::CompareSlt, Type::I1, pair);
    auto gt = b.emit(logical ? Op::CompareUgt : Op::CompareSgt, Type::I1, pair);
    auto eq = b.emit(Op::CompareEq, Type::I1, pair);
    auto xer = b.emit(Op::ReadXER, Type::I32);
    const ValueId fields[] = {lt, gt, eq, xer};
    b.emit(Op::WriteCRCompare, Type::Void, fields, i.crfd());
    return true;
  }

  if (m == "rlwinmx" || m == "rlwimix" || m == "rlwnmx") {
    auto src64 = b.read_gpr(i.rs());
    const ValueId ta[] = {src64};
    auto src = b.emit(Op::Truncate, Type::I32, ta);
    ValueId shift{};
    if (m == "rlwnmx") {
      auto rb = read_rb(); const ValueId rb_a[] = {rb};
      shift = b.emit(Op::Truncate, Type::I32, rb_a);
    } else shift = b.constant_i64(i.sh32());
    const ValueId rot_a[] = {src, shift};
    auto rot = b.emit(Op::Rotl, Type::I32, rot_a);
    auto mask = b.constant_i64(mask32(i.mb32(), i.me32()));
    const ValueId and_a[] = {rot, mask};
    auto selected = b.emit(Op::And, Type::I32, and_a);
    ValueId result = selected;
    if (m == "rlwimix") {
      auto old64 = b.read_gpr(i.ra()); const ValueId olda[] = {old64};
      auto old = b.emit(Op::Truncate, Type::I32, olda);
      auto invmask = b.constant_i64(~static_cast<std::uint64_t>(mask32(i.mb32(), i.me32())) & 0xFFFFFFFFu);
      const ValueId oldm_a[] = {old, invmask};
      auto kept = b.emit(Op::And, Type::I32, oldm_a);
      const ValueId or_a[] = {kept, selected};
      result = b.emit(Op::Or, Type::I32, or_a);
    }
    const ValueId z[] = {result};
    auto wide = b.emit(Op::ZeroExtend, Type::I64, z);
    b.write_gpr(i.ra(), wide);
    if (i.rc()) emit_cr0(b, wide);
    return true;
  }

  if (m == "rldclx" || m == "rldcrx") {
    auto src=b.read_gpr(i.rs()); auto rb=read_rb(); auto sixty3=b.constant_i64(63);
    const ValueId ma[]={rb,sixty3}; auto sh=b.emit(Op::And,Type::I64,ma);
    const ValueId ra[]={src,sh}; auto rot=b.emit(Op::Rotl,Type::I64,ra);
    const unsigned field=i.mb64_md();
    auto mask=b.constant_i64(m=="rldclx"?mask64(field,63):mask64(0,field));
    const ValueId aa[]={rot,mask}; auto r=b.emit(Op::And,Type::I64,aa); b.write_gpr(i.ra(),r);
    if(i.rc()) emit_cr0(b,r);
    return true;
  }

  if (m == "rldiclx" || m == "rldicrx" || m == "rldicx" || m == "rldimix") {
    auto src = b.read_gpr(i.rs());
    auto sh = b.constant_i64(i.sh64_md());
    const ValueId rot_a[] = {src, sh};
    auto rot = b.emit(Op::Rotl, Type::I64, rot_a);
    const unsigned field = i.mb64_md();
    unsigned mb = field;
    unsigned me = 63;
    if (m == "rldicrx") {
      // MD-form rldicr encodes ME in the field occupied by MB in rldicl/rldic/rldimi.
      mb = 0;
      me = field;
    } else if (m == "rldicx" || m == "rldimix") {
      me = static_cast<unsigned>(63 - i.sh64_md());
    }
    auto mask = b.constant_i64(mask64(mb, me));
    const ValueId and_a[] = {rot, mask};
    auto selected = b.emit(Op::And, Type::I64, and_a);
    ValueId result = selected;
    if (m == "rldimix") {
      auto old = b.read_gpr(i.ra());
      auto inv = b.constant_i64(~mask64(mb, me));
      const ValueId old_a[] = {old, inv};
      auto kept = b.emit(Op::And, Type::I64, old_a);
      const ValueId or_a[] = {kept, selected};
      result = b.emit(Op::Or, Type::I64, or_a);
    }
    b.write_gpr(i.ra(), result);
    if (i.rc()) emit_cr0(b, result);
    return true;
  }

  if (m == "sync" || m == "eieio" || m == "isync") {
    std::uint64_t kind{};
    if (m == "sync") {
      // PowerPC sync L=0 heavyweight, L=1 lightweight. Preserve other L
      // encodings for the backend rather than silently weakening them.
      const unsigned l=(i.word>>21)&0x3u;
      kind = l==1 ? 2u : 1u;
    } else kind = m == "eieio" ? 3u : 4u;
    b.emit(Op::Barrier, Type::Void, {}, kind, (i.word>>21)&0x3u, &i);
    return true;
  }

  if (m == "slwx" || m == "srwx" || m == "srawx" || m == "srawix" ||
      m == "sldx" || m == "srdx" || m == "sradx" || m == "sradix") {
    auto src=b.read_gpr(i.rs()); ValueId sh{};
    if(m=="srawix") sh=b.constant_i64(i.rb());
    else if(m=="sradix") sh=b.constant_i64(i.sh64_md());
    else sh=read_rb();
    const ValueId args[]={src,sh}; auto r=b.emit(Op::PpcShift,Type::I64,args,i.info->pattern,i.word,&i);
    b.write_gpr(i.ra(),r);
    if(m.starts_with("sra")){ auto ca=b.emit(Op::PpcShiftCarry,Type::I1,args,i.info->pattern,i.word,&i); const ValueId z[]={ca}; b.emit(Op::SetXerCA,Type::Void,z); }
    if(i.rc()) emit_cr0(b,r);
    return true;
  }

  // Multiplication/division and carry-chain forms have precise OE/CA corner
  // cases. They stay as semantic intrinsics until the dedicated arithmetic
  // lowering pass expands them; no runtime fallback is permitted.
  return false;
}

}  // namespace xenon::cpu

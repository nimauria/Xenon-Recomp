#include "xenon/cpu/lifter.hpp"

#include <cstdint>
#include <string_view>

namespace xenon::cpu {
namespace {
using ir::Endian;
using ir::Op;
using ir::Type;
using ir::ValueId;

ValueId add(ir::Builder& b, ValueId a, ValueId c) {
  const ValueId x[] = {a, c};
  return b.emit(Op::Add, Type::I64, x);
}

ValueId add_immediate(ir::Builder& b, ValueId base, std::int64_t displacement) {
  if (displacement == 0) return base;
  const ValueId a[] = {base};
  return b.emit(Op::AddImmediate, Type::I64, a,
                static_cast<std::uint64_t>(displacement));
}

ValueId ea_d(const DecodedInstruction& i, ir::Builder& b, bool update) {
  const auto displacement = static_cast<std::int64_t>(i.simm16());
  if (!update && i.ra() == 0)
    return b.constant_i64(static_cast<std::uint64_t>(displacement));
  return add_immediate(b, b.read_gpr(i.ra()), displacement);
}

ValueId ea_ds(const DecodedInstruction& i, ir::Builder& b, bool update) {
  const auto displacement = static_cast<std::int64_t>(i.ds_displacement());
  if (!update && i.ra() == 0)
    return b.constant_i64(static_cast<std::uint64_t>(displacement));
  return add_immediate(b, b.read_gpr(i.ra()), displacement);
}

ValueId ea_x(const DecodedInstruction& i, ir::Builder& b, bool update) {
  if (!update && i.ra() == 0) return b.read_gpr(i.rb());
  return add(b, b.read_gpr(i.ra()), b.read_gpr(i.rb()));
}

// Memory V2 accepts either I32 or I64 guest addresses and performs the single
// GuestAddress cast at the access boundary. Preserve the full 64-bit EA for
// update-form architectural writes and avoid a redundant IR truncate.
ValueId memory_address(ir::Builder&, ValueId ea) { return ea; }

ValueId load_raw(ir::Builder& b, ValueId ea, Type type, Endian endian, const DecodedInstruction& i) {
  ea = memory_address(b, ea);
  const ValueId a[] = {ea};
  return b.emit(Op::Load, type, a, static_cast<std::uint64_t>(endian), 0, &i);
}

void store_raw(ir::Builder& b, ValueId ea, ValueId v, Type type, Endian endian,
               const DecodedInstruction& i) {
  ea = memory_address(b, ea);
  const ValueId a[] = {ea, v};
  b.emit(Op::Store, Type::Void, a,
         static_cast<std::uint64_t>(endian), static_cast<std::uint64_t>(type), &i);
}

ValueId extend(ir::Builder& b, ValueId v, Type src, bool sign) {
  if (src == Type::I64) return v;
  const ValueId a[] = {v};
  return b.emit(sign ? Op::SignExtend : Op::ZeroExtend, Type::I64, a);
}

Type width_type(unsigned bytes) {
  switch (bytes) {
    case 1: return Type::I8;
    case 2: return Type::I16;
    case 4: return Type::I32;
    default: return Type::I64;
  }
}

bool scalar_load_desc(std::string_view m, unsigned& bytes, bool& sign, bool& update,
                      bool& indexed, Endian& endian) {
  bytes = 0; sign = false; update = false; indexed = false; endian = Endian::Big;
#define L(N,B,S,U,X) if (m == N) { bytes=B; sign=S; update=U; indexed=X; return true; }
  L("lbz",1,false,false,false) L("lbzu",1,false,true,false)
  L("lbzx",1,false,false,true) L("lbzux",1,false,true,true)
  L("lha",2,true,false,false) L("lhau",2,true,true,false)
  L("lhax",2,true,false,true) L("lhaux",2,true,true,true)
  L("lhz",2,false,false,false) L("lhzu",2,false,true,false)
  L("lhzx",2,false,false,true) L("lhzux",2,false,true,true)
  L("lwa",4,true,false,false) L("lwax",4,true,false,true) L("lwaux",4,true,true,true)
  L("lwz",4,false,false,false) L("lwzu",4,false,true,false)
  L("lwzx",4,false,false,true) L("lwzux",4,false,true,true)
  L("ld",8,false,false,false) L("ldu",8,false,true,false)
  L("ldx",8,false,false,true) L("ldux",8,false,true,true)
#undef L
  if (m == "lhbrx") { bytes=2; indexed=true; endian=Endian::Little; return true; }
  if (m == "lwbrx") { bytes=4; indexed=true; endian=Endian::Little; return true; }
  if (m == "ldbrx") { bytes=8; indexed=true; endian=Endian::Little; return true; }
  return false;
}

bool scalar_store_desc(std::string_view m, unsigned& bytes, bool& update,
                       bool& indexed, Endian& endian) {
  bytes=0; update=false; indexed=false; endian=Endian::Big;
#define S(N,B,U,X) if (m == N) { bytes=B; update=U; indexed=X; return true; }
  S("stb",1,false,false) S("stbu",1,true,false) S("stbx",1,false,true) S("stbux",1,true,true)
  S("sth",2,false,false) S("sthu",2,true,false) S("sthx",2,false,true) S("sthux",2,true,true)
  S("stw",4,false,false) S("stwu",4,true,false) S("stwx",4,false,true) S("stwux",4,true,true)
  S("std",8,false,false) S("stdu",8,true,false) S("stdx",8,false,true) S("stdux",8,true,true)
#undef S
  if (m == "sthbrx") { bytes=2; indexed=true; endian=Endian::Little; return true; }
  if (m == "stwbrx") { bytes=4; indexed=true; endian=Endian::Little; return true; }
  if (m == "stdbrx") { bytes=8; indexed=true; endian=Endian::Little; return true; }
  return false;
}

unsigned vector_reg(const DecodedInstruction& i) {
  switch (i.info->format) {
    case InstructionFormat::VX128_1: return i.vx128_vd();
    default: return i.vd5();
  }
}

} // namespace

bool Lifter::lift_memory(const DecodedInstruction& i, ir::Builder& b) const {
  const auto m = i.mnemonic();

  unsigned bytes{}; bool sign{}, update{}, indexed{}; Endian endian{};
  if (scalar_load_desc(m, bytes, sign, update, indexed, endian)) {
    ValueId ea{};
    if (i.info->format == InstructionFormat::DS) ea = ea_ds(i, b, update);
    else ea = indexed ? ea_x(i, b, update) : ea_d(i, b, update);
    auto raw = load_raw(b, ea, width_type(bytes), endian, i);
    auto value = extend(b, raw, width_type(bytes), sign);
    b.write_gpr(i.rt(), value);
    if (update) b.write_gpr(i.ra(), ea);
    return true;
  }

  if (scalar_store_desc(m, bytes, update, indexed, endian)) {
    ValueId ea{};
    if (i.info->format == InstructionFormat::DS) ea = ea_ds(i, b, update);
    else ea = indexed ? ea_x(i, b, update) : ea_d(i, b, update);
    auto value = b.read_gpr(i.rs());
    if (bytes != 8) {
      const ValueId a[] = {value}; value = b.emit(Op::Truncate, width_type(bytes), a);
    }
    store_raw(b, ea, value, width_type(bytes), endian, i);
    if (update) b.write_gpr(i.ra(), ea);
    return true;
  }

  // Multiple-register forms expand statically, so there is no runtime loop over opcodes.
  if (m == "lmw" || m == "stmw") {
    auto base = i.ra() ? b.read_gpr(i.ra()) : b.constant_i64(0);
    for (unsigned r = i.rt(); r < 32; ++r) {
      const auto displacement = static_cast<std::int64_t>(i.simm16()) +
                                static_cast<std::int64_t>((r - i.rt()) * 4);
      auto ea = add_immediate(b, base, displacement);
      if (m == "lmw") {
        auto raw = load_raw(b, ea, Type::I32, Endian::Big, i);
        b.write_gpr(r, extend(b, raw, Type::I32, false));
      } else {
        auto v = b.read_gpr(r); const ValueId a[] = {v};
        v = b.emit(Op::Truncate, Type::I32, a);
        store_raw(b, ea, v, Type::I32, Endian::Big, i);
      }
    }
    return true;
  }

  // String loads/stores have architectural byte-count/register-wrap semantics.
  // Preserve them as explicit generic IR operations; these are not runtime opcode dispatch.
  if (m == "lswi" || m == "lswx" || m == "stswi" || m == "stswx") {
    const bool indexed_string = (m == "lswx" || m == "stswx");
    auto ea = indexed_string ? ea_x(i, b, false)
                             : (i.ra() ? b.read_gpr(i.ra()) : b.constant_i64(0));
    ValueId count{};
    if (indexed_string) {
      auto xer = b.emit(Op::ReadXER, Type::I32);
      auto mask = b.constant_i32(0x7Fu); const ValueId aa[] = {xer, mask};
      count = b.emit(Op::And, Type::I32, aa);
    } else {
      const unsigned nb = i.rb() == 0 ? 32u : i.rb();
      count = b.constant_i32(nb);
    }
    const ValueId args[] = {ea, count};
    b.emit((m == "lswi" || m == "lswx") ? Op::StringLoad : Op::StringStore,
           Type::Void, args, i.rt(), indexed_string ? 1u : 0u, &i);
    return true;
  }

  // Floating-point memory operations carry bit-exact FPR state in CpuState.
  const bool fp_load = m == "lfd" || m == "lfdu" || m == "lfdx" || m == "lfdux" ||
                       m == "lfs" || m == "lfsu" || m == "lfsx" || m == "lfsux";
  if (fp_load) {
    const bool single = m.starts_with("lfs");
    update = m == "lfdu" || m == "lfdux" || m == "lfsu" || m == "lfsux";
    indexed = m.ends_with("x");
    auto ea = indexed ? ea_x(i,b,update) : ea_d(i,b,update);
    ValueId bits{};
    if (single) {
      auto raw = load_raw(b, ea, Type::I32, Endian::Big, i);
      const ValueId ba[] = {raw};
      auto f32 = b.emit(Op::Bitcast, Type::F32, ba);
      const ValueId fa[] = {f32}; auto f64 = b.emit(Op::FPromote, Type::F64, fa);
      const ValueId da[] = {f64}; bits = b.emit(Op::Bitcast, Type::I64, da);
    } else bits = load_raw(b, ea, Type::I64, Endian::Big, i);
    b.write_fpr_bits(i.frt(), bits);
    if (update) b.write_gpr(i.ra(), ea);
    return true;
  }

  const bool fp_store = m == "stfd" || m == "stfdu" || m == "stfdx" || m == "stfdux" ||
                        m == "stfs" || m == "stfsu" || m == "stfsx" || m == "stfsux";
  if (fp_store) {
    const bool single = m.starts_with("stfs");
    update = m == "stfdu" || m == "stfdux" || m == "stfsu" || m == "stfsux";
    indexed = m.ends_with("x");
    auto ea = indexed ? ea_x(i,b,update) : ea_d(i,b,update);
    auto bits = b.read_fpr_bits(i.frs());
    if (single) {
      const ValueId ba[] = {bits}; auto f64 = b.emit(Op::Bitcast, Type::F64, ba);
      const ValueId fa[] = {f64}; auto f32 = b.emit(Op::FTruncate, Type::F32, fa);
      const ValueId fb[] = {f32}; auto raw = b.emit(Op::Bitcast, Type::I32, fb);
      store_raw(b, ea, raw, Type::I32, Endian::Big, i);
    } else store_raw(b, ea, bits, Type::I64, Endian::Big, i);
    if (update) b.write_gpr(i.ra(), ea);
    return true;
  }

  if (m == "stfiwx") {
    auto ea = ea_x(i,b,false); auto bits = b.read_fpr_bits(i.frs());
    const ValueId a[] = {bits}; auto low = b.emit(Op::Truncate, Type::I32, a);
    store_raw(b, ea, low, Type::I32, Endian::Big, i);
    return true;
  }

  if (m == "lwarx" || m == "ldarx") {
    auto ea = ea_x(i,b,false); ea=memory_address(b,ea); const ValueId a[] = {ea};
    const Type t = m == "lwarx" ? Type::I32 : Type::I64;
    auto raw = b.emit(Op::ReserveLoad, t, a, static_cast<std::uint64_t>(Endian::Big), 0, &i);
    b.write_gpr(i.rt(), extend(b, raw, t, false));
    return true;
  }
  if (m == "stwcx" || m == "stdcx") {
    auto ea = ea_x(i,b,false); ea=memory_address(b,ea); auto value = b.read_gpr(i.rs());
    const Type t = m == "stwcx" ? Type::I32 : Type::I64;
    if (t == Type::I32) { const ValueId a[] = {value}; value = b.emit(Op::Truncate, Type::I32, a); }
    const ValueId a[] = {ea, value};
    auto ok = b.emit(Op::StoreConditional, Type::I1, a,
                     static_cast<std::uint64_t>(Endian::Big), static_cast<std::uint64_t>(t), &i);
    // CR0 = 00 | success | XER.SO. Use the same typed compare-field IR as
    // integer compares so CR forwarding can consume EQ without a state round-trip.
    auto no = b.constant_i1(false);
    auto so = b.emit(Op::ReadXerSO, Type::I1);
    const ValueId cr_args[] = {no, no, ok, so};
    b.emit(Op::WriteCRCompare, Type::Void, cr_args, 0, 0, &i);
    return true;
  }

  // VMX/VMX128 memory family. Complex alignment/partial semantics are explicit IR ops.
  if (m == "lvx" || m == "lvxl" || m == "lvx128" || m == "lvxl128") {
    auto ea = ea_x(i,b,false);
    auto align_mask=b.constant_i64(~0xFull); const ValueId ma[]={ea,align_mask}; ea=b.emit(Op::And,Type::I64,ma);
    ea=memory_address(b,ea); const ValueId a[] = {ea};
    auto v = b.emit(Op::Load, Type::V128, a, static_cast<std::uint64_t>(Endian::Raw), 16, &i);
    b.write_vector(vector_reg(i), v); return true;
  }
  if (m == "stvx" || m == "stvxl" || m == "stvx128" || m == "stvxl128") {
    auto ea = ea_x(i,b,false);
    auto align_mask=b.constant_i64(~0xFull); const ValueId ma[]={ea,align_mask}; ea=b.emit(Op::And,Type::I64,ma);
    ea=memory_address(b,ea); auto v=b.read_vector(vector_reg(i));
    const ValueId a[] = {ea,v}; b.emit(Op::Store, Type::Void, a,
      static_cast<std::uint64_t>(Endian::Raw), static_cast<std::uint64_t>(Type::V128), &i); return true;
  }
  if (m == "lvebx" || m == "lvehx" || m == "lvewx" || m == "lvewx128") {
    // AltiVec element loads write only the EA-selected element; all other
    // destination elements are architecturally undefined. Xenon preserves the
    // prior register contents for those undefined lanes to give deterministic
    // native-recompilation behaviour without making them observable inputs.
    auto ea = ea_x(i, b, false);
    ea = memory_address(b, ea);
    auto old = b.read_vector(vector_reg(i));
    const ValueId a[] = {old, ea};
    const unsigned width = m == "lvebx" ? 1u : (m == "lvehx" ? 2u : 4u);
    auto v = b.emit(Op::VLoadElement, Type::V128, a, width, 0, &i);
    b.write_vector(vector_reg(i), v);
    return true;
  }
  if (m == "stvebx" || m == "stvehx" || m == "stvewx" || m == "stvewx128") {
    auto ea=ea_x(i,b,false); ea=memory_address(b,ea); auto v=b.read_vector(vector_reg(i)); const ValueId a[]={v,ea};
    unsigned w=m=="stvebx"?1u:(m=="stvehx"?2u:4u); b.emit(Op::VStoreElement,Type::Void,a,w,0,&i); return true;
  }
  if (m.starts_with("lvlx")) {
    auto ea=ea_x(i,b,false); ea=memory_address(b,ea); auto old=b.read_vector(vector_reg(i)); const ValueId a[]={old,ea};
    auto v=b.emit(Op::VLoadLeft,Type::V128,a,m.ends_with("l")?1u:0u,0,&i); b.write_vector(vector_reg(i),v); return true;
  }
  if (m.starts_with("lvrx")) {
    auto ea=ea_x(i,b,false); ea=memory_address(b,ea); auto old=b.read_vector(vector_reg(i)); const ValueId a[]={old,ea};
    auto v=b.emit(Op::VLoadRight,Type::V128,a,m.ends_with("l")?1u:0u,0,&i); b.write_vector(vector_reg(i),v); return true;
  }
  if (m.starts_with("stvlx")) {
    auto ea=ea_x(i,b,false); ea=memory_address(b,ea); auto v=b.read_vector(vector_reg(i)); const ValueId a[]={v,ea};
    b.emit(Op::VStoreLeft,Type::Void,a,m.ends_with("l")?1u:0u,0,&i); return true;
  }
  if (m.starts_with("stvrx")) {
    auto ea=ea_x(i,b,false); ea=memory_address(b,ea); auto v=b.read_vector(vector_reg(i)); const ValueId a[]={v,ea};
    b.emit(Op::VStoreRight,Type::Void,a,m.ends_with("l")?1u:0u,0,&i); return true;
  }

  if (m == "dcbz" || m == "dcbz128") {
    auto ea=ea_x(i,b,false); ea=memory_address(b,ea); const ValueId a[]={ea};
    b.emit(Op::CacheZero,Type::Void,a,m=="dcbz128"?128u:32u,0,&i); return true;
  }
  if (m == "icbi") { auto ea=ea_x(i,b,false); ea=memory_address(b,ea); const ValueId a[]={ea}; b.emit(Op::ICacheInvalidate,Type::Void,a,0,0,&i); return true; }
  if (m == "dcbf" || m == "dcbi" || m == "dcbst" || m == "dcbt" || m == "dcbtst") {
    // No architecturally visible data result for our static-recompilation contract;
    // preserve address/hint as a semantic node for optional host cache optimisation.
    auto ea=ea_x(i,b,false); ea=memory_address(b,ea); const ValueId a[]={ea}; b.emit(Op::CacheHint,Type::Void,a,i.info->pattern,0,&i); return true;
  }

  return false;
}

} // namespace xenon::cpu

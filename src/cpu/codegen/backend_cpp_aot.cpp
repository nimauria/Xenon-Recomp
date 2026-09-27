#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/decoder.hpp"

#include <cctype>
#include <iomanip>
#include <optional>
#include <sstream>
#include <unordered_set>
#include <stdexcept>
#include <string>

namespace xenon::cpu::backend {

std::string alternate_entry_symbol(std::string_view function_name, GuestAddress entry) {
  std::ostringstream out;
  out << function_name << "_entry_" << std::hex << std::uppercase << entry << "_v2";
  return out.str();
}

namespace {
using ir::Instruction;
using ir::Op;
using ir::Type;

std::string v(ir::ValueId id) { return "v" + std::to_string(id); }
std::string u64(std::uint64_t x) {
  std::ostringstream o; o << "0x" << std::hex << std::uppercase << x << "ull"; return o.str();
}
const char* ctype(Type t) {
  switch(t){
    case Type::I1:return "bool"; case Type::I8:return "std::uint8_t";
    case Type::I16:return "std::uint16_t"; case Type::I32:return "std::uint32_t";
    case Type::I64:return "std::uint64_t"; case Type::F32:return "float";
    case Type::F64:return "double"; case Type::V128:return "Vector128";
    case Type::Void:break;
  }
  return "void";
}
unsigned bits(Type t){switch(t){case Type::I1:return 1;case Type::I8:return 8;case Type::I16:return 16;case Type::I32:return 32;default:return 64;}}
std::string arg(const Instruction&i,unsigned n){if(n>=i.args.size())return "{}";return v(i.args[n]);}
std::string decl(const Instruction&i,const std::string&e){return std::string("  [[maybe_unused]] ")+ctype(i.type)+" "+v(i.result)+" = "+e+";\n";}
std::string signed_type(Type t){switch(t){case Type::I8:return "std::int8_t";case Type::I16:return "std::int16_t";case Type::I32:return "std::int32_t";default:return "std::int64_t";}}
std::string_view guest_mnemonic(const Instruction& i) {
  const auto* info = Decoder::opcode_info(i.guest_opcode);
  return info ? info->mnemonic : std::string_view{};
}
std::string enum_name(const Instruction&i){
  const auto mnemonic = guest_mnemonic(i);
  if(mnemonic.empty()) throw std::runtime_error("vector IR missing guest opcode metadata");
  return "aot::VectorSemantic::" + std::string(mnemonic);
}

bool is_vector_compute(Op op){
  return op>=Op::VAdd && op<=Op::VMultiplyEvenOdd;
}

// Check if this is a vector splat operation that can be lowered natively
bool is_vector_splat(Op op) {
  return op == Op::VSplat;
}

std::string emit_one(const Instruction&i, const std::vector<Type>& value_types){
  std::ostringstream o;
  auto bin=[&](const char*op){return decl(i,"("+arg(i,0)+" "+op+" "+arg(i,1)+")");};
  auto src_type=[&](unsigned n)->Type{ return (n<i.args.size() && i.args[n]<value_types.size()) ? value_types[i.args[n]] : Type::I64; };
  switch(i.op){
    case Op::Constant:{
      if(i.type==Type::I1)return decl(i,i.imm0?"true":"false");
      if(i.type==Type::F32)return decl(i,"std::bit_cast<float>(std::uint32_t("+u64(i.imm0)+"))");
      if(i.type==Type::F64)return decl(i,"std::bit_cast<double>(std::uint64_t("+u64(i.imm0)+"))");
      return decl(i,"static_cast<"+std::string(ctype(i.type))+">("+u64(i.imm0)+")");
    }
    case Op::ReadGpr:return decl(i,"state.gpr["+std::to_string(i.imm0&31)+"]");
    case Op::WriteGpr:o<<"  state.gpr["<<(i.imm0&31)<<"] = "<<arg(i,0)<<";\n";break;
    case Op::ReadFprBits:return decl(i,"state.fpr_bits["+std::to_string(i.imm0&31)+"]");
    case Op::WriteFprBits:o<<"  state.fpr_bits["<<(i.imm0&31)<<"] = "<<arg(i,0)<<";\n";break;
    case Op::ReadVector:return decl(i,"state.vr["+std::to_string(i.imm0&127)+"]");
    case Op::WriteVector:o<<"  state.vr["<<(i.imm0&127)<<"] = "<<arg(i,0)<<";\n";break;
    case Op::ReadCR:return decl(i,"state.cr");
    case Op::WriteCR:o<<"  state.cr = "<<arg(i,0)<<";\n";break;
    case Op::ReadCRBit:return decl(i,"state.cr_bit("+std::to_string(i.imm0)+")");
    case Op::WriteCRBit:o<<"  state.set_cr_bit("<<i.imm0<<", "<<arg(i,0)<<");\n";break;
    case Op::ReadCRField:return decl(i,"state.cr_field("+std::to_string(i.imm0)+")");
    case Op::WriteCRField:o<<"  state.set_cr_field("<<i.imm0<<", static_cast<std::uint8_t>("<<arg(i,0)<<"));\n";break;
    case Op::WriteCRCompare:{o<<"  { std::uint8_t f = "<<arg(i,0)<<" ? 0x8u : ("<<arg(i,1)<<" ? 0x4u : ("<<arg(i,2)<<" ? 0x2u : 0u)); if ("<<arg(i,3)<<") f|=1u; state.set_cr_field("<<i.imm0<<", f); }\n";break;}
    case Op::WriteCR0StoreConditional:o<<"  state.set_cr_field(0, static_cast<std::uint8_t>(("<<arg(i,0)<<" ? 0x2u : 0u) | (("<<arg(i,1)<<" & xer_bits::SO) ? 1u : 0u)));\n";break;
    case Op::UpdateCR0Signed:o<<"  state.update_cr0_signed(static_cast<std::uint64_t>("<<arg(i,0)<<"));\n";break;
    case Op::MoveCRFields:{o<<"  { [[maybe_unused]] const std::uint32_t src=static_cast<std::uint32_t>("<<arg(i,0)<<");";for(unsigned f=0;f<8;++f)if(i.imm0&(0x80u>>f))o<<" state.set_cr_field("<<f<<", std::uint8_t((src>>"<<((7-f)*4)<<")&0xFu));";o<<" }\n";break;}
    case Op::MoveXERToCR:o<<"  { std::uint8_t f=(state.xer_so()?8u:0u)|(state.xer_ov()?4u:0u)|(state.xer_ca()?2u:0u); state.set_cr_field("<<i.imm0<<",f); state.xer &= ~(xer_bits::SO|xer_bits::OV|xer_bits::CA); }\n";break;
    case Op::ReadXER:return decl(i,"state.xer");
    case Op::ReadXerCA:return decl(i,"state.xer_ca()");
    case Op::ReadXerOV:return decl(i,"state.xer_ov()");
    case Op::ReadXerSO:return decl(i,"state.xer_so()");
    case Op::WriteXER:o<<"  state.xer = static_cast<std::uint32_t>("<<arg(i,0)<<");\n";break;
    case Op::SetXerCA:o<<"  state.set_xer_ca("<<arg(i,0)<<");\n";break;
    case Op::SetXerOV:o<<"  state.set_xer_ov("<<arg(i,0)<<");\n";break;
    case Op::SetXerSO:o<<"  state.set_xer_so("<<arg(i,0)<<");\n";break;
    case Op::SetXerOverflowSticky:o<<"  state.set_xer_overflow("<<arg(i,0)<<");\n";break;
    case Op::ReadFPSCR:return decl(i,"state.fpscr");
    case Op::WriteFPSCR:o<<"  state.fpscr=static_cast<std::uint32_t>("<<arg(i,0)<<");\n";break;
    case Op::UpdateCR1FromFPSCR:o<<"  state.update_cr1_from_fpscr();\n";break;
    case Op::MoveFPSCRFieldToCR:o<<"  aot::move_fpscr_field_to_cr(state,"<<i.imm0<<","<<i.imm1<<");\n";break;
    case Op::MoveFPSCRToFPRBits:return decl(i,"static_cast<std::uint64_t>("+arg(i,0)+")");
    case Op::SetFPSCRBit:o<<"  aot::set_fpscr_bit(state,"<<i.imm0<<","<<(i.imm1?"true":"false")<<");\n";break;
    case Op::WriteFPSCRFieldImmediate:o<<"  aot::write_fpscr_field(state,"<<i.imm0<<",static_cast<std::uint8_t>("<<i.imm1<<"));\n";break;
    case Op::WriteFPSCRFields:o<<"  aot::write_fpscr_fields(state,static_cast<std::uint8_t>("<<i.imm0<<"),static_cast<std::uint32_t>("<<arg(i,0)<<"));\n";break;
    case Op::ReadVSCR:return decl(i,"state.vscr");
    case Op::WriteVSCR:o<<"  state.vscr=static_cast<std::uint32_t>("<<arg(i,0)<<");\n";break;
    case Op::SetVSCRSaturation:o<<"  if ("<<arg(i,0)<<") state.set_vector_saturated();\n";break;
    case Op::MoveVSCRToVector:return decl(i,"([&]{ Vector128 q{}; q.set_u32_be(3,state.vscr); return q; }())");
    case Op::MoveVectorToVSCR:o<<"  state.vscr="<<arg(i,0)<<".u32_be(3);\n";break;
    case Op::ReadLR:return decl(i,"state.lr"); case Op::WriteLR:o<<"  state.lr="<<arg(i,0)<<";\n";break;
    case Op::ReadCTR:return decl(i,"state.ctr"); case Op::WriteCTR:o<<"  state.ctr="<<arg(i,0)<<";\n";break;
    case Op::ReadMSR:return decl(i,"state.msr"); case Op::WriteMSR:o<<"  aot::write_msr(state,static_cast<std::uint64_t>("<<arg(i,0)<<"),"<<(i.imm0?"true":"false")<<","<<(i.imm1?"true":"false")<<");\n";break;
    case Op::ReadTimeBase:return decl(i,"runtime.read_time_base(state)");
    case Op::ReadVRSAVE:return decl(i,"state.vrsave"); case Op::WriteVRSAVE:o<<"  state.vrsave=static_cast<std::uint32_t>("<<arg(i,0)<<");\n";break;
    case Op::ReadPVR:return decl(i,"state.pvr");
    case Op::ReadSPR:return decl(i,"runtime.read_spr("+std::to_string(i.imm0)+",state)");
    case Op::WriteSPR:o<<"  runtime.write_spr("<<i.imm0<<","<<arg(i,0)<<",state);\n";break;

    case Op::Add:return bin("+");
    case Op::AddImmediate:return decl(i,"static_cast<std::uint64_t>("+arg(i,0)+" + "+u64(i.imm0)+")");
    case Op::Sub:return bin("-"); case Op::Mul:return bin("*");
    case Op::And:return bin("&"); case Op::Or:return bin("|"); case Op::Xor:return bin("^");
    case Op::Not:return i.type==Type::I1 ? decl(i,"!"+arg(i,0)) : decl(i,"static_cast<"+std::string(ctype(i.type))+">(~"+arg(i,0)+")");
    case Op::AddCarry:return decl(i,"static_cast<"+std::string(ctype(i.type))+">(aot::add_carry("+arg(i,0)+","+arg(i,1)+","+arg(i,2)+"))");
    case Op::CarryOut:return decl(i,"aot::carry_out("+arg(i,0)+","+arg(i,1)+","+arg(i,2)+")");
    case Op::SignedOverflow:return decl(i,"aot::signed_add_overflow("+arg(i,0)+","+arg(i,1)+","+arg(i,2)+")");
    case Op::MulHighSigned:return decl(i,"static_cast<"+std::string(ctype(i.type))+">(aot::mul_hi_signed_width("+arg(i,0)+","+arg(i,1)+","+std::to_string(bits(i.type))+"))");
    case Op::MulHighUnsigned:return decl(i,"static_cast<"+std::string(ctype(i.type))+">(aot::mul_hi_unsigned_width("+arg(i,0)+","+arg(i,1)+","+std::to_string(bits(i.type))+"))");
    case Op::MulOverflowSigned:return decl(i,"aot::mul_overflow_signed("+arg(i,0)+","+arg(i,1)+","+std::to_string(i.imm0)+")");
    case Op::DivSigned:return decl(i,"static_cast<"+std::string(ctype(i.type))+">(aot::div_signed("+arg(i,0)+","+arg(i,1)+","+std::to_string(i.imm0)+"))");
    case Op::DivUnsigned:return decl(i,"static_cast<"+std::string(ctype(i.type))+">(aot::div_unsigned("+arg(i,0)+","+arg(i,1)+","+std::to_string(i.imm0)+"))");
    case Op::DivOverflow:return decl(i,"aot::div_overflow("+arg(i,0)+","+arg(i,1)+","+std::to_string(i.imm0)+","+(i.imm1?"true":"false")+")");
    case Op::Neg:return decl(i,"static_cast<"+std::string(ctype(i.type))+">(0-"+arg(i,0)+")");
    case Op::CountLeadingZeros:{std::string fn=bits(i.type)==32?"std::countl_zero(static_cast<std::uint32_t>(":"std::countl_zero(static_cast<std::uint64_t>(";return decl(i,"static_cast<"+std::string(ctype(i.type))+">("+fn+arg(i,0)+")))");}
    case Op::PpcShift:return decl(i,"static_cast<std::uint64_t>(aot::ppc_shift(\""+std::string(guest_mnemonic(i))+"\","+arg(i,0)+","+arg(i,1)+"))");
    case Op::PpcShiftCarry:return decl(i,"aot::ppc_shift_carry(\""+std::string(guest_mnemonic(i))+"\","+arg(i,0)+","+arg(i,1)+")");
    case Op::Shl:return decl(i,"static_cast<"+std::string(ctype(i.type))+">((std::uint64_t("+arg(i,1)+") >= "+std::to_string(bits(i.type))+"u) ? 0 : ("+arg(i,0)+" << "+arg(i,1)+"))");
    case Op::ShrLogical:return decl(i,"static_cast<"+std::string(ctype(i.type))+">((std::uint64_t("+arg(i,1)+") >= "+std::to_string(bits(i.type))+"u) ? 0 : ("+arg(i,0)+" >> "+arg(i,1)+"))");
    case Op::ShrArithmetic:return decl(i,"static_cast<"+std::string(ctype(i.type))+">((std::uint64_t("+arg(i,1)+") >= "+std::to_string(bits(i.type))+"u) ? (static_cast<"+signed_type(i.type)+">("+arg(i,0)+")<0 ? -1 : 0) : (static_cast<"+signed_type(i.type)+">("+arg(i,0)+") >> "+arg(i,1)+"))");
    case Op::Rotl:{const char*fn=bits(i.type)==32?"xenon::cpu::rotate_left32":"xenon::cpu::rotate_left64";return decl(i,std::string(fn)+"("+arg(i,0)+",static_cast<unsigned>("+arg(i,1)+"))");}
    case Op::SignExtend:return decl(i,"static_cast<"+std::string(ctype(i.type))+">(static_cast<"+signed_type(src_type(0))+">("+arg(i,0)+"))");
    case Op::ZeroExtend:return decl(i,"static_cast<"+std::string(ctype(i.type))+">("+arg(i,0)+")");
    case Op::Truncate:return decl(i,"static_cast<"+std::string(ctype(i.type))+">("+arg(i,0)+")");
    case Op::Bitcast:{std::string from=i.type==Type::F32?"std::uint32_t":i.type==Type::F64?"std::uint64_t":i.type==Type::I32?"float":"double";return decl(i,"std::bit_cast<"+std::string(ctype(i.type))+">(static_cast<"+from+">("+arg(i,0)+"))");}
    case Op::FPromote:return decl(i,"static_cast<double>("+arg(i,0)+")");
    case Op::FTruncate:return decl(i,"static_cast<float>("+arg(i,0)+")");
    case Op::CompareEq:return decl(i,"("+arg(i,0)+" == "+arg(i,1)+")"); case Op::CompareNe:return decl(i,"("+arg(i,0)+" != "+arg(i,1)+")");
    case Op::CompareSlt:{auto st=src_type(0);return decl(i,"(static_cast<"+signed_type(st)+">("+arg(i,0)+") < static_cast<"+signed_type(st)+">("+arg(i,1)+"))");}
    case Op::CompareSgt:{auto st=src_type(0);return decl(i,"(static_cast<"+signed_type(st)+">("+arg(i,0)+") > static_cast<"+signed_type(st)+">("+arg(i,1)+"))");}
    case Op::CompareUlt:return decl(i,"("+arg(i,0)+" < "+arg(i,1)+")"); case Op::CompareUgt:return decl(i,"("+arg(i,0)+" > "+arg(i,1)+")");
    case Op::Select:return decl(i,"("+arg(i,0)+" ? "+arg(i,1)+" : "+arg(i,2)+")");

    case Op::FAdd:return decl(i,"aot::fp_add(state,"+arg(i,0)+","+arg(i,1)+")");
    case Op::FSub:return decl(i,"aot::fp_sub(state,"+arg(i,0)+","+arg(i,1)+")");
    case Op::FMul:return decl(i,"aot::fp_mul(state,"+arg(i,0)+","+arg(i,1)+")");
    case Op::FDiv:return decl(i,"aot::fp_div(state,"+arg(i,0)+","+arg(i,1)+")");
    case Op::FFma:{std::string c=((i.imm0&1)?"-":"")+arg(i,2);std::string e="aot::fp_fma(state,"+arg(i,0)+","+arg(i,1)+","+c+")";if(i.imm0&2)e="-("+e+")";return decl(i,e);}
    case Op::FSqrt:return decl(i,"aot::fp_sqrt(state,"+arg(i,0)+")");
    case Op::FAbs:return decl(i,"aot::fp_abs_bits("+arg(i,0)+")"); case Op::FNeg:return decl(i,"aot::fp_neg_bits("+arg(i,0)+")");
    case Op::FSelect:return decl(i,"aot::fp_select("+arg(i,0)+","+arg(i,1)+","+arg(i,2)+")");
    case Op::FRoundSingle:return decl(i,"aot::fp_round_single(state,"+arg(i,0)+")");
    case Op::FConvertFromI64:return decl(i,"aot::fp_from_i64(state,"+arg(i,0)+")");
    case Op::FConvertToI64:return decl(i,"aot::fp_to_integer_bits(state,"+arg(i,0)+","+std::to_string(i.imm1)+","+(i.imm0?"true":"false")+")");
    case Op::FReciprocalEstimate:return decl(i,"aot::fp_reciprocal(state,"+arg(i,0)+")");
    case Op::FReciprocalSqrtEstimate:return decl(i,"aot::fp_rsqrt(state,"+arg(i,0)+")");
    case Op::FCompare:return decl(i,"aot::fp_compare_field("+arg(i,0)+","+arg(i,1)+")");
    case Op::FUpdateStatus:{
      if(i.args.size()>=2) {
        const bool ordered = (i.imm0 == 0x201u);
        o<<"  aot::update_fp_compare_status(state,"<<arg(i,0)<<","<<arg(i,1)<<","<<(ordered?"true":"false")<<");\n";
      } else if((i.imm0 & 0x100u) != 0) {
        o<<"  aot::update_fp_conversion_status(state);\n";
      } else if(i.args.size()==1) {
        o<<"  aot::update_fpscr(state,static_cast<double>("<<arg(i,0)<<"));\n";
      }
      break;
    }

    case Op::VUpdateCR6:o<<"  aot::update_cr6_from_vector_compare(state,"<<arg(i,0)<<");\n";break;
    case Op::VMakeLoadShiftLeft:return decl(i,"aot::vector_load_shift_left(static_cast<std::uint64_t>("+arg(i,0)+"+"+arg(i,1)+"))");
    case Op::VMakeLoadShiftRight:return decl(i,"aot::vector_load_shift_right(static_cast<std::uint64_t>("+arg(i,0)+"+"+arg(i,1)+"))");
    case Op::VLoadElement:return decl(i,"aot::vector_load_element("+arg(i,0)+",memory_access,static_cast<GuestAddress>("+arg(i,1)+"),"+std::to_string(i.imm0)+")");
    case Op::VStoreElement:o<<"  aot::vector_store_element("<<arg(i,0)<<",memory_access,static_cast<GuestAddress>("<<arg(i,1)<<"),"<<i.imm0<<");\n";break;
    case Op::VLoadLeft:return decl(i,"aot::vector_load_left("+arg(i,0)+",memory_access,static_cast<GuestAddress>("+arg(i,1)+"))");
    case Op::VLoadRight:return decl(i,"aot::vector_load_right("+arg(i,0)+",memory_access,static_cast<GuestAddress>("+arg(i,1)+"))");
    case Op::VStoreLeft:o<<"  aot::vector_store_left("<<arg(i,0)<<",memory_access,static_cast<GuestAddress>("<<arg(i,1)<<"));\n";break;
    case Op::VStoreRight:o<<"  aot::vector_store_right("<<arg(i,0)<<",memory_access,static_cast<GuestAddress>("<<arg(i,1)<<"));\n";break;

    case Op::Load:{std::string ea="static_cast<GuestAddress>("+arg(i,0)+")"; if(i.type==Type::I8)return decl(i,"memory_access.read8("+ea+")"); if(i.type==Type::V128)return decl(i,"memory_access.read128("+ea+")"); const bool little=i.imm0==static_cast<std::uint64_t>(ir::Endian::Little);std::string fn="memory_access.read"+std::to_string(bits(i.type))+(little?"_le":"_be");return decl(i,fn+"("+ea+")");}
    case Op::Store:{std::string ea="static_cast<GuestAddress>("+arg(i,0)+")";Type st=static_cast<Type>(i.imm1);if(st==Type::I8)o<<"  memory_access.write8("<<ea<<","<<arg(i,1)<<");\n";else if(st==Type::V128)o<<"  memory_access.write128("<<ea<<","<<arg(i,1)<<");\n";else{bool little=i.imm0==static_cast<std::uint64_t>(ir::Endian::Little);o<<"  memory_access.write"<<bits(st)<<(little?"_le":"_be")<<"("<<ea<<","<<arg(i,1)<<");\n";}break;}
    case Op::ReserveLoad:{
      const bool w=i.type==Type::I32;
      o<<"  if (state.reservation.valid) memory_access.cancel_reservation(state.reservation.token);\n";
      o<<"  std::"<<(w?"uint32_t":"uint64_t")<<" tmp_res_"<<i.result<<"{}; state.reservation.token=memory_access.reserve"<<(w?32:64)<<"(static_cast<GuestAddress>("<<arg(i,0)<<"),tmp_res_"<<i.result<<"); state.reservation.valid=(state.reservation.token != 0u); state.reservation.width="<<(w?4:8)<<"; state.reservation.address=static_cast<GuestAddress>("<<arg(i,0)<<"); state.reservation.observed_value=tmp_res_"<<i.result<<";\n"<<decl(i,"tmp_res_"+std::to_string(i.result));
      break;
    }
    case Op::StoreConditional:{
      const bool w=static_cast<Type>(i.imm1)==Type::I32;
      const std::string ea="static_cast<GuestAddress>("+arg(i,0)+")";
      const auto tmp="tmp_sc_"+std::to_string(i.result);
      o<<"  bool "<<tmp<<" = false;\n";
      o<<"  if (state.reservation.valid) {\n";
      o<<"    if (state.reservation.width == "<<(w?4:8)<<") "<<tmp<<" = memory_access.store_conditional"<<(w?32:64)<<"("<<ea<<",state.reservation.token,static_cast<"<<(w?"std::uint32_t":"std::uint64_t")<<">("<<arg(i,1)<<"));\n";
      o<<"    else memory_access.cancel_reservation(state.reservation.token);\n";
      o<<"  }\n";
      o<<decl(i,tmp)<<"  state.reservation.clear();\n";
      break;
    }
    case Op::StringLoad:o<<"  aot::string_load(state,memory_access,static_cast<GuestAddress>("<<arg(i,0)<<"),static_cast<std::uint32_t>("<<arg(i,1)<<"),"<<i.imm0<<");\n";break;
    case Op::StringStore:o<<"  aot::string_store(state,memory_access,static_cast<GuestAddress>("<<arg(i,0)<<"),static_cast<std::uint32_t>("<<arg(i,1)<<"),"<<i.imm0<<");\n";break;
    case Op::Barrier:{
      const char* k=i.imm0==1?"Sync":i.imm0==2?"LightweightSync":i.imm0==3?"Eieio":"InstructionSync";
      o<<"  memory_access.barrier(BarrierKind::"<<k<<");\n";
      if(i.imm0==4){
        // isync discards already-fetched guest instructions. Recompiled guest
        // code therefore leaves the current native translation after the host
        // instruction barrier so the dispatcher/code cache can validate the
        // next guest instruction against executable-page generations.
        const auto next=static_cast<GuestAddress>(i.guest_address+4u);
        o<<"  state.nia="<<next<<"u;\n";
        o<<"  return {FlowReason::Branch,state.nia,0};\n";
      }
      break;
    }
    case Op::CacheZero:o<<"  memory_access.zero_cache_block(static_cast<GuestAddress>("<<arg(i,0)<<"),"<<i.imm0<<");\n";break;
    case Op::ICacheInvalidate:o<<"  memory_access.instruction_cache_invalidate(static_cast<GuestAddress>("<<arg(i,0)<<"));\n";break;
    case Op::CacheHint:o<<"  (void)"<<arg(i,0)<<";\n";break;

    case Op::Branch:o<<"  { const auto branch_target=static_cast<GuestAddress>("<<arg(i,0)<<"); state.nia=branch_target; return {FlowReason::Branch,branch_target,0}; }\n";break;
    // A direct `bl` target is a compile-time constant, so try the local CPU
    // V2 compiled registry first - exactly the same "local lookup, else
    // runtime.call() fallback" pattern already used below for
    // CallIndirect/linked BranchIndirect. Without this, a `bl` to another
    // function discovered/compiled in the SAME module always fell straight
    // to runtime.call() (there is no CPU V1 code_cache_ registration for a
    // native-extension-loaded title - see XenonSession::call()), which found
    // nothing, wasn't a recognized import either, and used to return a
    // non-terminal Branch that this call site's `if(rr.terminal())` check
    // silently swallowed - i.e. every local guest-to-guest `bl` silently
    // no-op'd instead of running the callee. This was a real, previously
    // undetected defect: the two-import fixture that first proved the
    // import-thunk bridge never happened to exercise a local-to-local call.
    case Op::Call:{
      o<<"  { const auto call_target=static_cast<GuestAddress>("<<arg(i,0)<<"); "
       <<"if(auto* native=context.lookup_compiled(call_target,CompiledLookupKind::Call)) { "
       <<"auto rr=native(context); if(rr.terminal()) return rr; } else { "
       <<"auto fallback=context.try_dynamic_fallback(call_target,CompiledLookupKind::Call); "
       <<"auto rr=fallback.handled ? fallback.result : runtime.call(call_target,state,memory); "
       <<"if(rr.terminal()) return rr; } }\n";
      break;
    }
    case Op::BranchIf:{
      o<<"  if("<<arg(i,0)<<") { ";
      if(i.imm0){
        o<<"const auto call_target=static_cast<GuestAddress>("<<arg(i,1)<<"); "
         <<"ExecutionResult rr{}; "
         <<"if(auto* native=context.lookup_compiled(call_target,CompiledLookupKind::Call)) rr=native(context); "
         <<"else { auto fallback=context.try_dynamic_fallback(call_target,CompiledLookupKind::Call); "
         <<"rr=fallback.handled ? fallback.result : runtime.call(call_target,state,memory); } "
         <<"if(rr.terminal()) return rr;";
      }else{
        o<<"const auto branch_target=static_cast<GuestAddress>("<<arg(i,1)<<"); state.nia=branch_target; return {FlowReason::Branch,branch_target,0};";
      }
      o<<" }\n";
      break;
    }
    case Op::BranchIndirect:{
      o<<"  if("<<arg(i,0)<<") { ";
      if(i.imm0){
        const auto expected=static_cast<GuestAddress>(i.guest_address+4u);
        o<<"const auto raw_target=static_cast<GuestAddress>("<<arg(i,1)<<"); "
         <<"const auto guest_target=static_cast<GuestAddress>(raw_target & ~3u); "
         <<"if(auto* native=context.lookup_compiled(guest_target,CompiledLookupKind::Call)) { "
         <<"auto rr=native(context); if(rr.reason != FlowReason::Return || rr.next_address != "
         <<expected<<"u) return rr; } else { auto fallback=context.try_dynamic_fallback(guest_target,CompiledLookupKind::Call); "
         <<"auto rr=fallback.handled ? fallback.result : runtime.call(raw_target,state,memory); "
         <<"if(rr.reason==FlowReason::Return) { if(rr.next_address != "<<expected<<"u) return rr; } "
         <<"else if(rr.reason!=FlowReason::Fallthrough) return rr; }";
      }else if(i.imm1==1){
        o<<"const auto return_target=static_cast<GuestAddress>("<<arg(i,1)<<" & ~3ull); state.nia=return_target; return {FlowReason::Return,return_target,0};";
      }else{
        o<<"const auto guest_target=static_cast<GuestAddress>("<<arg(i,1)<<" & ~3ull); "
         <<"if(auto* native=context.lookup_compiled(guest_target,CompiledLookupKind::Branch)) "
         <<"return native(context); auto fallback=context.try_dynamic_fallback(guest_target,CompiledLookupKind::Branch); "
         <<"if(fallback.handled) return fallback.result; state.nia=guest_target; return {FlowReason::Branch,guest_target,0};";
      }
      o<<" }\n";break;
    }
    case Op::CallIndirect:{
      o<<"  { const auto raw_target=static_cast<GuestAddress>("<<arg(i,0)<<"); "
       <<"const auto guest_target=static_cast<GuestAddress>(raw_target & ~3u); "
       <<"if(auto* native=context.lookup_compiled(guest_target,CompiledLookupKind::Call)) { "
       <<"auto rr=native(context); if(rr.reason != FlowReason::Return || rr.next_address != "
       <<"static_cast<GuestAddress>(state.lr & ~3ull)) return rr; } else { "
       <<"auto fallback=context.try_dynamic_fallback(guest_target,CompiledLookupKind::Call); "
       <<"auto rr=fallback.handled ? fallback.result : runtime.call(raw_target,state,memory); "
       <<"if(rr.reason==FlowReason::Return) { if(rr.next_address != static_cast<GuestAddress>(state.lr & ~3ull)) return rr; } "
       <<"else if(rr.reason!=FlowReason::Fallthrough) return rr; } }\n";break;
    }
    case Op::Return:o<<"  { const auto return_target=static_cast<GuestAddress>(state.lr & ~3ull); state.nia=return_target; return {FlowReason::Return,return_target,0}; }\n";break;
    case Op::Syscall:o<<"  return runtime.syscall("<<i.imm0<<",state,memory);\n";break;
    case Op::Trap:{const std::uint32_t w=i.guest_word;const auto m=guest_mnemonic(i);const bool word=m=="tw"||m=="twi";const bool imm=m=="tdi"||m=="twi";unsigned ra=(w>>16)&31u;unsigned rb=(w>>11)&31u;std::int64_t simm=static_cast<std::int16_t>(w&0xFFFFu);o<<"  if(aot::trap_condition("<<i.imm0<<",state.gpr["<<ra<<"],"<<(imm?("std::uint64_t(std::int64_t("+std::to_string(simm)+"))"):("state.gpr["+std::to_string(rb)+"]"))<<","<<(word?"true":"false")<<")) return runtime.trap("<<i.imm0<<",state,memory);\n";break;}
    default:
      if (i.op == Op::VSelect)
        return decl(i, "aot::vector_select(" + arg(i,0) + "," + arg(i,1) + "," + arg(i,2) + ")");
      if (i.op == Op::VAnd || i.op == Op::VAndNot || i.op == Op::VOr || i.op == Op::VXor || i.op == Op::VNot)
        return decl(i, "aot::vector_logic<" + enum_name(i) + ">(" + arg(i,0) + "," + arg(i,1) + ")");
      
      // Native vector splat
      if (i.op == Op::VSplat) {
        const auto m = guest_mnemonic(i);
        if (m.find("vspltb") == 0) return decl(i, "aot::vector_splat_byte<" + enum_name(i) + ">(" + arg(i,0) + "," + std::to_string(i.imm0) + ")");
        if (m.find("vsplth") == 0) return decl(i, "aot::vector_splat_halfword<" + enum_name(i) + ">(" + arg(i,0) + "," + std::to_string(i.imm0) + ")");
        if (m.find("vspltw") == 0) return decl(i, "aot::vector_splat_word<" + enum_name(i) + ">(" + arg(i,0) + "," + std::to_string(i.imm0) + ")");
        if (m.find("vspltisb") == 0) return decl(i, "aot::vector_splat_immediate_byte<" + enum_name(i) + ">(" + std::to_string(i.imm0) + ")");
        if (m.find("vspltish") == 0) return decl(i, "aot::vector_splat_immediate_halfword<" + enum_name(i) + ">(" + std::to_string(i.imm0) + ")");
        if (m.find("vspltisw") == 0) return decl(i, "aot::vector_splat_immediate_word<" + enum_name(i) + ">(" + std::to_string(i.imm0) + ")");
      }
      
      // Native modular vector arithmetic
      if (i.op == Op::VAdd || i.op == Op::VSub) {
        const auto m = guest_mnemonic(i);
        if (m == "vaddubm") return decl(i, "aot::vector_add_modular<std::uint8_t>(" + arg(i,0) + "," + arg(i,1) + ")");
        if (m == "vadduhm") return decl(i, "aot::vector_add_modular<std::uint16_t>(" + arg(i,0) + "," + arg(i,1) + ")");
        if (m == "vadduwm") return decl(i, "aot::vector_add_modular<std::uint32_t>(" + arg(i,0) + "," + arg(i,1) + ")");
        if (m == "vsububm") return decl(i, "aot::vector_sub_modular<std::uint8_t>(" + arg(i,0) + "," + arg(i,1) + ")");
        if (m == "vsubuhm") return decl(i, "aot::vector_sub_modular<std::uint16_t>(" + arg(i,0) + "," + arg(i,1) + ")");
        if (m == "vsubuwm") return decl(i, "aot::vector_sub_modular<std::uint32_t>(" + arg(i,0) + "," + arg(i,1) + ")");
      }
      
      if(is_vector_compute(i.op)){
        std::string args="state,"+arg(i,0)+","+arg(i,1)+","+arg(i,2)+","+arg(i,3);
        return decl(i,"aot::execute_vector("+enum_name(i)+","+std::to_string(i.guest_word)+"u,"+args+")");
      }
      throw std::runtime_error("CppAotBackend: unsupported IR op " + std::to_string(static_cast<unsigned>(i.op)) + " from " + std::string(guest_mnemonic(i)));
  }
  return o.str();
}


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

void emit_longjump_dispatch(std::ostringstream& o, std::string_view rr,
                            const std::unordered_set<GuestAddress>& local_targets,
                            std::string_view indent = "  ") {
  o << indent << "if(" << rr << ".reason==FlowReason::LongJump) { switch("
    << rr << ".next_address) { ";
  for (const auto target : local_targets) {
    o << "case " << target << "u: state.cia=" << target
      << "u; state.nia=" << static_cast<GuestAddress>(target + 4u)
      << "u; goto " << local_label(target) << "; ";
  }
  o << "default: return " << rr << "; } }\n";
}

void emit_call_result_check(std::ostringstream& o, std::string_view rr,
                            GuestAddress expected_return,
                            const std::unordered_set<GuestAddress>& local_targets,
                            std::string_view indent = "  ") {
  emit_longjump_dispatch(o, rr, local_targets, indent);
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
        emit_call_result_check(o, "rr", expected_return, local_targets, "    ");
        o << "  }\n";
        return o.str();
      }
      if (const auto* binding = find_direct_call(guest_target, direct_calls)) {
        o << "  { auto rr=" << binding->native_symbol << "(context);\n";
        emit_call_result_check(o, "rr", expected_return, local_targets, "    ");
        o << "  }\n";
        return o.str();
      }
      o << "  { ExecutionResult rr{}; if(auto* native=context.lookup_compiled("
        << guest_target << "u,CompiledLookupKind::Call)) rr=native(context); "
        << "else { auto fallback=context.try_dynamic_fallback(" << guest_target
        << "u,CompiledLookupKind::Call); rr=fallback.handled ? fallback.result : runtime.call("
        << guest_target << "u,state,memory); }\n";
      emit_call_result_check(o, "rr", expected_return, local_targets, "    ");
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
      emit_call_result_check(o, "rr", expected_return, local_targets, "    ");
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
    o << indent << "ExecutionResult rr{}; bool handled_local=false; switch(guest_target) { ";
    for (const auto target : local_targets)
      o << "case " << target << "u: rr=" << local_dispatch_symbol
        << "(context,guest_target); handled_local=true; break; ";
    o << "default: break; }\n";
    o << indent << "if(!handled_local) { if(auto* native=context.lookup_compiled(guest_target,CompiledLookupKind::Call)) rr=native(context); else { auto fallback=context.try_dynamic_fallback(guest_target,CompiledLookupKind::Call); rr=fallback.handled ? fallback.result : runtime.call(raw_target,state,memory); } }\n";
    emit_call_result_check(o, "rr", expected_return, local_targets, indent);
    if (!condition.empty()) o << "  }\n";
    return o.str();
  }

  if (i.op == Op::BranchIndirect && i.imm0 == 0u && i.imm1 != 1u) {
    o << "  if(" << arg(i,0) << ") { const auto guest_target=static_cast<GuestAddress>("
      << arg(i,1) << " & ~3ull); switch(guest_target) { ";
    for (const auto target : local_targets)
      o << "case " << target << "u: state.nia=" << target << "u; goto "
        << local_label(target) << "; ";
    o << "default: break; } if(auto* native=context.lookup_compiled(guest_target,CompiledLookupKind::Branch)) return native(context); auto fallback=context.try_dynamic_fallback(guest_target,CompiledLookupKind::Branch); if(fallback.handled) return fallback.result; state.nia=guest_target; return {FlowReason::Branch,guest_target,0}; }\n";
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
  o << "static ExecutionResult " << dispatch_symbol << "([[maybe_unused]] ExecutionContext& context, GuestAddress entry) {\n";
  o << "  auto& state = context.state;\n";
  o << "  auto& memory = context.memory;\n";
  o << "  auto& runtime = context.runtime;\n";
  o << "  auto& memory_access = context.memory_access;\n";
  o << "  switch(entry) {\n";
  for (const auto target : local_targets)
    o << "    case " << target << "u: goto " << local_label(target) << ";\n";
  o << "    default: return {FlowReason::Trap,entry,0x80000003u};\n  }\n";

  for (const auto& block : function.blocks) {
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

#include "xenon/cpu/backend/cpp_aot.hpp"

#include <iomanip>
#include <optional>
#include <sstream>
#include <unordered_set>
#include <stdexcept>
#include <string>

namespace xenon::cpu::backend {
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
std::string enum_name(const Instruction&i){
  if(i.guest_mnemonic.empty()) throw std::runtime_error("vector IR missing guest mnemonic");
  return "aot::VectorSemantic::"+i.guest_mnemonic;
}

bool is_vector_compute(Op op){
  return op>=Op::VAdd && op<=Op::VMultiplyEvenOdd;
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
    case Op::WriteCRCompare:{o<<"  { std::uint8_t f = "<<arg(i,0)<<" ? 0x8u : ("<<arg(i,1)<<" ? 0x4u : 0x2u); if (("<<arg(i,3)<<" & xer_bits::SO)!=0) f|=1u; state.set_cr_field("<<i.imm0<<", f); }\n";break;}
    case Op::WriteCR0StoreConditional:o<<"  state.set_cr_field(0, static_cast<std::uint8_t>(("<<arg(i,0)<<" ? 0x2u : 0u) | (("<<arg(i,1)<<" & xer_bits::SO) ? 1u : 0u)));\n";break;
    case Op::UpdateCR0Signed:o<<"  state.update_cr0_signed(static_cast<std::uint64_t>("<<arg(i,0)<<"));\n";break;
    case Op::MoveCRFields:{o<<"  { [[maybe_unused]] const std::uint32_t src=static_cast<std::uint32_t>("<<arg(i,0)<<");";for(unsigned f=0;f<8;++f)if(i.imm0&(0x80u>>f))o<<" state.set_cr_field("<<f<<", std::uint8_t((src>>"<<((7-f)*4)<<")&0xFu));";o<<" }\n";break;}
    case Op::MoveXERToCR:o<<"  { std::uint8_t f=(state.xer_so()?8u:0u)|(state.xer_ov()?4u:0u)|(state.xer_ca()?2u:0u); state.set_cr_field("<<i.imm0<<",f); state.xer &= ~(xer_bits::SO|xer_bits::OV|xer_bits::CA); }\n";break;
    case Op::ReadXER:return decl(i,"state.xer");
    case Op::WriteXER:o<<"  state.xer = static_cast<std::uint32_t>("<<arg(i,0)<<");\n";break;
    case Op::SetXerCA:o<<"  state.set_xer_ca("<<arg(i,0)<<");\n";break;
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

    case Op::Add:return bin("+"); case Op::Sub:return bin("-"); case Op::Mul:return bin("*");
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
    case Op::PpcShift:return decl(i,"static_cast<std::uint64_t>(aot::ppc_shift(\""+i.guest_mnemonic+"\","+arg(i,0)+","+arg(i,1)+"))");
    case Op::PpcShiftCarry:return decl(i,"aot::ppc_shift_carry(\""+i.guest_mnemonic+"\","+arg(i,0)+","+arg(i,1)+")");
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
    case Op::VLoadElement:return decl(i,"aot::vector_load_element("+arg(i,0)+",memory,static_cast<GuestAddress>("+arg(i,1)+"),"+std::to_string(i.imm0)+")");
    case Op::VStoreElement:o<<"  aot::vector_store_element("<<arg(i,0)<<",memory,static_cast<GuestAddress>("<<arg(i,1)<<"),"<<i.imm0<<");\n";break;
    case Op::VLoadLeft:return decl(i,"aot::vector_load_left("+arg(i,0)+",memory,static_cast<GuestAddress>("+arg(i,1)+"))");
    case Op::VLoadRight:return decl(i,"aot::vector_load_right("+arg(i,0)+",memory,static_cast<GuestAddress>("+arg(i,1)+"))");
    case Op::VStoreLeft:o<<"  aot::vector_store_left("<<arg(i,0)<<",memory,static_cast<GuestAddress>("<<arg(i,1)<<"));\n";break;
    case Op::VStoreRight:o<<"  aot::vector_store_right("<<arg(i,0)<<",memory,static_cast<GuestAddress>("<<arg(i,1)<<"));\n";break;

    case Op::Load:{std::string ea="static_cast<GuestAddress>("+arg(i,0)+")"; if(i.type==Type::I8)return decl(i,"memory.read8("+ea+")"); if(i.type==Type::V128)return decl(i,"memory.read128("+ea+")"); const bool little=i.imm0==static_cast<std::uint64_t>(ir::Endian::Little);std::string fn="memory.read"+std::to_string(bits(i.type))+(little?"_le":"_be");return decl(i,fn+"("+ea+")");}
    case Op::Store:{std::string ea="static_cast<GuestAddress>("+arg(i,0)+")";Type st=static_cast<Type>(i.imm1);if(st==Type::I8)o<<"  memory.write8("<<ea<<","<<arg(i,1)<<");\n";else if(st==Type::V128)o<<"  memory.write128("<<ea<<","<<arg(i,1)<<");\n";else{bool little=i.imm0==static_cast<std::uint64_t>(ir::Endian::Little);o<<"  memory.write"<<bits(st)<<(little?"_le":"_be")<<"("<<ea<<","<<arg(i,1)<<");\n";}break;}
    case Op::ReserveLoad:{bool w=i.type==Type::I32;o<<"  std::"<<(w?"uint32_t":"uint64_t")<<" tmp_res_"<<i.result<<"{}; state.reservation.token=memory.reserve"<<(w?32:64)<<"(static_cast<GuestAddress>("<<arg(i,0)<<"),tmp_res_"<<i.result<<"); state.reservation.valid=true; state.reservation.width="<<(w?4:8)<<"; state.reservation.address=static_cast<GuestAddress>("<<arg(i,0)<<"); state.reservation.observed_value=tmp_res_"<<i.result<<";\n"<<decl(i,"tmp_res_"+std::to_string(i.result));break;}
    case Op::StoreConditional:{bool w=static_cast<Type>(i.imm1)==Type::I32;std::string ea="static_cast<GuestAddress>("+arg(i,0)+")";std::string e="(state.reservation.valid && state.reservation.width=="+std::to_string(w?4:8)+" && state.reservation.address=="+ea+" && memory.store_conditional"+std::to_string(w?32:64)+"("+ea+",state.reservation.token,static_cast<"+(w?std::string("std::uint32_t"):std::string("std::uint64_t"))+">("+arg(i,1)+")))";o<<decl(i,e)<<"  state.reservation.clear();\n";break;}
    case Op::StringLoad:o<<"  aot::string_load(state,memory,static_cast<GuestAddress>("<<arg(i,0)<<"),static_cast<std::uint32_t>("<<arg(i,1)<<"),"<<i.imm0<<");\n";break;
    case Op::StringStore:o<<"  aot::string_store(state,memory,static_cast<GuestAddress>("<<arg(i,0)<<"),static_cast<std::uint32_t>("<<arg(i,1)<<"),"<<i.imm0<<");\n";break;
    case Op::Barrier:{const char* k=i.imm0==1?"Sync":i.imm0==2?"LightweightSync":i.imm0==3?"Eieio":"InstructionSync";o<<"  memory.barrier(BarrierKind::"<<k<<");\n";break;}
    case Op::CacheZero:o<<"  memory.zero_cache_block(static_cast<GuestAddress>("<<arg(i,0)<<"),"<<i.imm0<<");\n";break;
    case Op::ICacheInvalidate:o<<"  memory.instruction_cache_invalidate(static_cast<GuestAddress>("<<arg(i,0)<<"));\n";break;
    case Op::CacheHint:o<<"  (void)"<<arg(i,0)<<";\n";break;

    case Op::Branch:o<<"  return {FlowReason::Branch,static_cast<GuestAddress>("<<arg(i,0)<<"),0};\n";break;
    case Op::Call:o<<"  { auto rr=runtime.call(static_cast<GuestAddress>("<<arg(i,0)<<"),state,memory); if(rr.terminal()) return rr; }\n";break;
    case Op::BranchIf:{o<<"  if("<<arg(i,0)<<") { ";if(i.imm0)o<<"auto rr=runtime.call(static_cast<GuestAddress>("<<arg(i,1)<<"),state,memory); if(rr.terminal()) return rr;";else o<<"return {FlowReason::Branch,static_cast<GuestAddress>("<<arg(i,1)<<"),0};";o<<" }\n";break;}
    case Op::BranchIndirect:{o<<"  if("<<arg(i,0)<<") { ";if(i.imm0)o<<"auto rr=runtime.call(static_cast<GuestAddress>("<<arg(i,1)<<"),state,memory); if(rr.terminal()) return rr;";else if(i.imm1==1)o<<"return {FlowReason::Return,static_cast<GuestAddress>("<<arg(i,1)<<" & ~3ull),0};";else o<<"return {FlowReason::Branch,static_cast<GuestAddress>("<<arg(i,1)<<" & ~3ull),0};";o<<" }\n";break;}
    case Op::CallIndirect:o<<"  { auto rr=runtime.call(static_cast<GuestAddress>("<<arg(i,0)<<"),state,memory); if(rr.terminal()) return rr; }\n";break;
    case Op::Return:o<<"  return {FlowReason::Return,static_cast<GuestAddress>(state.lr & ~3ull),0};\n";break;
    case Op::Syscall:o<<"  return runtime.syscall("<<i.imm0<<",state,memory);\n";break;
    case Op::Trap:{const std::uint32_t w=i.guest_word;const bool word=i.guest_mnemonic=="tw"||i.guest_mnemonic=="twi";const bool imm=i.guest_mnemonic=="tdi"||i.guest_mnemonic=="twi";unsigned ra=(w>>16)&31u;unsigned rb=(w>>11)&31u;std::int64_t simm=static_cast<std::int16_t>(w&0xFFFFu);o<<"  if(aot::trap_condition("<<i.imm0<<",state.gpr["<<ra<<"],"<<(imm?("std::uint64_t(std::int64_t("+std::to_string(simm)+"))"):("state.gpr["+std::to_string(rb)+"]"))<<","<<(word?"true":"false")<<")) return runtime.trap("<<i.imm0<<",state,memory);\n";break;}
    default:
      if(is_vector_compute(i.op)){
        std::string args="state,"+arg(i,0)+","+arg(i,1)+","+arg(i,2)+","+arg(i,3);
        return decl(i,"aot::execute_vector("+enum_name(i)+","+std::to_string(i.guest_word)+"u,"+args+")");
      }
      throw std::runtime_error("CppAotBackend: unsupported IR op " + std::to_string(static_cast<unsigned>(i.op)) + " from " + i.guest_mnemonic);
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

bool is_unconditional_terminal(const ir::Block& block) {
  for (const auto& insn : block.instructions) {
    if (insn.op == Op::Branch || insn.op == Op::Return || insn.op == Op::Syscall) return true;
  }
  return false;
}

std::string emit_one_in_function(const Instruction& i,
                                 const std::vector<Type>& value_types,
                                 const ir::Block& block,
                                 const std::unordered_set<GuestAddress>& local_targets) {
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
  return emit_one(i, value_types);
}

} // namespace

std::string CppAotBackend::emit_function(const ir::Block& block,std::string_view name) const {
  std::ostringstream o;
  o<<"ExecutionResult "<<name<<"([[maybe_unused]] CpuState& state, [[maybe_unused]] MemoryPort& memory, [[maybe_unused]] RuntimeServices& runtime) {\n";
  o<<"  state.cia="<<block.guest_address<<"u;\n";
  std::vector<Type> value_types;
  for (const auto& i : block.instructions) if (i.result != ir::kNoValue) { if (value_types.size() <= i.result) value_types.resize(i.result + 1, Type::Void); value_types[i.result] = i.type; }
  for(const auto&i:block.instructions)o<<emit_one(i,value_types);
  o<<"  return {FlowReason::Fallthrough,"<<(block.guest_address+4u)<<"u,0};\n}\n";
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


std::string CppAotBackend::emit_function(const ir::Function& function,
                                         std::string_view name) const {
  if (function.blocks.empty()) throw std::runtime_error("CppAotBackend: empty function");

  std::unordered_set<GuestAddress> local_targets;
  for (const auto& block : function.blocks) local_targets.insert(block.guest_address);

  std::ostringstream o;
  o << "ExecutionResult " << name
    << "([[maybe_unused]] CpuState& state, [[maybe_unused]] MemoryPort& memory, "
       "[[maybe_unused]] RuntimeServices& runtime) {\n";
  o << "  goto " << local_label(function.guest_address) << ";\n";

  for (std::size_t bi = 0; bi < function.blocks.size(); ++bi) {
    const auto& block = function.blocks[bi];
    o << local_label(block.guest_address) << ": {\n";
    o << "  state.cia=" << block.guest_address << "u;\n";
    o << "  state.nia=" << static_cast<GuestAddress>(block.guest_address + 4u) << "u;\n";

    std::vector<Type> value_types;
    for (const auto& i : block.instructions) {
      if (i.result != ir::kNoValue) {
        if (value_types.size() <= i.result) value_types.resize(i.result + 1, Type::Void);
        value_types[i.result] = i.type;
      }
    }
    for (const auto& i : block.instructions) {
      o << emit_one_in_function(i, value_types, block, local_targets);
    }

    if (!is_unconditional_terminal(block)) {
      if (bi + 1 < function.blocks.size()) {
        const auto next = function.blocks[bi + 1].guest_address;
        o << "  state.nia=" << next << "u;\n";
        o << "  goto " << local_label(next) << ";\n";
      } else {
        o << "  return {FlowReason::Fallthrough,state.nia,0};\n";
      }
    }
    o << "}\n";
  }
  o << "}\n";
  return o.str();
}

std::string CppAotBackend::emit_translation_unit(const ir::Function& function,
                                                  std::string_view name) const {
  std::ostringstream o;
  o << "#include <bit>\n#include <cmath>\n#include <cfenv>\n#include <cstdint>\n#include <limits>\n";
  o << "#include \"xenon/cpu/aot_semantics.hpp\"\n\n";
  o << "using namespace xenon::cpu;\n";
  o << emit_function(function, name);
  return o.str();
}

} // namespace xenon::cpu::backend

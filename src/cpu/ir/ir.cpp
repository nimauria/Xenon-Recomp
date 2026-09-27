#include "xenon/cpu/ir.hpp"

#include <stdexcept>

namespace xenon::cpu::ir {

Effect effects(Op op) noexcept {
  switch (op) {
    case Op::ReadGpr: case Op::ReadFprBits: case Op::ReadVector:
    case Op::ReadCR: case Op::ReadCRBit: case Op::ReadCRField:
    case Op::ReadXER: case Op::ReadXerCA: case Op::ReadXerOV: case Op::ReadXerSO:
    case Op::ReadFPSCR: case Op::ReadVSCR:
    case Op::ReadLR: case Op::ReadCTR: case Op::ReadMSR:
    case Op::ReadVRSAVE: case Op::ReadPVR:
      return Effect::StateRead;
    case Op::ReadTimeBase: case Op::ReadSPR:
      return Effect::StateRead | Effect::Call;

    case Op::WriteGpr: case Op::WriteFprBits: case Op::WriteVector:
    case Op::WriteCR: case Op::WriteCRBit: case Op::WriteCRField:
    case Op::WriteCRCompare: case Op::WriteCR0StoreConditional:
    case Op::UpdateCR0Signed: case Op::MoveCRFields:
    case Op::WriteXER: case Op::SetXerCA: case Op::SetXerOV: case Op::SetXerSO:
    case Op::WriteFPSCR:
    case Op::WriteFPSCRFieldImmediate: case Op::WriteFPSCRFields:
    case Op::WriteVSCR: case Op::SetVSCRSaturation:
    case Op::MoveVectorToVSCR: case Op::WriteLR: case Op::WriteCTR:
    case Op::WriteMSR: case Op::WriteVRSAVE:
      return Effect::StateWrite;
    case Op::MoveVSCRToVector:
      return Effect::StateRead;
    case Op::MoveXERToCR: case Op::SetXerOverflowSticky:
    case Op::UpdateCR1FromFPSCR: case Op::MoveFPSCRFieldToCR:
    case Op::SetFPSCRBit:
      return Effect::StateRead | Effect::StateWrite;
    case Op::WriteSPR:
      return Effect::StateWrite | Effect::Call;

    // These helpers participate in the exact guest FP status model. Keep them
    // conservatively ordered until the dedicated FP V2 phases split status
    // production from pure arithmetic where that is provably safe.
    case Op::FAdd: case Op::FSub: case Op::FMul: case Op::FDiv:
    case Op::FFma: case Op::FSqrt: case Op::FCompare:
    case Op::FRoundSingle: case Op::FConvertFromI64: case Op::FConvertToI64:
    case Op::FReciprocalEstimate: case Op::FReciprocalSqrtEstimate:
    case Op::FUpdateStatus:
      return Effect::StateWrite;

    // The current family vector helper can update VSCR saturation. Phase 16
    // will split pure and saturating forms; until then preserve ordering.
    case Op::VAdd: case Op::VSub: case Op::VMul: case Op::VMadd:
    case Op::VMin: case Op::VMax: case Op::VAvg: case Op::VAnd:
    case Op::VAndNot: case Op::VOr: case Op::VXor: case Op::VNot:
    case Op::VCompare: case Op::VShuffle: case Op::VPermute:
    case Op::VPack: case Op::VUnpack: case Op::VConvert: case Op::VShift:
    case Op::VShiftDouble: case Op::VRotate: case Op::VRotateInsert:
    case Op::VSplat: case Op::VMerge: case Op::VSum: case Op::VEstimate:
    case Op::VDot: case Op::VSelect: case Op::VMultiplyEvenOdd:
    case Op::VUpdateCR6:
      return Effect::StateWrite;

    case Op::Load: case Op::VLoadElement: case Op::VLoadLeft:
    case Op::VLoadRight:
      return Effect::MemoryRead | Effect::MayFault;
    case Op::Store: case Op::VStoreElement: case Op::VStoreLeft:
    case Op::VStoreRight: case Op::CacheZero:
      return Effect::MemoryWrite | Effect::MayFault;
    case Op::ReserveLoad:
      return Effect::MemoryRead | Effect::StateWrite | Effect::Synchronization |
             Effect::MayFault;
    case Op::StoreConditional:
      return Effect::MemoryRead | Effect::MemoryWrite | Effect::StateWrite |
             Effect::Synchronization | Effect::MayFault;
    case Op::StringLoad:
      return Effect::MemoryRead | Effect::StateWrite | Effect::MayFault;
    case Op::StringStore:
      return Effect::MemoryWrite | Effect::StateRead | Effect::MayFault;
    case Op::ICacheInvalidate:
      return Effect::MemoryWrite | Effect::Barrier | Effect::Synchronization |
             Effect::MayFault;
    case Op::CacheHint:
      return Effect::MemoryRead;
    case Op::Barrier:
      return Effect::Barrier | Effect::Synchronization;

    case Op::Branch: case Op::BranchIf: case Op::BranchIndirect:
    case Op::Return:
      return Effect::ControlFlow;
    case Op::Call: case Op::CallIndirect:
      return Effect::ControlFlow | Effect::Call | Effect::StateRead |
             Effect::StateWrite;
    case Op::Trap:
      return Effect::ControlFlow | Effect::Trap | Effect::Call |
             Effect::StateRead | Effect::StateWrite;
    case Op::Syscall:
      return Effect::ControlFlow | Effect::Call | Effect::StateRead |
             Effect::StateWrite;
    default:
      return Effect::None;
  }
}

bool is_block_terminator(const Instruction& instruction) noexcept {
  switch (instruction.op) {
    case Op::Branch:
    case Op::Return:
    case Op::Syscall:
    case Op::Trap:
      return true;
    case Op::BranchIf:
    case Op::BranchIndirect:
      return instruction.imm0 == 0u; // LK forms are calls and return locally.
    case Op::Barrier:
      return instruction.imm0 == 4u; // isync exits for executable revalidation.
    default:
      return false;
  }
}

void OperandList::assign(std::span<const ValueId> values) {
  if (values.size() > kCapacity)
    throw std::length_error("Xenon IR operand capacity exceeded");
  size_ = static_cast<std::uint8_t>(values.size());
  for (std::size_t n = 0; n < values.size(); ++n) values_[n] = values[n];
}

void OperandList::assign(std::initializer_list<ValueId> values) {
  assign(std::span<const ValueId>(values.begin(), values.size()));
}

ValueId Builder::emit(Op op, Type type, std::span<const ValueId> args,
                      std::uint64_t imm0, std::uint64_t imm1,
                      const DecodedInstruction* guest) {
  Instruction i{};
  i.op = op;
  i.type = type;
  if (type != Type::Void) i.result = next_value_++;
  i.args.assign(args);
  i.imm0 = imm0;
  i.imm1 = imm1;
  const auto* source = guest ? guest : current_guest_;
  if (source) {
    i.guest_address = source->address;
    i.guest_word = source->word;
    i.guest_opcode = source->opcode_id();
  }
  block_.instructions.push_back(std::move(i));
  return block_.instructions.back().result;
}

ValueId Builder::constant_i64(std::uint64_t value) {
  return emit(Op::Constant, Type::I64, {}, value);
}
ValueId Builder::constant_i32(std::uint32_t value) {
  return emit(Op::Constant, Type::I32, {}, value);
}
ValueId Builder::constant_i1(bool value) {
  return emit(Op::Constant, Type::I1, {}, value ? 1u : 0u);
}

ValueId Builder::read_gpr(unsigned reg) {
  return emit(Op::ReadGpr, Type::I64, {}, reg);
}

void Builder::write_gpr(unsigned reg, ValueId value) {
  const ValueId a[] = {value};
  emit(Op::WriteGpr, Type::Void, a, reg);
}
ValueId Builder::read_fpr_bits(unsigned reg) {
  return emit(Op::ReadFprBits, Type::I64, {}, reg);
}
void Builder::write_fpr_bits(unsigned reg, ValueId value) {
  const ValueId a[] = {value};
  emit(Op::WriteFprBits, Type::Void, a, reg);
}
ValueId Builder::read_vector(unsigned reg) {
  return emit(Op::ReadVector, Type::V128, {}, reg);
}
void Builder::write_vector(unsigned reg, ValueId value) {
  const ValueId a[] = {value};
  emit(Op::WriteVector, Type::Void, a, reg);
}

ValueId Builder::read_lr() { return emit(Op::ReadLR, Type::I64); }
void Builder::write_lr(ValueId value) {
  const ValueId a[] = {value};
  emit(Op::WriteLR, Type::Void, a);
}
ValueId Builder::read_ctr() { return emit(Op::ReadCTR, Type::I64); }
void Builder::write_ctr(ValueId value) {
  const ValueId a[] = {value};
  emit(Op::WriteCTR, Type::Void, a);
}


}  // namespace xenon::cpu::ir

#include "xenon/cpu/ir.hpp"

namespace xenon::cpu::ir {

ValueId Builder::emit(Op op, Type type, std::span<const ValueId> args,
                      std::uint64_t imm0, std::uint64_t imm1,
                      const DecodedInstruction* guest) {
  Instruction i{};
  i.op = op;
  i.type = type;
  if (type != Type::Void) i.result = next_value_++;
  i.args.assign(args.begin(), args.end());
  i.imm0 = imm0;
  i.imm1 = imm1;
  if (guest) {
    i.guest_address = guest->address;
    i.guest_word = guest->word;
    i.guest_mnemonic = std::string(guest->mnemonic());
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

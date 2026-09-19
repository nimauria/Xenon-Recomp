#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

#include "xenon/cpu/instruction.hpp"

namespace xenon::cpu::ir {

using ValueId = std::uint32_t;
inline constexpr ValueId kNoValue = 0xFFFFFFFFu;

enum class Type : std::uint8_t { Void, I1, I8, I16, I32, I64, F32, F64, V128 };

enum class Endian : std::uint8_t { Big, Little, Raw };
enum class MemoryOrdering : std::uint8_t { None, Sync, LightweightSync, Eieio, InstructionSync };

enum class Effect : std::uint16_t {
  None = 0,
  StateRead = 1u << 0,
  StateWrite = 1u << 1,
  MemoryRead = 1u << 2,
  MemoryWrite = 1u << 3,
  Barrier = 1u << 4,
  Call = 1u << 5,
  Trap = 1u << 6,
  MayFault = 1u << 7,
  ControlFlow = 1u << 8,
  Synchronization = 1u << 9,
};

[[nodiscard]] constexpr Effect operator|(Effect a, Effect b) noexcept {
  return static_cast<Effect>(static_cast<std::uint16_t>(a) |
                             static_cast<std::uint16_t>(b));
}
[[nodiscard]] constexpr bool has_effect(Effect set, Effect flag) noexcept {
  return (static_cast<std::uint16_t>(set) & static_cast<std::uint16_t>(flag)) != 0;
}

enum class Op : std::uint16_t {
  Constant,
  ReadGpr, WriteGpr,
  ReadFprBits, WriteFprBits,
  ReadVector, WriteVector,
  ReadCR, WriteCR, ReadCRBit, WriteCRBit, ReadCRField, WriteCRField, WriteCRCompare, WriteCR0StoreConditional, UpdateCR0Signed,
  MoveCRFields, MoveXERToCR,
  ReadXER, WriteXER,
  ReadXerCA, ReadXerOV, ReadXerSO,
  SetXerCA, SetXerOV, SetXerSO, SetXerOverflowSticky,
  ReadFPSCR, WriteFPSCR, UpdateCR1FromFPSCR,
  MoveFPSCRFieldToCR, MoveFPSCRToFPRBits, SetFPSCRBit, WriteFPSCRFieldImmediate, WriteFPSCRFields,
  ReadVSCR, WriteVSCR, SetVSCRSaturation, MoveVSCRToVector, MoveVectorToVSCR,
  ReadLR, WriteLR,
  ReadCTR, WriteCTR,
  ReadMSR, WriteMSR,
  ReadTimeBase,
  ReadVRSAVE, WriteVRSAVE, ReadPVR, ReadSPR, WriteSPR,

  Add, AddImmediate, AddCarry, Sub, Mul, MulHighSigned, MulHighUnsigned,
  DivSigned, DivUnsigned, Neg, CountLeadingZeros,
  CarryOut, SignedOverflow, MulOverflowSigned, DivOverflow, PpcShift, PpcShiftCarry,
  And, Or, Xor, Not,
  Shl, ShrLogical, ShrArithmetic, Rotl,
  SignExtend, ZeroExtend, Truncate, Bitcast, FPromote, FTruncate,
  CompareEq, CompareNe, CompareSlt, CompareSgt, CompareUlt, CompareUgt,
  Select,

  FAdd, FSub, FMul, FDiv, FFma, FSqrt, FAbs, FNeg,
  FCompare, FSelect, FRoundSingle, FConvertFromI64, FConvertToI64,
  FReciprocalEstimate, FReciprocalSqrtEstimate, FUpdateStatus,

  VAdd, VSub, VMul, VMadd, VMin, VMax, VAvg, VAnd, VAndNot, VOr, VXor, VNot,
  VCompare, VUpdateCR6, VShuffle, VPermute, VPack, VUnpack, VConvert,
  VShift, VShiftDouble, VRotate, VRotateInsert, VSplat, VMerge, VSum, VEstimate, VDot, VSelect, VMultiplyEvenOdd,
  VLoadElement, VStoreElement, VLoadLeft, VLoadRight, VStoreLeft, VStoreRight,
  VMakeLoadShiftLeft, VMakeLoadShiftRight,

  Load, Store, ReserveLoad, StoreConditional, StringLoad, StringStore,
  Barrier, CacheZero, ICacheInvalidate, CacheHint,

  Branch, BranchIf, BranchIndirect, Call, CallIndirect, Return,
  Trap, Syscall,


};

// All current Xenon IR operations use at most four value operands. Keeping
// operands inline removes the per-node heap allocation from std::vector while
// retaining range-for/size/index access used by the verifier and optimizer.
class OperandList {
 public:
  static constexpr std::size_t kCapacity = 4;

  void assign(std::span<const ValueId> values);
  void assign(std::initializer_list<ValueId> values);
  OperandList& operator=(std::initializer_list<ValueId> values) {
    assign(values);
    return *this;
  }
  void clear() noexcept { size_ = 0; }

  [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] constexpr ValueId operator[](std::size_t index) const noexcept { return values_[index]; }
  constexpr void set(std::size_t index, ValueId value) noexcept { values_[index] = value; }
  [[nodiscard]] constexpr const ValueId* begin() const noexcept { return values_.data(); }
  [[nodiscard]] constexpr const ValueId* end() const noexcept { return values_.data() + size_; }

 private:
  std::array<ValueId, kCapacity> values_{};
  std::uint8_t size_{};
};

enum class EdgeKind : std::uint8_t { Fallthrough, Branch, Call };

struct ControlFlowEdge {
  GuestAddress target{};
  EdgeKind kind{EdgeKind::Fallthrough};
  bool local{};
};

struct Instruction {
  Op op{};
  Type type{Type::Void};
  ValueId result{kNoValue};
  OperandList args{};
  std::uint64_t imm0{};
  std::uint64_t imm1{};
  GuestAddress guest_address{};
  std::uint32_t guest_word{};
  OpcodeId guest_opcode{};
};

[[nodiscard]] Effect effects(Op op) noexcept;
[[nodiscard]] bool is_block_terminator(const Instruction& instruction) noexcept;

struct Block {
  GuestAddress guest_address{};
  std::vector<Instruction> instructions{};
  // Exclusive guest-address end. Zero is permitted for hand-built single-op IR.
  GuestAddress end_address{};
  std::vector<ControlFlowEdge> successors{};
  std::vector<GuestAddress> predecessors{};
  bool has_external_exit{};
  bool has_indirect_exit{};
  bool has_indirect_call{};
};

struct Function {
  GuestAddress guest_address{};
  std::vector<Block> blocks{};
};

class Builder {
 public:
  explicit Builder(Block& block) : block_(block) {}

  ValueId emit(Op op, Type type = Type::Void,
               std::span<const ValueId> args = {},
               std::uint64_t imm0 = 0, std::uint64_t imm1 = 0,
               const DecodedInstruction* guest = nullptr);

  // Real guest blocks contain many instructions. Tag every emitted IR node with
  // its owning guest instruction so codegen can preserve precise CIA/NIA until
  // the dedicated Phase 14 PC optimization replaces hot-path stores.
  void set_guest(const DecodedInstruction* guest) noexcept { current_guest_ = guest; }

  ValueId constant_i64(std::uint64_t value);
  ValueId constant_i32(std::uint32_t value);
  ValueId constant_i1(bool value);
  ValueId read_gpr(unsigned reg);
  void write_gpr(unsigned reg, ValueId value);
  ValueId read_fpr_bits(unsigned reg);
  void write_fpr_bits(unsigned reg, ValueId value);
  ValueId read_vector(unsigned reg);
  void write_vector(unsigned reg, ValueId value);
  ValueId read_lr();
  void write_lr(ValueId value);
  ValueId read_ctr();
  void write_ctr(ValueId value);

 private:
  Block& block_;
  ValueId next_value_{};
  const DecodedInstruction* current_guest_{};
};


}  // namespace xenon::cpu::ir

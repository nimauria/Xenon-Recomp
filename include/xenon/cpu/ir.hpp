#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xenon/cpu/instruction.hpp"

namespace xenon::cpu::ir {

using ValueId = std::uint32_t;
inline constexpr ValueId kNoValue = 0xFFFFFFFFu;

enum class Type : std::uint8_t { Void, I1, I8, I16, I32, I64, F32, F64, V128 };

enum class Endian : std::uint8_t { Big, Little, Raw };
enum class MemoryOrdering : std::uint8_t { None, Sync, LightweightSync, Eieio, InstructionSync };

enum class Op : std::uint16_t {
  Constant,
  ReadGpr, WriteGpr,
  ReadFprBits, WriteFprBits,
  ReadVector, WriteVector,
  ReadCR, WriteCR, ReadCRBit, WriteCRBit, ReadCRField, WriteCRField, WriteCRCompare, WriteCR0StoreConditional, UpdateCR0Signed,
  MoveCRFields, MoveXERToCR,
  ReadXER, WriteXER, SetXerCA, SetXerOverflowSticky,
  ReadFPSCR, WriteFPSCR, UpdateCR1FromFPSCR,
  MoveFPSCRFieldToCR, MoveFPSCRToFPRBits, SetFPSCRBit, WriteFPSCRFieldImmediate, WriteFPSCRFields,
  ReadVSCR, WriteVSCR, SetVSCRSaturation, MoveVSCRToVector, MoveVectorToVSCR,
  ReadLR, WriteLR,
  ReadCTR, WriteCTR,
  ReadMSR, WriteMSR,
  ReadTimeBase,
  ReadVRSAVE, WriteVRSAVE, ReadPVR, ReadSPR, WriteSPR,

  Add, AddCarry, Sub, Mul, MulHighSigned, MulHighUnsigned,
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

struct Instruction {
  Op op{};
  Type type{Type::Void};
  ValueId result{kNoValue};
  std::vector<ValueId> args{};
  std::uint64_t imm0{};
  std::uint64_t imm1{};
  GuestAddress guest_address{};
  std::uint32_t guest_word{};
  std::string guest_mnemonic{};
};

struct Block {
  GuestAddress guest_address{};
  std::vector<Instruction> instructions{};
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
};


}  // namespace xenon::cpu::ir

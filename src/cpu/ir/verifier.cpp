#include "xenon/cpu/ir_verifier.hpp"
#include "xenon/cpu/decoder.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace xenon::cpu::ir {
namespace {

VerifyResult fail(VerifyError error, GuestAddress block, std::size_t index,
                  std::string message) {
  return {false, error, block, index, std::move(message)};
}

struct Arity { std::uint8_t min{}; std::uint8_t max{}; };

Arity arity(Op op) noexcept {
  switch (op) {
    case Op::Constant:
    case Op::ReadGpr: case Op::ReadFprBits: case Op::ReadVector:
    case Op::ReadCR: case Op::ReadCRBit: case Op::ReadCRField:
    case Op::MoveXERToCR: case Op::ReadXER: case Op::ReadXerCA:
    case Op::ReadXerOV: case Op::ReadXerSO: case Op::ReadFPSCR:
    case Op::UpdateCR1FromFPSCR: case Op::MoveFPSCRFieldToCR:
    case Op::SetFPSCRBit: case Op::WriteFPSCRFieldImmediate:
    case Op::ReadVSCR: case Op::MoveVSCRToVector: case Op::ReadLR:
    case Op::ReadCTR: case Op::ReadMSR: case Op::ReadTimeBase:
    case Op::ReadVRSAVE: case Op::ReadPVR: case Op::ReadSPR:
    case Op::Trap: case Op::Syscall: case Op::Return:
    case Op::Barrier:
      return {0, 0};

    case Op::WriteGpr: case Op::WriteFprBits: case Op::WriteVector:
    case Op::WriteCR: case Op::WriteCRBit: case Op::WriteCRField:
    case Op::UpdateCR0Signed: case Op::MoveCRFields: case Op::WriteXER:
    case Op::SetXerCA: case Op::SetXerOV: case Op::SetXerSO:
    case Op::SetXerOverflowSticky: case Op::WriteFPSCR:
    case Op::MoveFPSCRToFPRBits: case Op::WriteFPSCRFields:
    case Op::WriteVSCR: case Op::SetVSCRSaturation:
    case Op::MoveVectorToVSCR: case Op::WriteLR: case Op::WriteCTR:
    case Op::WriteMSR: case Op::WriteVRSAVE: case Op::WriteSPR:
    case Op::AddImmediate: case Op::Neg: case Op::CountLeadingZeros: case Op::Not:
    case Op::SignExtend: case Op::ZeroExtend: case Op::Truncate:
    case Op::Bitcast: case Op::FPromote: case Op::FTruncate:
    case Op::FSqrt: case Op::FAbs: case Op::FNeg: case Op::FRoundSingle:
    case Op::FConvertFromI64: case Op::FConvertToI64:
    case Op::FReciprocalEstimate: case Op::FReciprocalSqrtEstimate:
    case Op::VUpdateCR6: case Op::Load: case Op::ReserveLoad:
    case Op::CacheZero: case Op::ICacheInvalidate: case Op::CacheHint:
    case Op::Branch: case Op::Call: case Op::CallIndirect:
      return {1, 1};

    case Op::FUpdateStatus:
      return {0, 2};

    case Op::Add: case Op::Sub: case Op::Mul:
    case Op::MulHighSigned: case Op::MulHighUnsigned:
    case Op::DivSigned: case Op::DivUnsigned: case Op::MulOverflowSigned:
    case Op::DivOverflow: case Op::PpcShift: case Op::PpcShiftCarry:
    case Op::And: case Op::Or: case Op::Xor: case Op::Shl:
    case Op::ShrLogical: case Op::ShrArithmetic: case Op::Rotl:
    case Op::CompareEq: case Op::CompareNe: case Op::CompareSlt:
    case Op::CompareSgt: case Op::CompareUlt: case Op::CompareUgt:
    case Op::FAdd: case Op::FSub: case Op::FMul: case Op::FDiv:
    case Op::FCompare: case Op::Store: case Op::StoreConditional:
    case Op::StringLoad: case Op::StringStore: case Op::BranchIf:
    case Op::BranchIndirect:
      return {2, 2};

    case Op::AddCarry: case Op::CarryOut: case Op::SignedOverflow:
    case Op::Select: case Op::FFma: case Op::FSelect:
      return {3, 3};

    case Op::WriteCR0StoreConditional:
      return {2, 2};
    case Op::WriteCRCompare:
      return {4, 4};

    // VMX family nodes deliberately retain family-level arity until Phase 16.
    case Op::VAdd: case Op::VSub: case Op::VMul: case Op::VMadd:
    case Op::VMin: case Op::VMax: case Op::VAvg: case Op::VAnd:
    case Op::VAndNot: case Op::VOr: case Op::VXor: case Op::VNot:
    case Op::VCompare: case Op::VShuffle: case Op::VPermute:
    case Op::VPack: case Op::VUnpack: case Op::VConvert: case Op::VShift:
    case Op::VShiftDouble: case Op::VRotate: case Op::VRotateInsert:
    case Op::VSplat: case Op::VMerge: case Op::VSum: case Op::VEstimate:
    case Op::VDot: case Op::VSelect: case Op::VMultiplyEvenOdd:
      return {1, 3};

    case Op::VLoadElement: case Op::VStoreElement:
    case Op::VLoadLeft: case Op::VLoadRight:
    case Op::VStoreLeft: case Op::VStoreRight:
    case Op::VMakeLoadShiftLeft: case Op::VMakeLoadShiftRight:
      return {2, 2};
  }
  return {0, 4};
}

bool expects_void(Op op) noexcept {
  switch (op) {
    case Op::WriteGpr: case Op::WriteFprBits: case Op::WriteVector:
    case Op::WriteCR: case Op::WriteCRBit: case Op::WriteCRField:
    case Op::WriteCRCompare: case Op::WriteCR0StoreConditional:
    case Op::UpdateCR0Signed: case Op::MoveCRFields: case Op::MoveXERToCR:
    case Op::WriteXER: case Op::SetXerCA: case Op::SetXerOverflowSticky:
    case Op::WriteFPSCR: case Op::UpdateCR1FromFPSCR:
    case Op::MoveFPSCRFieldToCR: case Op::SetFPSCRBit:
    case Op::WriteFPSCRFieldImmediate: case Op::WriteFPSCRFields:
    case Op::WriteVSCR: case Op::SetVSCRSaturation:
    case Op::MoveVectorToVSCR: case Op::WriteLR: case Op::WriteCTR:
    case Op::WriteMSR: case Op::WriteVRSAVE: case Op::WriteSPR:
    case Op::FUpdateStatus: case Op::VUpdateCR6:
    case Op::VStoreElement: case Op::VStoreLeft: case Op::VStoreRight:
    case Op::Store: case Op::StringLoad: case Op::StringStore:
    case Op::Barrier: case Op::CacheZero: case Op::ICacheInvalidate:
    case Op::CacheHint: case Op::Branch: case Op::BranchIf:
    case Op::BranchIndirect: case Op::Call: case Op::CallIndirect:
    case Op::Return: case Op::Trap: case Op::Syscall:
      return true;
    default:
      return false;
  }
}

bool is_integer(Type t) noexcept {
  return t == Type::I1 || t == Type::I8 || t == Type::I16 ||
         t == Type::I32 || t == Type::I64;
}
bool is_address(Type t) noexcept { return t == Type::I32 || t == Type::I64; }
bool is_scalar_memory_type(Type t) noexcept {
  return t == Type::I8 || t == Type::I16 || t == Type::I32 || t == Type::I64;
}
bool valid_endian(std::uint64_t v) noexcept {
  return v <= static_cast<std::uint64_t>(Endian::Raw);
}

bool fixed_result_type_ok(const Instruction& i) noexcept {
  switch (i.op) {
    case Op::ReadGpr: case Op::ReadFprBits: case Op::ReadLR:
    case Op::ReadCTR: case Op::ReadMSR: case Op::ReadTimeBase:
    case Op::ReadSPR: case Op::MoveFPSCRToFPRBits:
      return i.type == Type::I64;
    case Op::ReadCR: case Op::ReadXER: case Op::ReadFPSCR:
    case Op::ReadVSCR: case Op::ReadVRSAVE: case Op::ReadPVR:
      return i.type == Type::I32;
    case Op::ReadCRBit: case Op::ReadXerCA: case Op::ReadXerOV: case Op::ReadXerSO:
    case Op::CarryOut: case Op::SignedOverflow:
    case Op::MulOverflowSigned: case Op::DivOverflow:
    case Op::PpcShiftCarry: case Op::CompareEq: case Op::CompareNe:
    case Op::CompareSlt: case Op::CompareSgt: case Op::CompareUlt:
    case Op::CompareUgt: case Op::StoreConditional:
      return i.type == Type::I1;
    case Op::ReadCRField: case Op::FCompare:
      return i.type == Type::I8;
    case Op::ReadVector: case Op::MoveVSCRToVector:
    case Op::VAdd: case Op::VSub: case Op::VMul: case Op::VMadd:
    case Op::VMin: case Op::VMax: case Op::VAvg: case Op::VAnd:
    case Op::VAndNot: case Op::VOr: case Op::VXor: case Op::VNot:
    case Op::VCompare: case Op::VShuffle: case Op::VPermute:
    case Op::VPack: case Op::VUnpack: case Op::VConvert: case Op::VShift:
    case Op::VShiftDouble: case Op::VRotate: case Op::VRotateInsert:
    case Op::VSplat: case Op::VMerge: case Op::VSum: case Op::VEstimate:
    case Op::VDot: case Op::VSelect: case Op::VMultiplyEvenOdd:
    case Op::VLoadElement: case Op::VLoadLeft: case Op::VLoadRight:
    case Op::VMakeLoadShiftLeft: case Op::VMakeLoadShiftRight:
      return i.type == Type::V128;
    default:
      return true;
  }
}

VerifyResult validate_memory(const Instruction& i, GuestAddress block,
                             std::size_t index,
                             const std::vector<Type>& values) {
  auto type_of = [&](std::size_t n) { return values[i.args[n]]; };
  auto bad = [&](std::string message) {
    return fail(VerifyError::InvalidMemoryMetadata, block, index, std::move(message));
  };
  switch (i.op) {
    case Op::Load:
      if (!valid_endian(i.imm0)) return bad("load has invalid endian metadata");
      if (!(is_scalar_memory_type(i.type) || i.type == Type::V128))
        return bad("load has unsupported result width");
      if (i.type == Type::V128 &&
          (i.imm0 != static_cast<std::uint64_t>(Endian::Raw) || i.imm1 != 16u))
        return bad("vector load must be raw 16-byte memory");
      break;
    case Op::Store: {
      if (!valid_endian(i.imm0)) return bad("store has invalid endian metadata");
      const auto stored = static_cast<Type>(i.imm1);
      if (!(is_scalar_memory_type(stored) || stored == Type::V128))
        return bad("store has unsupported value width");
      if (type_of(1) != stored) return bad("store value type disagrees with width metadata");
      if (stored == Type::V128 && i.imm0 != static_cast<std::uint64_t>(Endian::Raw))
        return bad("vector store must use raw byte order");
      break;
    }
    case Op::ReserveLoad:
      if (!valid_endian(i.imm0) || (i.type != Type::I32 && i.type != Type::I64))
        return bad("reserve load metadata is invalid");
      break;
    case Op::StoreConditional: {
      if (!valid_endian(i.imm0)) return bad("conditional store has invalid endian metadata");
      const auto stored = static_cast<Type>(i.imm1);
      if (stored != Type::I32 && stored != Type::I64)
        return bad("conditional store width must be I32 or I64");
      if (type_of(1) != stored) return bad("conditional store value type disagrees with width");
      break;
    }
    case Op::VLoadElement: case Op::VStoreElement:
      if (i.imm0 != 1u && i.imm0 != 2u && i.imm0 != 4u)
        return bad("vector element memory width must be 1, 2, or 4");
      break;
    case Op::VLoadLeft: case Op::VLoadRight:
    case Op::VStoreLeft: case Op::VStoreRight:
      if (i.imm0 > 1u) return bad("vector partial-load/store mode is invalid");
      break;
    case Op::CacheZero:
      if (i.imm0 != 32u && i.imm0 != 128u)
        return bad("cache-zero line size must be 32 or 128 bytes");
      break;
    case Op::StringLoad: case Op::StringStore:
      if (i.imm0 >= 32u || i.imm1 > 1u)
        return bad("string load/store register or indexed metadata is invalid");
      break;
    default:
      break;
  }
  return {true, VerifyError::None, block, index, {}};
}

VerifyResult verify_block_impl(const Block& block, bool require_guest_source) {
  if (block.end_address &&
      (block.end_address <= block.guest_address ||
       ((block.end_address - block.guest_address) & 3u) != 0u)) {
    return fail(VerifyError::InvalidBlockRange, block.guest_address, 0,
                "invalid guest block address range");
  }

  std::vector<Type> values;
  std::vector<bool> defined;
  bool saw_terminator = false;
  for (std::size_t index = 0; index < block.instructions.size(); ++index) {
    const auto& i = block.instructions[index];
    if (saw_terminator) {
      return fail(VerifyError::InvalidSideEffectOrdering, block.guest_address, index,
                  "IR operation appears after a terminal control-flow effect");
    }
    if (require_guest_source) {
      if (!i.guest_opcode.valid() || Decoder::opcode_info(i.guest_opcode) == nullptr ||
          (i.guest_address & 3u) != 0u || i.guest_address < block.guest_address ||
          (block.end_address && i.guest_address >= block.end_address)) {
        return fail(VerifyError::InvalidGuestSource, block.guest_address, index,
                    "IR node is missing valid guest-source metadata");
      }
    }

    const auto expected = arity(i.op);
    if (i.args.size() < expected.min || i.args.size() > expected.max) {
      return fail(VerifyError::InvalidArgumentCount, block.guest_address, index,
                  "IR operation has an invalid operand count");
    }
    for (const auto arg : i.args) {
      if (arg >= defined.size() || !defined[arg]) {
        return fail(VerifyError::UseBeforeDefinition, block.guest_address, index,
                    "IR operand is not defined earlier in this block");
      }
    }

    const bool must_void = expects_void(i.op);
    if (must_void) {
      if (i.type != Type::Void || i.result != kNoValue) {
        return fail(VerifyError::InvalidResultType, block.guest_address, index,
                    "side-effect/control IR operation unexpectedly produces a value");
      }
    } else {
      if (i.type == Type::Void || i.result == kNoValue) {
        return fail(VerifyError::InvalidResult, block.guest_address, index,
                    "value-producing IR operation has no result");
      }
      if (!fixed_result_type_ok(i)) {
        return fail(VerifyError::InvalidResultType, block.guest_address, index,
                    "IR operation has an invalid fixed result type");
      }
      if (i.result >= defined.size()) {
        defined.resize(i.result + 1u, false);
        values.resize(i.result + 1u, Type::Void);
      }
      if (defined[i.result]) {
        return fail(VerifyError::DuplicateResult, block.guest_address, index,
                    "IR result id is defined more than once");
      }
      defined[i.result] = true;
      values[i.result] = i.type;
    }

    auto type_of = [&](std::size_t n) { return values[i.args[n]]; };
    auto operand_fail = [&](std::string message) {
      return fail(VerifyError::InvalidOperandType, block.guest_address, index,
                  std::move(message));
    };
    switch (i.op) {
      case Op::WriteGpr: case Op::WriteFprBits: case Op::WriteLR:
      case Op::WriteCTR: case Op::WriteMSR: case Op::WriteSPR:
        if (type_of(0) != Type::I64) return operand_fail("64-bit state write requires I64");
        break;
      case Op::AddImmediate:
        if (i.type != Type::I64 || type_of(0) != Type::I64)
          return operand_fail("AddImmediate requires I64 input/result");
        break;
      case Op::WriteVector: case Op::MoveVectorToVSCR:
        if (type_of(0) != Type::V128) return operand_fail("vector state write requires V128");
        break;
      case Op::Branch: case Op::Call: case Op::CallIndirect:
        if (type_of(0) != Type::I64) return operand_fail("guest control-flow target requires I64");
        break;
      case Op::BranchIf: case Op::BranchIndirect:
        if (type_of(0) != Type::I1 || type_of(1) != Type::I64)
          return operand_fail("conditional guest control flow requires I1 + I64");
        break;
      case Op::Load: case Op::ReserveLoad: case Op::CacheZero:
      case Op::ICacheInvalidate: case Op::CacheHint:
        if (!is_address(type_of(0))) return operand_fail("guest memory address requires I32/I64");
        break;
      case Op::Store: case Op::StoreConditional:
        if (!is_address(type_of(0))) return operand_fail("guest memory address requires I32/I64");
        break;
      case Op::VLoadElement: case Op::VLoadLeft: case Op::VLoadRight:
        if (type_of(0) != Type::V128 || !is_address(type_of(1)))
          return operand_fail("vector load requires V128 + address");
        break;
      case Op::VStoreElement: case Op::VStoreLeft: case Op::VStoreRight:
        if (type_of(0) != Type::V128 || !is_address(type_of(1)))
          return operand_fail("vector store requires V128 + address");
        break;
      case Op::StringLoad: case Op::StringStore:
        if (!is_address(type_of(0)) || type_of(1) != Type::I32)
          return operand_fail("string memory operation requires address + I32 count");
        break;
      case Op::CompareEq: case Op::CompareNe: case Op::CompareSlt:
      case Op::CompareSgt: case Op::CompareUlt: case Op::CompareUgt:
        if (type_of(0) != type_of(1) || !is_integer(type_of(0)))
          return operand_fail("integer comparison requires equal integer operand types");
        break;
      case Op::Select:
        if (type_of(0) != Type::I1 || type_of(1) != type_of(2) || type_of(1) != i.type)
          return operand_fail("select requires I1 condition and equal value/result types");
        break;
      default:
        break;
    }

    if (auto memory = validate_memory(i, block.guest_address, index, values); !memory.ok)
      return memory;
    saw_terminator = is_block_terminator(i);
  }
  return {true, VerifyError::None, block.guest_address, 0, {}};
}

}  // namespace

VerifyResult Verifier::verify(const Block& block) const {
  return verify_block_impl(block, false);
}

VerifyResult Verifier::verify(const Function& function) const {
  if (function.blocks.empty())
    return fail(VerifyError::EmptyFunction, function.guest_address, 0,
                "IR function contains no blocks");

  std::unordered_map<GuestAddress, const Block*> blocks;
  blocks.reserve(function.blocks.size());
  for (const auto& block : function.blocks) {
    if (!blocks.emplace(block.guest_address, &block).second)
      return fail(VerifyError::DuplicateBlock, block.guest_address, 0,
                  "duplicate IR block guest address");
    if (auto r = verify_block_impl(block, true); !r.ok) return r;
  }
  if (!blocks.contains(function.guest_address))
    return fail(VerifyError::MissingEntryBlock, function.guest_address, 0,
                "function entry address is not an IR block");

  for (const auto& block : function.blocks) {
    // If a direct control-flow target is represented by an in-block constant,
    // the CFG must describe that exact target. Dynamic targets are validated by
    // type/shape here and by the compiled-code dispatch contract in later phases.
    std::unordered_map<ValueId, std::uint64_t> constants;
    for (const auto& instruction : block.instructions) {
      if (instruction.op == Op::Constant && instruction.result != kNoValue)
        constants[instruction.result] = instruction.imm0;
      if (instruction.args.empty()) continue;
      EdgeKind expected_kind{};
      std::size_t target_arg{};
      bool has_static_target = false;
      switch (instruction.op) {
        case Op::Branch:
          expected_kind = EdgeKind::Branch; target_arg = 0; has_static_target = true; break;
        case Op::Call:
          expected_kind = EdgeKind::Call; target_arg = 0; has_static_target = true; break;
        case Op::BranchIf:
          expected_kind = instruction.imm0 ? EdgeKind::Call : EdgeKind::Branch;
          target_arg = 1; has_static_target = true; break;
        default:
          break;
      }
      if (!has_static_target || target_arg >= instruction.args.size()) continue;
      const auto constant = constants.find(instruction.args[target_arg]);
      if (constant == constants.end()) continue;
      const auto target = static_cast<GuestAddress>(constant->second);
      const bool found = std::any_of(block.successors.begin(), block.successors.end(),
          [&](const ControlFlowEdge& edge) {
            return edge.target == target && edge.kind == expected_kind;
          });
      if (!found)
        return fail(VerifyError::InvalidBranchTarget, block.guest_address, 0,
                    "direct IR control-flow target is not represented in the CFG");
    }

    for (const auto& edge : block.successors) {
      if (!edge.local) continue;
      const auto target = blocks.find(edge.target);
      if (target == blocks.end())
        return fail(VerifyError::InvalidLocalEdge, block.guest_address, 0,
                    "local CFG edge targets a missing block");
      // Call edges annotate a local call relationship; they deliberately do not
      // participate in CFG predecessor flow because execution returns locally.
      if (edge.kind == EdgeKind::Call) continue;
      const auto& predecessors = target->second->predecessors;
      if (std::find(predecessors.begin(), predecessors.end(), block.guest_address) ==
          predecessors.end())
        return fail(VerifyError::InvalidPredecessor, block.guest_address, 0,
                    "local CFG edge is missing target predecessor metadata");
    }
    for (const auto predecessor : block.predecessors) {
      const auto source = blocks.find(predecessor);
      if (source == blocks.end())
        return fail(VerifyError::InvalidPredecessor, block.guest_address, 0,
                    "CFG predecessor references a missing block");
      const bool found = std::any_of(source->second->successors.begin(),
          source->second->successors.end(), [&](const ControlFlowEdge& edge) {
            return edge.local && edge.kind != EdgeKind::Call &&
                   edge.target == block.guest_address;
          });
      if (!found)
        return fail(VerifyError::InvalidPredecessor, block.guest_address, 0,
                    "CFG predecessor has no matching local successor edge");
    }
  }

  return {true, VerifyError::None, function.guest_address, 0, {}};
}

}  // namespace xenon::cpu::ir

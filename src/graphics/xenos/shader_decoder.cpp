#include "xenon/gpu/shader_ir.hpp"

#include <algorithm>
#include <limits>
#include <set>

namespace xenon::gpu {
namespace {

std::uint32_t bits(std::uint32_t value, unsigned first, unsigned count) {
  return (value >> first) & ((std::uint32_t{1} << count) - 1u);
}

std::int32_t signed_bits(std::uint32_t value, unsigned first, unsigned count) {
  const auto raw = bits(value, first, count);
  const auto sign = std::uint32_t{1} << (count - 1u);
  return static_cast<std::int32_t>((raw ^ sign) - sign);
}

template <typename T>
void assign_sorted(const std::set<T>& source, std::vector<T>& destination) {
  destination.assign(source.begin(), source.end());
}

struct Usage {
  std::set<std::uint16_t> floats;
  std::set<std::uint8_t> bools;
  std::set<std::uint8_t> loops;
  std::set<std::uint8_t> vertices;
  std::set<std::uint8_t> textures;
  std::set<std::uint8_t> exports;
};

Predicate decode_predicate(bool enabled, bool condition, bool clean = false) {
  return Predicate{enabled, condition, clean};
}

DecodedControlFlow decode_cf(const ControlFlowInstruction48& raw,
                             std::uint32_t index, Usage& usage,
                             ShaderReflection& reflection) {
  DecodedControlFlow result{};
  result.index = index;
  result.opcode = raw.opcode;
  result.addressing = raw.absolute_addressing() ? AddressingMode::Absolute
                                                : AddressingMode::LoopRelative;
  const auto op = static_cast<std::uint8_t>(raw.opcode);
  if (raw.is_exec()) {
    result.target = raw.exec_address();
    result.count = raw.exec_count();
    result.sequence = raw.exec_sequence();
    result.yield = raw.exec_yield();
    if (op == 3 || op == 4 || op == 13 || op == 14) {
      result.bool_constant = bits(raw.word1, 2, 8);
      result.condition = bits(raw.word1, 10, 1) != 0;
      usage.bools.insert(static_cast<std::uint8_t>(result.bool_constant));
    } else if (op == 5 || op == 6) {
      result.predicate = decode_predicate(true, bits(raw.word1, 10, 1) != 0,
                                          bits(raw.word1, 9, 1) != 0);
      reflection.uses_predication = true;
    }
  } else if (raw.opcode == ControlFlowOpcode::LoopStart ||
             raw.opcode == ControlFlowOpcode::LoopEnd) {
    result.target = bits(raw.word0, 0, 13);
    result.loop_constant = bits(raw.word0, 16, 5);
    result.repeat = raw.opcode == ControlFlowOpcode::LoopStart &&
                    bits(raw.word0, 13, 1) != 0;
    result.predicate = decode_predicate(
        raw.opcode == ControlFlowOpcode::LoopEnd && bits(raw.word0, 21, 1),
        bits(raw.word1, 10, 1) != 0);
    usage.loops.insert(static_cast<std::uint8_t>(result.loop_constant));
    reflection.uses_loops = true;
    reflection.uses_predication |= result.predicate.enabled;
  } else if (raw.opcode == ControlFlowOpcode::CondCall ||
             raw.opcode == ControlFlowOpcode::CondJump) {
    result.target = bits(raw.word0, 0, 13);
    result.unconditional = bits(raw.word0, 13, 1) != 0;
    result.condition = bits(raw.word1, 10, 1) != 0;
    result.predicate = decode_predicate(bits(raw.word0, 14, 1) != 0,
                                        result.condition);
    result.bool_constant = bits(raw.word1, 2, 8);
    if (!result.unconditional && !result.predicate.enabled) {
      usage.bools.insert(static_cast<std::uint8_t>(result.bool_constant));
    }
    reflection.uses_predication |= result.predicate.enabled;
  } else if (raw.opcode == ControlFlowOpcode::Alloc) {
    result.allocation_size = bits(raw.word0, 0, 3);
    result.allocation = static_cast<AllocationType>(bits(raw.word1, 9, 2));
  }
  reflection.uses_dynamic_addressing |= result.addressing == AddressingMode::Absolute;
  return result;
}

AluInstruction decode_alu(const ShaderInstruction96& raw, std::uint32_t address,
                          ShaderStage stage, Usage& usage,
                          ShaderReflection& reflection) {
  const auto w0 = raw.words[0], w1 = raw.words[1], w2 = raw.words[2];
  AluInstruction out{};
  out.address = address;
  out.vector_destination = static_cast<std::uint8_t>(bits(w0, 0, 6));
  out.vector_destination_relative = bits(w0, 6, 1);
  out.absolute_constants = bits(w0, 7, 1);
  out.scalar_destination = static_cast<std::uint8_t>(bits(w0, 8, 6));
  out.scalar_destination_relative = bits(w0, 14, 1);
  out.export_data = bits(w0, 15, 1);
  out.vector_write_mask = static_cast<std::uint8_t>(bits(w0, 16, 4));
  out.scalar_write_mask = static_cast<std::uint8_t>(bits(w0, 20, 4));
  out.vector_clamp = bits(w0, 24, 1);
  out.scalar_clamp = bits(w0, 25, 1);
  out.scalar_opcode = static_cast<std::uint8_t>(bits(w0, 26, 6));
  out.predicate = decode_predicate(bits(w1, 28, 1), bits(w1, 27, 1));
  out.constant_address_register_relative = bits(w1, 29, 1);
  out.vector_opcode = static_cast<std::uint8_t>(bits(w2, 24, 5));
  for (unsigned i = 0; i < 3; ++i) {
    const unsigned reverse = 2u - i;
    auto& source = out.sources[i];
    source.index = bits(w2, reverse * 8, 8);
    source.swizzle = static_cast<std::uint8_t>(bits(w1, reverse * 8, 8));
    source.negate = bits(w1, 24 + reverse, 1);
    source.temporary = bits(w2, 29 + reverse, 1);
    source.relative = source.temporary ? (source.index & 0x40u) != 0
                                       : (i == 0 ? bits(w1, 31, 1)
                                                 : bits(w1, 30, 1));
    source.absolute_value = source.temporary && (source.index & 0x80u);
    if (source.temporary) {
      source.index &= 0x3Fu;
      reflection.temporary_register_count =
          std::max(reflection.temporary_register_count, source.index + 1u);
    } else {
      usage.floats.insert(static_cast<std::uint16_t>(source.index));
    }
    reflection.uses_dynamic_addressing |= source.relative;
  }
  reflection.uses_predication |= out.predicate.enabled;
  if (out.export_data) {
    // Xenos scalar and vector ALU results share vector_dest when exporting.
    // Export register numbers are stage-specific (for example PS e61 is depth).
    usage.exports.insert(out.vector_destination);
    if (stage == ShaderStage::Pixel && out.vector_destination == 61u) {
      reflection.writes_depth = true;
    }
  } else {
    reflection.temporary_register_count = std::max(
        reflection.temporary_register_count,
        std::uint32_t(std::max(out.vector_destination, out.scalar_destination)) + 1u);
  }
  reflection.kills_pixels |= out.vector_opcode >= 24 && out.vector_opcode <= 27;
  return out;
}

DecodedInstruction decode_fetch(const ShaderInstruction96& raw,
                                std::uint32_t address, bool serialize,
                                Usage& usage, ShaderReflection& reflection) {
  const auto w0 = raw.words[0], w1 = raw.words[1], w2 = raw.words[2];
  DecodedInstruction result{};
  result.serialize = serialize;
  const auto opcode = static_cast<std::uint8_t>(bits(w0, 0, 5));
  if (opcode == 0) {
    result.kind = ShaderInstructionKind::VertexFetch;
    auto& out = result.vertex_fetch;
    out.address = address;
    out.source_register = static_cast<std::uint8_t>(bits(w0, 5, 6));
    out.source_relative = bits(w0, 11, 1);
    out.destination_register = static_cast<std::uint8_t>(bits(w0, 12, 6));
    out.destination_relative = bits(w0, 18, 1);
    out.fetch_constant = static_cast<std::uint8_t>(bits(w0, 20, 5));
    out.fetch_constant_select = static_cast<std::uint8_t>(bits(w0, 25, 2));
    out.prefetch_count = static_cast<std::uint8_t>(bits(w0, 27, 3) + 1u);
    out.source_swizzle = static_cast<std::uint8_t>(bits(w0, 30, 2));
    out.destination_swizzle = static_cast<std::uint16_t>(bits(w1, 0, 12));
    out.signed_data = bits(w1, 12, 1);
    out.normalized = !bits(w1, 13, 1);
    out.index_rounded = bits(w1, 15, 1);
    out.data_format = static_cast<std::uint8_t>(bits(w1, 16, 6));
    out.exponent_adjust = static_cast<std::int8_t>(signed_bits(w1, 24, 6));
    out.mini_fetch = bits(w1, 30, 1);
    out.predicate = decode_predicate(bits(w1, 31, 1), bits(w2, 31, 1));
    out.stride_dwords = static_cast<std::uint8_t>(bits(w2, 0, 8));
    out.offset_dwords = signed_bits(w2, 8, 23);
    usage.vertices.insert(out.fetch_constant);
    reflection.uses_dynamic_addressing |= out.source_relative || out.destination_relative;
    reflection.uses_predication |= out.predicate.enabled;
  } else {
    result.kind = ShaderInstructionKind::TextureFetch;
    auto& out = result.texture_fetch;
    out.address = address;
    out.opcode = opcode;
    out.source_register = static_cast<std::uint8_t>(bits(w0, 5, 6));
    out.source_relative = bits(w0, 11, 1);
    out.destination_register = static_cast<std::uint8_t>(bits(w0, 12, 6));
    out.destination_relative = bits(w0, 18, 1);
    out.fetch_valid_only = bits(w0, 19, 1);
    out.fetch_constant = static_cast<std::uint8_t>(bits(w0, 20, 5));
    out.unnormalized_coordinates = bits(w0, 25, 1);
    out.source_swizzle = static_cast<std::uint8_t>(bits(w0, 26, 6));
    out.destination_swizzle = static_cast<std::uint16_t>(bits(w1, 0, 12));
    out.use_computed_lod = bits(w1, 28, 1);
    out.use_register_lod = bits(w1, 29, 1);
    out.predicate = decode_predicate(bits(w1, 31, 1), bits(w2, 31, 1));
    out.use_register_gradients = bits(w2, 0, 1);
    out.lod_bias_sixteenths = static_cast<std::int8_t>(signed_bits(w2, 2, 7));
    out.dimension = static_cast<std::uint8_t>(bits(w2, 14, 2));
    out.offsets_half_texels = {static_cast<std::int8_t>(signed_bits(w2, 16, 5)),
                               static_cast<std::int8_t>(signed_bits(w2, 21, 5)),
                               static_cast<std::int8_t>(signed_bits(w2, 26, 5))};
    usage.textures.insert(out.fetch_constant);
    reflection.uses_dynamic_addressing |= out.source_relative || out.destination_relative;
    reflection.uses_predication |= out.predicate.enabled;
  }
  return result;
}

}  // namespace

DecodedShader ShaderDecoder::decode(const ShaderProgram& program) {
  DecodedShader result{};
  result.stage = program.stage();
  result.source_hash = program.hash();
  result.start_slot = program.start_slot();
  if (!program.complete_instruction_stream()) {
    result.diagnostics.emplace_back("shader bytecode is not a whole number of 96-bit groups");
    return result;
  }

  Usage usage;
  std::set<std::uint32_t> decoded_addresses;
  bool ended = false;
  for (std::uint32_t group_index = 0;
       group_index < program.instruction_count() && !ended; ++group_index) {
    const auto pair = program.control_flow_pair(group_index);
    for (unsigned half = 0; half < 2; ++half) {
      const auto cf_index = group_index * 2u + half;
      const auto decoded = decode_cf(pair.instructions[half], cf_index, usage,
                                     result.reflection);
      result.control_flow.push_back(decoded);
      if (pair.instructions[half].is_exec()) {
        for (std::uint32_t i = 0; i < decoded.count; ++i) {
          const auto address = decoded.target + i;
          if (address >= program.instruction_count()) {
            result.diagnostics.emplace_back("exec range exceeds shader microcode");
            continue;
          }
          if (!decoded_addresses.insert(address).second) continue;
          const bool fetch = ((decoded.sequence >> (i * 2u)) & 1u) != 0;
          const bool serialize = ((decoded.sequence >> (i * 2u + 1u)) & 1u) != 0;
          if (fetch) {
            result.instructions.push_back(decode_fetch(program.instruction(address), address,
                                                       serialize, usage, result.reflection));
          } else {
            DecodedInstruction instruction{};
            instruction.kind = ShaderInstructionKind::Alu;
            instruction.serialize = serialize;
            instruction.alu = decode_alu(program.instruction(address), address,
                                         program.stage(), usage, result.reflection);
            result.instructions.push_back(instruction);
          }
        }
      }
      // Conditional-end opcodes terminate only invocations that satisfy their
      // condition. The remaining control-flow stream is still reachable by the
      // other invocations, so only an unconditional EXEC_END closes decoding.
      ended = pair.instructions[half].opcode == ControlFlowOpcode::ExecEnd;
      if (ended) break;
    }
  }
  assign_sorted(usage.floats, result.reflection.float_constants);
  assign_sorted(usage.bools, result.reflection.bool_constants);
  assign_sorted(usage.loops, result.reflection.loop_constants);
  assign_sorted(usage.vertices, result.reflection.vertex_fetch_constants);
  assign_sorted(usage.textures, result.reflection.texture_fetch_constants);
  assign_sorted(usage.exports, result.reflection.exports);
  for (auto export_index : result.reflection.exports) {
    // eA is 32 and eM0..eM4 are 33..37 for both stages.
    if (export_index >= 33u && export_index <= 37u) {
      ++result.reflection.memory_exports;
    }
    if (program.stage() == ShaderStage::Vertex) {
      if (export_index <= 15u) ++result.reflection.interpolator_exports;
      if (export_index == 62u) ++result.reflection.position_exports;
    } else {
      if (export_index <= 3u) ++result.reflection.color_exports;
      if (export_index == 61u) result.reflection.writes_depth = true;
    }
  }
  result.complete = ended && result.diagnostics.empty();
  if (!ended) result.diagnostics.emplace_back("shader has no terminating control-flow instruction");
  return result;
}

}  // namespace xenon::gpu

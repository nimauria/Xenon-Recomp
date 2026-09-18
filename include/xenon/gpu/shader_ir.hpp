#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "xenon/gpu/shader.hpp"

namespace xenon::gpu {

enum class ShaderInstructionKind : std::uint8_t { Alu, VertexFetch, TextureFetch };
enum class AddressingMode : std::uint8_t { LoopRelative, Absolute };
enum class AllocationType : std::uint8_t { None, Position, InterpolatorsOrColors, Memory };

struct Predicate {
  bool enabled{};
  bool condition{};
  bool clean{};
};

struct DecodedControlFlow {
  std::uint32_t index{};
  ControlFlowOpcode opcode{ControlFlowOpcode::Nop};
  std::uint32_t target{};
  std::uint32_t count{};
  std::uint32_t sequence{};
  std::uint32_t bool_constant{};
  std::uint32_t loop_constant{};
  AddressingMode addressing{AddressingMode::LoopRelative};
  Predicate predicate{};
  bool condition{};
  bool unconditional{};
  bool repeat{};
  bool yield{};
  AllocationType allocation{AllocationType::None};
  std::uint32_t allocation_size{};
};

struct AluSource {
  std::uint32_t index{};
  std::uint8_t swizzle{};
  bool temporary{};
  bool relative{};
  bool absolute_value{};
  bool negate{};
};

struct AluInstruction {
  std::uint32_t address{};
  std::uint8_t vector_opcode{};
  std::uint8_t scalar_opcode{};
  std::uint8_t vector_destination{};
  std::uint8_t scalar_destination{};
  std::uint8_t vector_write_mask{};
  std::uint8_t scalar_write_mask{};
  bool export_data{};
  bool vector_destination_relative{};
  bool scalar_destination_relative{};
  bool vector_clamp{};
  bool scalar_clamp{};
  bool absolute_constants{};
  bool constant_address_register_relative{};
  Predicate predicate{};
  std::array<AluSource, 3> sources{};
};

struct VertexFetchInstruction {
  std::uint32_t address{};
  std::uint8_t source_register{};
  std::uint8_t source_swizzle{};
  std::uint8_t destination_register{};
  std::uint16_t destination_swizzle{};
  std::uint8_t fetch_constant{};
  std::uint8_t fetch_constant_select{};
  std::uint8_t prefetch_count{};
  std::uint8_t data_format{};
  std::uint8_t stride_dwords{};
  std::int32_t offset_dwords{};
  std::int8_t exponent_adjust{};
  bool source_relative{};
  bool destination_relative{};
  bool mini_fetch{};
  bool signed_data{};
  bool normalized{};
  bool index_rounded{};
  Predicate predicate{};
};

struct TextureFetchInstruction {
  std::uint32_t address{};
  std::uint8_t opcode{};
  std::uint8_t source_register{};
  std::uint8_t source_swizzle{};
  std::uint8_t destination_register{};
  std::uint16_t destination_swizzle{};
  std::uint8_t fetch_constant{};
  std::uint8_t dimension{};
  std::int8_t lod_bias_sixteenths{};
  std::array<std::int8_t, 3> offsets_half_texels{};
  bool source_relative{};
  bool destination_relative{};
  bool unnormalized_coordinates{};
  bool fetch_valid_only{};
  bool use_computed_lod{};
  bool use_register_lod{};
  bool use_register_gradients{};
  Predicate predicate{};
};

struct DecodedInstruction {
  ShaderInstructionKind kind{ShaderInstructionKind::Alu};
  bool serialize{};
  AluInstruction alu{};
  VertexFetchInstruction vertex_fetch{};
  TextureFetchInstruction texture_fetch{};
};

struct ShaderReflection {
  std::vector<std::uint16_t> float_constants{};
  std::vector<std::uint8_t> bool_constants{};
  std::vector<std::uint8_t> loop_constants{};
  std::vector<std::uint8_t> vertex_fetch_constants{};
  std::vector<std::uint8_t> texture_fetch_constants{};
  std::vector<std::uint8_t> exports{};
  std::uint32_t temporary_register_count{};
  std::uint32_t position_exports{};
  std::uint32_t interpolator_exports{};
  std::uint32_t color_exports{};
  std::uint32_t memory_exports{};
  bool uses_predication{};
  bool uses_loops{};
  bool uses_dynamic_addressing{};
  bool kills_pixels{};
  bool writes_depth{};
};

struct DecodedShader {
  ShaderStage stage{ShaderStage::Vertex};
  std::uint64_t source_hash{};
  std::uint32_t start_slot{};
  std::vector<DecodedControlFlow> control_flow{};
  std::vector<DecodedInstruction> instructions{};
  ShaderReflection reflection{};
  std::vector<std::string> diagnostics{};
  bool complete{};
};

class ShaderDecoder {
 public:
  [[nodiscard]] static DecodedShader decode(const ShaderProgram& program);
};

}  // namespace xenon::gpu

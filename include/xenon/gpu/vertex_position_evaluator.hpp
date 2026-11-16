#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "xenon/gpu/shader_ir.hpp"

namespace xenon::gpu {

// A CPU twin of a lowered Xenos vertex shader (HlslShaderLowerer), run for
// the few vertices of a draw the renderer cannot otherwise bound: one with
// clipping disabled or the viewport transform off, whose exported position is
// already in render-target pixels (Direct3D's EDRAM clears and copies). How
// far down such a draw reaches decides how tall its render target is; without
// it the target has to cover the whole open scissor, which for AC6's clears
// is every row of EDRAM (xenia runs the vertex shader on the CPU for the same
// reason, execute_unclipped_draw_vs_on_cpu).
//
// It executes the decoded control flow, ALU and vertex fetches op for op as
// the generated HLSL does (vector_operation, scalar_operation in shader_translation.cpp,
// xenon_vertex_decode, the loop/call/predicate machinery), reading the draw's
// shader constants exactly as the GPU receives them - the constant buffer of
// ResourceStateTracker::constants(): float, bool, loop and packed vertex
// fetch constants and XenonDrawState - and vertex data through `memory`.
// Texture fetches have no CPU counterpart here; a shader that samples is
// reported unsupported and its draw keeps the scissor bound.
struct VertexEvaluation {
  // The exported position (e62) as the shader wrote it.
  std::array<float, 4> position{};
  // Where the vertex lands in render-target pixels: PA_CL_VTE_CNTL's formats
  // and the guest viewport transform applied, divided by W - the same
  // mapping xenon_host_position() makes before the host's NDC scaling.
  std::array<float, 2> pixel{};
  bool valid{};
  std::string error{};
};

// Reads guest physical memory: true when all of `out` was filled.
using GuestPhysicalReader =
    std::function<bool(std::uint32_t physical_address, std::span<std::byte> out)>;

// A decoded vertex shader prepared for repeated CPU evaluation: whether it can
// be evaluated at all, and its instructions and control flow laid out by
// address so each step of a run is an array index. Immutable and
// self-contained (it copies what it needs from the decoded shader), so the
// renderer builds one per shader and reuses it for every vertex of every draw
// - before it, each vertex rebuilt two hash maps over the whole shader and
// rescanned it for support.
class VertexEvaluatorPlan {
 public:
  explicit VertexEvaluatorPlan(const DecodedShader& shader);

  [[nodiscard]] bool supported() const noexcept { return reason_.empty(); }
  // Why the shader cannot be evaluated on the CPU, or empty when it can.
  [[nodiscard]] const std::string& unsupported_reason() const noexcept { return reason_; }

  // The instruction at microcode `address`, or null when the decoded IR has
  // none there.
  [[nodiscard]] const DecodedInstruction* instruction(std::uint32_t address) const noexcept {
    const auto slot = static_cast<std::uint64_t>(address) - instruction_base_;
    if (address < instruction_base_ || slot >= instruction_slots_.size()) return nullptr;
    const auto index = instruction_slots_[static_cast<std::size_t>(slot)];
    return index < 0 ? nullptr : &instructions_[static_cast<std::size_t>(index)];
  }
  // The control-flow instruction with index `index`, or null.
  [[nodiscard]] const DecodedControlFlow* control_flow(std::uint32_t index) const noexcept {
    if (index >= control_flow_slots_.size()) return nullptr;
    const auto slot = control_flow_slots_[index];
    return slot < 0 ? nullptr : &control_flow_[static_cast<std::size_t>(slot)];
  }

 private:
  std::string reason_;
  std::vector<DecodedInstruction> instructions_;
  std::vector<std::int32_t> instruction_slots_;  // address - base -> instructions_
  std::uint32_t instruction_base_{};
  std::vector<DecodedControlFlow> control_flow_;
  std::vector<std::int32_t> control_flow_slots_;  // cf index -> control_flow_
};

class VertexPositionEvaluator {
 public:
  // Why `shader` cannot be evaluated on the CPU, or empty when it can.
  [[nodiscard]] static std::string unsupported_reason(const DecodedShader& shader);

  // Runs `shader` for the vertex the host would see as SV_VertexID
  // `vertex_id` (an auto-generated index or the index buffer's value; the
  // shader applies VGT_INDX_OFFSET and the min/max clamp itself, from
  // XenonDrawState). `constants` is the draw's whole shader constant buffer.
  // Builds a plan for the one call; repeated evaluation passes a plan.
  [[nodiscard]] static VertexEvaluation evaluate(const DecodedShader& shader,
                                                 std::span<const std::byte> constants,
                                                 std::uint32_t vertex_id,
                                                 const GuestPhysicalReader& memory);
  [[nodiscard]] static VertexEvaluation evaluate(const VertexEvaluatorPlan& plan,
                                                 std::span<const std::byte> constants,
                                                 std::uint32_t vertex_id,
                                                 const GuestPhysicalReader& memory);
};

}  // namespace xenon::gpu

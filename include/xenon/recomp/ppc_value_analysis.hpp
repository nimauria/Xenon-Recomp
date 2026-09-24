#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "xenon/cpu/instruction.hpp"
#include "xenon/xbox/xex_loader.hpp"

namespace xenon::recomp::value_analysis {

// Gen 6 abstract PPC value lattice.  The analyzer intentionally keeps this
// small and bounded: values are facts useful for control-flow recovery, not a
// general symbolic-expression tree.
enum class AbstractValueKind : std::uint8_t {
  Unknown,
  Constant,
  Address,
  AddressPlusOffset,
  FiniteSet,
  Range,
  StackRelative,
  LoadedFromReadOnlyTable,
};

struct AbstractValue {
  AbstractValueKind kind{AbstractValueKind::Unknown};
  std::uint64_t value{};       // exact Constant/Address/table-loaded value
  std::uint32_t base{};        // AddressPlusOffset / StackRelative base tag
  std::int64_t offset{};       // AddressPlusOffset / StackRelative displacement
  std::uint32_t range_min{};   // inclusive
  std::uint32_t range_max{};   // inclusive
  std::vector<std::uint32_t> finite_values;
  std::optional<std::uint32_t> loaded_from;

  [[nodiscard]] static AbstractValue unknown() noexcept;
  [[nodiscard]] static AbstractValue constant(std::uint64_t value) noexcept;
  [[nodiscard]] static AbstractValue address(std::uint32_t value) noexcept;
  [[nodiscard]] static AbstractValue address_plus_offset(std::uint32_t base,
                                                         std::int64_t offset) noexcept;
  [[nodiscard]] static AbstractValue stack_relative(std::int64_t offset) noexcept;
  [[nodiscard]] static AbstractValue loaded_read_only(std::uint64_t value,
                                                      std::uint32_t source_address) noexcept;
  [[nodiscard]] static AbstractValue finite_set(std::vector<std::uint32_t> values);
  [[nodiscard]] static AbstractValue range(std::uint32_t minimum, std::uint32_t maximum) noexcept;

  [[nodiscard]] bool is_unknown() const noexcept { return kind == AbstractValueKind::Unknown; }
  [[nodiscard]] std::optional<std::uint32_t> exact_u32() const noexcept;
  [[nodiscard]] std::vector<std::uint32_t> possible_u32(std::size_t max_values = 16u) const;
};

[[nodiscard]] const char* abstract_value_kind_name(AbstractValueKind kind) noexcept;
[[nodiscard]] AbstractValue join(const AbstractValue& lhs, const AbstractValue& rhs,
                                 std::size_t max_finite_values = 8u);

enum class IndirectResolverKind : std::uint8_t {
  None,
  FastValue,
  ReadOnlyTable,
  BackwardSlice,
};

struct IndirectResolution {
  IndirectResolverKind kind{IndirectResolverKind::None};
  std::vector<std::uint32_t> targets;
  std::size_t instructions_examined{};
  std::optional<std::uint32_t> table_address;
};

[[nodiscard]] const char* indirect_resolver_name(IndirectResolverKind kind) noexcept;

// Cheap forward state used on every decoded instruction. Unknown instructions
// invalidate only registers they may write; unrelated compares/branches/FP/
// vector work no longer destroys all useful GPR knowledge.
class PpcValueTracker {
 public:
  explicit PpcValueTracker(const xbox::XexImage& image);

  void reset() noexcept;
  void step(const cpu::DecodedInstruction& instruction);

  [[nodiscard]] const AbstractValue& gpr(std::uint32_t index) const noexcept;
  [[nodiscard]] const AbstractValue& ctr() const noexcept { return ctr_; }
  [[nodiscard]] const AbstractValue& lr() const noexcept { return lr_; }

 private:
  const xbox::XexImage* image_{};
  std::array<AbstractValue, 32> gpr_{};
  AbstractValue ctr_{};
  AbstractValue lr_{};
};

// Tiered indirect-flow resolution. FastValue is attempted first. Only when it
// fails do we run the bounded backward slicer over the supplied prior
// instructions. The slicer follows the last writer of CTR/LR and recursively
// reconstructs the small family of integer/address/load operations commonly
// used by Xbox 360 compilers for function pointers and switch dispatch.
[[nodiscard]] IndirectResolution resolve_indirect_flow(
    const cpu::DecodedInstruction& branch,
    const PpcValueTracker& tracker,
    std::span<const cpu::DecodedInstruction> history,
    const xbox::XexImage& image,
    std::size_t max_slice_instructions = 48u,
    std::size_t max_recursion_depth = 12u);

}  // namespace xenon::recomp::value_analysis

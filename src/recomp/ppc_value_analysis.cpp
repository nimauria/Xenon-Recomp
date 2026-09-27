#include "xenon/recomp/ppc_value_analysis.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <string_view>

namespace xenon::recomp::value_analysis {
namespace {

[[nodiscard]] const xbox::XexSection* containing_section(const xbox::XexImage& image,
                                                         std::uint32_t address) noexcept {
  for (const auto& section : image.sections) {
    const auto size = std::max(section.virtual_size, section.raw_size);
    if (size == 0u) continue;
    const auto end = static_cast<std::uint64_t>(section.virtual_address) + size;
    if (address >= section.virtual_address && static_cast<std::uint64_t>(address) < end)
      return &section;
  }
  return nullptr;
}

[[nodiscard]] bool mapped_address(const xbox::XexImage& image, std::uint32_t address) noexcept {
  return containing_section(image, address) != nullptr;
}

[[nodiscard]] bool executable_address(const xbox::XexImage& image, std::uint32_t address) noexcept {
  const auto* section = containing_section(image, address);
  return section != nullptr && section->executable && (address & 3u) == 0u;
}

[[nodiscard]] AbstractValue classify_exact(const xbox::XexImage& image, std::uint64_t raw) {
  const auto value = static_cast<std::uint32_t>(raw);
  if ((raw >> 32u) == 0u && mapped_address(image, value)) return AbstractValue::address(value);
  return AbstractValue::constant(raw);
}

[[nodiscard]] std::optional<std::uint64_t> read_static_unsigned(const xbox::XexImage& image,
                                                                std::uint32_t address,
                                                                std::size_t width) {
  const auto* section = containing_section(image, address);
  // Writable tables are runtime state, not a static fact.  Never fold them.
  if (!section || section->writable || !section->readable || address < section->virtual_address)
    return std::nullopt;
  const auto offset = static_cast<std::size_t>(address - section->virtual_address);
  if (offset + width > section->bytes.size()) return std::nullopt;
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < width; ++i)
    value = (value << 8u) | std::to_integer<std::uint8_t>(section->bytes[offset + i]);
  return value;
}

[[nodiscard]] AbstractValue add_value(const xbox::XexImage& image, const AbstractValue& lhs,
                                      std::int64_t rhs) {
  if (lhs.kind == AbstractValueKind::StackRelative)
    return AbstractValue::stack_relative(lhs.offset + rhs);
  if (lhs.kind == AbstractValueKind::Address || lhs.kind == AbstractValueKind::AddressPlusOffset ||
      lhs.kind == AbstractValueKind::LoadedFromReadOnlyTable) {
    if (const auto exact = lhs.exact_u32()) {
      const auto result = static_cast<std::uint32_t>(*exact + static_cast<std::uint32_t>(rhs));
      if (mapped_address(image, result)) {
        const auto base = lhs.kind == AbstractValueKind::AddressPlusOffset ? lhs.base : *exact;
        const auto prior_offset = lhs.kind == AbstractValueKind::AddressPlusOffset ? lhs.offset : 0;
        return AbstractValue::address_plus_offset(base, prior_offset + rhs);
      }
      return AbstractValue::constant(result);
    }
  }
  if (const auto exact = lhs.exact_u32())
    return classify_exact(image, static_cast<std::uint32_t>(*exact + static_cast<std::uint32_t>(rhs)));
  if (lhs.kind == AbstractValueKind::FiniteSet) {
    std::vector<std::uint32_t> values;
    values.reserve(lhs.finite_values.size());
    for (const auto value : lhs.finite_values)
      values.push_back(value + static_cast<std::uint32_t>(rhs));
    return AbstractValue::finite_set(std::move(values));
  }
  return AbstractValue::unknown();
}

template <typename Fn>
[[nodiscard]] AbstractValue exact_binary(const xbox::XexImage& image, const AbstractValue& lhs,
                                         const AbstractValue& rhs, Fn&& fn) {
  const auto left = lhs.exact_u32();
  const auto right = rhs.exact_u32();
  if (left && right) return classify_exact(image, fn(*left, *right));
  return AbstractValue::unknown();
}

[[nodiscard]] bool mnemonic_is(std::string_view mnemonic,
                               std::initializer_list<std::string_view> names) noexcept {
  return std::find(names.begin(), names.end(), mnemonic) != names.end();
}

[[nodiscard]] bool is_load(std::string_view mnemonic) noexcept {
  return !mnemonic.empty() && mnemonic.front() == 'l' &&
         !mnemonic_is(mnemonic, {"lwarx", "ldarx"});
}

[[nodiscard]] bool is_store(std::string_view mnemonic) noexcept {
  return mnemonic.size() >= 2u && mnemonic[0] == 's' && mnemonic[1] == 't';
}

[[nodiscard]] bool update_form(std::string_view mnemonic) noexcept {
  return mnemonic.ends_with("u") || mnemonic.ends_with("ux");
}

[[nodiscard]] std::optional<std::size_t> load_width(std::string_view mnemonic) noexcept {
  if (mnemonic.starts_with("lbz")) return 1u;
  if (mnemonic.starts_with("lhz") || mnemonic.starts_with("lha")) return 2u;
  if (mnemonic.starts_with("lwz")) return 4u;
  if (mnemonic == "ld" || mnemonic == "ldu" || mnemonic == "ldx" || mnemonic == "ldux") return 8u;
  return std::nullopt;
}

[[nodiscard]] AbstractValue load_value(const xbox::XexImage& image, const AbstractValue& address,
                                       std::string_view mnemonic) {
  const auto exact = address.exact_u32();
  const auto width = load_width(mnemonic);
  if (!exact || !width) return AbstractValue::unknown();
  const auto raw = read_static_unsigned(image, *exact, *width);
  if (!raw) return AbstractValue::unknown();
  std::uint64_t value = *raw;
  if (mnemonic.starts_with("lha") && *width == 2u)
    value = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int16_t>(value)));
  return AbstractValue::loaded_read_only(value, *exact);
}

[[nodiscard]] AbstractValue effective_address_d(const xbox::XexImage& image,
                                                const std::array<AbstractValue, 32>& gpr,
                                                const cpu::DecodedInstruction& instruction) {
  const auto base = instruction.ra() == 0u ? AbstractValue::constant(0u) : gpr[instruction.ra()];
  const auto displacement = static_cast<std::int64_t>(instruction.simm16());
  return add_value(image, base, displacement);
}

[[nodiscard]] AbstractValue effective_address_ds(const xbox::XexImage& image,
                                                 const std::array<AbstractValue, 32>& gpr,
                                                 const cpu::DecodedInstruction& instruction) {
  const auto base = instruction.ra() == 0u ? AbstractValue::constant(0u) : gpr[instruction.ra()];
  return add_value(image, base, instruction.ds_displacement());
}

[[nodiscard]] AbstractValue effective_address_x(const xbox::XexImage& image,
                                                const std::array<AbstractValue, 32>& gpr,
                                                const cpu::DecodedInstruction& instruction) {
  const auto base = instruction.ra() == 0u ? AbstractValue::constant(0u) : gpr[instruction.ra()];
  return exact_binary(image, base, gpr[instruction.rb()], [](std::uint32_t a, std::uint32_t b) {
    return a + b;
  });
}

// Xbox 360 PPC calling convention: r0 and the argument/scratch bank below
// r14 are caller-clobbered for analysis purposes; r1 is the stack pointer and
// r14-r31 are nonvolatile. r2/r13 have platform-specific reserved roles in
// some code, but invalidating them is the conservative choice here.
[[nodiscard]] bool call_clobbers_gpr(std::uint32_t reg) noexcept {
  return reg == 0u || (reg >= 2u && reg <= 13u);
}

// Whether an instruction can clobber a specific GPR when the slicer does not
// otherwise understand it. False negatives here would be unsound, so the
// integer fallback intentionally treats both RT/RS and RA fields as possible
// destinations.
[[nodiscard]] bool may_write_gpr(const cpu::DecodedInstruction& instruction,
                                 std::uint32_t reg) noexcept {
  if (!instruction.valid()) return true;
  const auto mnemonic = instruction.mnemonic();
  if (instruction.info->group == cpu::InstructionGroup::Branch)
    return instruction.lk() && call_clobbers_gpr(reg);
  if (instruction.info->group == cpu::InstructionGroup::FloatingPoint ||
      instruction.info->group == cpu::InstructionGroup::Vector)
    return false;
  if (mnemonic.starts_with("cmp")) return false;
  if (mnemonic == "mtspr") return false;
  if (mnemonic == "mfspr") return instruction.rt() == reg;
  if (instruction.info->group == cpu::InstructionGroup::Memory) {
    if (is_load(mnemonic) && instruction.rt() == reg) return true;
    if (update_form(mnemonic) && instruction.ra() == reg) return true;
    return false;
  }
  if (instruction.info->group == cpu::InstructionGroup::Integer)
    return instruction.rt() == reg || instruction.ra() == reg;
  return false;
}

class BackwardSlicer {
 public:
  BackwardSlicer(std::span<const cpu::DecodedInstruction> history,
                 const xbox::XexImage& image,
                 std::size_t max_instructions,
                 std::size_t max_depth)
      : history_(history), image_(image), max_instructions_(max_instructions), max_depth_(max_depth) {}

  [[nodiscard]] AbstractValue resolve_special(std::uint32_t spr) {
    if (history_.empty()) return AbstractValue::unknown();
    const auto begin = history_.size() > max_instructions_ ? history_.size() - max_instructions_ : 0u;
    for (std::size_t pos = history_.size(); pos-- > begin;) {
      ++examined_;
      const auto& instruction = history_[pos];
      if (instruction.mnemonic() == "mtspr" && instruction.spr() == spr)
        return resolve_reg(instruction.rs(), pos, 0u);
      // A linked branch overwrites LR even without an explicit mtspr and a
      // callee may freely clobber CTR. Do not slice CTR through a call.
      if (instruction.info->group == cpu::InstructionGroup::Branch && instruction.lk()) {
        if (spr == 8u) return AbstractValue::address(instruction.address + 4u);
        if (spr == 9u) return AbstractValue::unknown();
      }
    }
    return AbstractValue::unknown();
  }

  [[nodiscard]] std::size_t examined() const noexcept { return examined_; }

 private:
  [[nodiscard]] AbstractValue resolve_reg(std::uint32_t reg, std::size_t before, std::size_t depth) {
    if (depth >= max_depth_) return AbstractValue::unknown();
    if (reg == 1u && before == 0u) return AbstractValue::stack_relative(0);
    const auto begin = before > max_instructions_ ? before - max_instructions_ : 0u;
    for (std::size_t pos = before; pos-- > begin;) {
      ++examined_;
      const auto& instruction = history_[pos];
      const auto mnemonic = instruction.mnemonic();

      if (mnemonic_is(mnemonic, {"addi", "addis"}) && instruction.rt() == reg) {
        const auto source = instruction.ra() == 0u ? AbstractValue::constant(0u)
                                                   : resolve_reg(instruction.ra(), pos, depth + 1u);
        const auto delta = mnemonic == "addis"
            ? (static_cast<std::int64_t>(instruction.simm16()) << 16)
            : static_cast<std::int64_t>(instruction.simm16());
        return add_value(image_, source, delta);
      }
      if (mnemonic_is(mnemonic, {"ori", "oris", "xori", "xoris"}) && instruction.ra() == reg) {
        const auto source = resolve_reg(instruction.rs(), pos, depth + 1u);
        const auto exact = source.exact_u32();
        if (!exact) return AbstractValue::unknown();
        const auto imm = mnemonic.ends_with("is")
            ? (static_cast<std::uint32_t>(instruction.uimm16()) << 16u)
            : static_cast<std::uint32_t>(instruction.uimm16());
        const auto result = mnemonic.starts_with("xor") ? (*exact ^ imm) : (*exact | imm);
        return classify_exact(image_, result);
      }
      if (mnemonic_is(mnemonic, {"orx", "andx", "xorx"}) && instruction.ra() == reg) {
        const auto lhs = resolve_reg(instruction.rs(), pos, depth + 1u);
        const auto rhs = resolve_reg(instruction.rb(), pos, depth + 1u);
        if (mnemonic == "orx")
          return exact_binary(image_, lhs, rhs, [](std::uint32_t a, std::uint32_t b) { return a | b; });
        if (mnemonic == "andx")
          return exact_binary(image_, lhs, rhs, [](std::uint32_t a, std::uint32_t b) { return a & b; });
        return exact_binary(image_, lhs, rhs, [](std::uint32_t a, std::uint32_t b) { return a ^ b; });
      }
      if (mnemonic_is(mnemonic, {"addx", "subfx"}) && instruction.rt() == reg) {
        const auto a = resolve_reg(instruction.ra(), pos, depth + 1u);
        const auto b = resolve_reg(instruction.rb(), pos, depth + 1u);
        if (mnemonic == "addx")
          return exact_binary(image_, a, b, [](std::uint32_t x, std::uint32_t y) { return x + y; });
        return exact_binary(image_, a, b, [](std::uint32_t x, std::uint32_t y) { return y - x; });
      }
      if (instruction.info->group == cpu::InstructionGroup::Memory && is_load(mnemonic) &&
          instruction.rt() == reg) {
        AbstractValue address = AbstractValue::unknown();
        if (instruction.info->format == cpu::InstructionFormat::D) {
          const auto base = instruction.ra() == 0u ? AbstractValue::constant(0u)
                                                   : resolve_reg(instruction.ra(), pos, depth + 1u);
          address = add_value(image_, base, instruction.simm16());
        } else if (instruction.info->format == cpu::InstructionFormat::DS) {
          const auto base = instruction.ra() == 0u ? AbstractValue::constant(0u)
                                                   : resolve_reg(instruction.ra(), pos, depth + 1u);
          address = add_value(image_, base, instruction.ds_displacement());
        } else if (instruction.info->format == cpu::InstructionFormat::X) {
          const auto base = instruction.ra() == 0u ? AbstractValue::constant(0u)
                                                   : resolve_reg(instruction.ra(), pos, depth + 1u);
          const auto index = resolve_reg(instruction.rb(), pos, depth + 1u);
          address = exact_binary(image_, base, index,
                                 [](std::uint32_t x, std::uint32_t y) { return x + y; });
        }
        return load_value(image_, address, mnemonic);
      }
      if (mnemonic == "mfspr" && instruction.rt() == reg) {
        // Recursing through arbitrary SPRs is deliberately not supported.
        // LR/CTR are only resolved as top-level branch sources.
        return AbstractValue::unknown();
      }
      if (may_write_gpr(instruction, reg)) return AbstractValue::unknown();
    }
    return reg == 1u ? AbstractValue::stack_relative(0) : AbstractValue::unknown();
  }

  std::span<const cpu::DecodedInstruction> history_;
  const xbox::XexImage& image_;
  std::size_t max_instructions_{};
  std::size_t max_depth_{};
  std::size_t examined_{};
};

[[nodiscard]] std::vector<std::uint32_t> valid_executable_targets(const xbox::XexImage& image,
                                                                  const AbstractValue& value) {
  std::vector<std::uint32_t> result;
  for (const auto target : value.possible_u32()) {
    if (executable_address(image, target)) result.push_back(target);
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

}  // namespace

AbstractValue AbstractValue::unknown() noexcept { return {}; }
AbstractValue AbstractValue::constant(std::uint64_t value) noexcept {
  AbstractValue result{};
  result.kind = AbstractValueKind::Constant;
  result.value = value;
  return result;
}
AbstractValue AbstractValue::address(std::uint32_t value) noexcept {
  AbstractValue result{};
  result.kind = AbstractValueKind::Address;
  result.value = value;
  result.base = value;
  return result;
}
AbstractValue AbstractValue::address_plus_offset(std::uint32_t base, std::int64_t offset) noexcept {
  AbstractValue result{};
  result.kind = AbstractValueKind::AddressPlusOffset;
  result.base = base;
  result.offset = offset;
  result.value = static_cast<std::uint32_t>(base + static_cast<std::uint32_t>(offset));
  return result;
}
AbstractValue AbstractValue::stack_relative(std::int64_t offset) noexcept {
  AbstractValue result{};
  result.kind = AbstractValueKind::StackRelative;
  result.base = 1u;
  result.offset = offset;
  return result;
}
AbstractValue AbstractValue::loaded_read_only(std::uint64_t value, std::uint32_t source_address) noexcept {
  AbstractValue result{};
  result.kind = AbstractValueKind::LoadedFromReadOnlyTable;
  result.value = value;
  result.loaded_from = source_address;
  return result;
}
AbstractValue AbstractValue::finite_set(std::vector<std::uint32_t> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  if (values.empty()) return unknown();
  if (values.size() == 1u) return constant(values.front());
  AbstractValue result{};
  result.kind = AbstractValueKind::FiniteSet;
  result.finite_values = std::move(values);
  return result;
}
AbstractValue AbstractValue::range(std::uint32_t minimum, std::uint32_t maximum) noexcept {
  if (minimum == maximum) return constant(minimum);
  AbstractValue result{};
  result.kind = AbstractValueKind::Range;
  result.range_min = std::min(minimum, maximum);
  result.range_max = std::max(minimum, maximum);
  return result;
}

std::optional<std::uint32_t> AbstractValue::exact_u32() const noexcept {
  switch (kind) {
    case AbstractValueKind::Constant:
    case AbstractValueKind::Address:
    case AbstractValueKind::AddressPlusOffset:
    case AbstractValueKind::LoadedFromReadOnlyTable:
      if ((value >> 32u) == 0u) return static_cast<std::uint32_t>(value);
      return std::nullopt;
    case AbstractValueKind::FiniteSet:
      if (finite_values.size() == 1u) return finite_values.front();
      return std::nullopt;
    case AbstractValueKind::Range:
      if (range_min == range_max) return range_min;
      return std::nullopt;
    case AbstractValueKind::Unknown:
    case AbstractValueKind::StackRelative:
      return std::nullopt;
  }
  return std::nullopt;
}

std::vector<std::uint32_t> AbstractValue::possible_u32(std::size_t max_values) const {
  if (const auto exact = exact_u32()) return {*exact};
  if (kind == AbstractValueKind::FiniteSet && finite_values.size() <= max_values) return finite_values;
  if (kind == AbstractValueKind::Range && range_max >= range_min &&
      static_cast<std::uint64_t>(range_max) - range_min + 1u <= max_values) {
    std::vector<std::uint32_t> values;
    values.reserve(static_cast<std::size_t>(range_max - range_min + 1u));
    for (std::uint32_t value = range_min;; ++value) {
      values.push_back(value);
      if (value == range_max) break;
    }
    return values;
  }
  return {};
}

const char* abstract_value_kind_name(AbstractValueKind kind) noexcept {
  switch (kind) {
    case AbstractValueKind::Unknown: return "unknown";
    case AbstractValueKind::Constant: return "constant";
    case AbstractValueKind::Address: return "address";
    case AbstractValueKind::AddressPlusOffset: return "address+offset";
    case AbstractValueKind::FiniteSet: return "finite-set";
    case AbstractValueKind::Range: return "range";
    case AbstractValueKind::StackRelative: return "stack-relative";
    case AbstractValueKind::LoadedFromReadOnlyTable: return "read-only-table-load";
  }
  return "unknown";
}

AbstractValue join(const AbstractValue& lhs, const AbstractValue& rhs, std::size_t max_finite_values) {
  // Unknown is the top element: if a value is not known on one incoming
  // path, merging it with a precise fact cannot manufacture certainty.
  if (lhs.kind == AbstractValueKind::Unknown || rhs.kind == AbstractValueKind::Unknown)
    return AbstractValue::unknown();
  const auto left = lhs.possible_u32(max_finite_values);
  const auto right = rhs.possible_u32(max_finite_values);
  if (!left.empty() && !right.empty()) {
    std::vector<std::uint32_t> values = left;
    values.insert(values.end(), right.begin(), right.end());
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    if (values.size() <= max_finite_values) return AbstractValue::finite_set(std::move(values));
    return AbstractValue::range(values.front(), values.back());
  }
  if (lhs.kind == AbstractValueKind::StackRelative && rhs.kind == AbstractValueKind::StackRelative &&
      lhs.offset == rhs.offset)
    return lhs;
  return AbstractValue::unknown();
}

const char* indirect_resolver_name(IndirectResolverKind kind) noexcept {
  switch (kind) {
    case IndirectResolverKind::None: return "none";
    case IndirectResolverKind::FastValue: return "fast-value";
    case IndirectResolverKind::ReadOnlyTable: return "read-only-table";
    case IndirectResolverKind::BackwardSlice: return "backward-slice";
  }
  return "none";
}

PpcValueTracker::PpcValueTracker(const xbox::XexImage& image) : image_(&image) { reset(); }

void PpcValueTracker::reset() noexcept {
  gpr_.fill(AbstractValue::unknown());
  // r1 starts as an unknown stack base but its relative displacements can be
  // tracked without pretending the host knows the actual runtime stack VA.
  gpr_[1] = AbstractValue::stack_relative(0);
  ctr_ = AbstractValue::unknown();
  lr_ = AbstractValue::unknown();
}

const AbstractValue& PpcValueTracker::gpr(std::uint32_t index) const noexcept {
  static const AbstractValue kUnknown{};
  return index < gpr_.size() ? gpr_[index] : kUnknown;
}

void PpcValueTracker::step(const cpu::DecodedInstruction& instruction) {
  if (!instruction.valid() || image_ == nullptr) {
    reset();
    return;
  }
  const auto mnemonic = instruction.mnemonic();

  // Architecturally, every linked branch writes LR with the next PC. A
  // completed call also invalidates caller-clobbered GPR facts and CTR;
  // otherwise a constant built before a call could be unsafely reused after
  // an arbitrary callee changed it. Nonvolatile r14-r31 survive.
  if (instruction.info->group == cpu::InstructionGroup::Branch && instruction.lk()) {
    lr_ = AbstractValue::address(instruction.address + 4u);
    ctr_ = AbstractValue::unknown();
    for (std::uint32_t reg = 0u; reg < gpr_.size(); ++reg)
      if (call_clobbers_gpr(reg)) gpr_[reg] = AbstractValue::unknown();
  }

  if (mnemonic_is(mnemonic, {"addi", "addis"})) {
    const auto source = instruction.ra() == 0u ? AbstractValue::constant(0u) : gpr_[instruction.ra()];
    const auto delta = mnemonic == "addis"
        ? (static_cast<std::int64_t>(instruction.simm16()) << 16)
        : static_cast<std::int64_t>(instruction.simm16());
    gpr_[instruction.rt()] = add_value(*image_, source, delta);
    return;
  }
  if (mnemonic_is(mnemonic, {"ori", "oris", "xori", "xoris"})) {
    const auto exact = gpr_[instruction.rs()].exact_u32();
    if (!exact) {
      gpr_[instruction.ra()] = AbstractValue::unknown();
      return;
    }
    const auto imm = mnemonic.ends_with("is")
        ? (static_cast<std::uint32_t>(instruction.uimm16()) << 16u)
        : static_cast<std::uint32_t>(instruction.uimm16());
    const auto result = mnemonic.starts_with("xor") ? (*exact ^ imm) : (*exact | imm);
    gpr_[instruction.ra()] = classify_exact(*image_, result);
    return;
  }
  if (mnemonic_is(mnemonic, {"orx", "andx", "xorx"})) {
    if (mnemonic == "orx")
      gpr_[instruction.ra()] = exact_binary(*image_, gpr_[instruction.rs()], gpr_[instruction.rb()],
                                            [](std::uint32_t a, std::uint32_t b) { return a | b; });
    else if (mnemonic == "andx")
      gpr_[instruction.ra()] = exact_binary(*image_, gpr_[instruction.rs()], gpr_[instruction.rb()],
                                            [](std::uint32_t a, std::uint32_t b) { return a & b; });
    else
      gpr_[instruction.ra()] = exact_binary(*image_, gpr_[instruction.rs()], gpr_[instruction.rb()],
                                            [](std::uint32_t a, std::uint32_t b) { return a ^ b; });
    return;
  }
  if (mnemonic_is(mnemonic, {"addx", "subfx"})) {
    if (mnemonic == "addx")
      gpr_[instruction.rt()] = exact_binary(*image_, gpr_[instruction.ra()], gpr_[instruction.rb()],
                                            [](std::uint32_t a, std::uint32_t b) { return a + b; });
    else
      gpr_[instruction.rt()] = exact_binary(*image_, gpr_[instruction.ra()], gpr_[instruction.rb()],
                                            [](std::uint32_t a, std::uint32_t b) { return b - a; });
    return;
  }
  if (mnemonic == "mtspr") {
    if (instruction.spr() == 9u) ctr_ = gpr_[instruction.rs()];
    else if (instruction.spr() == 8u) lr_ = gpr_[instruction.rs()];
    return;
  }
  if (mnemonic == "mfspr") {
    if (instruction.spr() == 9u) gpr_[instruction.rt()] = ctr_;
    else if (instruction.spr() == 8u) gpr_[instruction.rt()] = lr_;
    else gpr_[instruction.rt()] = AbstractValue::unknown();
    return;
  }
  if (instruction.info->group == cpu::InstructionGroup::Memory && is_load(mnemonic)) {
    AbstractValue ea = AbstractValue::unknown();
    if (instruction.info->format == cpu::InstructionFormat::D)
      ea = effective_address_d(*image_, gpr_, instruction);
    else if (instruction.info->format == cpu::InstructionFormat::DS)
      ea = effective_address_ds(*image_, gpr_, instruction);
    else if (instruction.info->format == cpu::InstructionFormat::X)
      ea = effective_address_x(*image_, gpr_, instruction);
    gpr_[instruction.rt()] = load_value(*image_, ea, mnemonic);
    if (update_form(mnemonic)) gpr_[instruction.ra()] = ea;
    return;
  }
  if (instruction.info->group == cpu::InstructionGroup::Memory && is_store(mnemonic)) {
    if (update_form(mnemonic)) {
      AbstractValue ea = AbstractValue::unknown();
      if (instruction.info->format == cpu::InstructionFormat::D)
        ea = effective_address_d(*image_, gpr_, instruction);
      else if (instruction.info->format == cpu::InstructionFormat::DS)
        ea = effective_address_ds(*image_, gpr_, instruction);
      else if (instruction.info->format == cpu::InstructionFormat::X)
        ea = effective_address_x(*image_, gpr_, instruction);
      gpr_[instruction.ra()] = ea;
    }
    return;
  }

  // Compares and branches do not clobber GPR facts. This is the key
  // difference from the old all-or-nothing tracker and lets constants survive
  // harmless scheduling between address construction and mtctr.
  if (mnemonic.starts_with("cmp") || instruction.info->group == cpu::InstructionGroup::Branch ||
      instruction.info->group == cpu::InstructionGroup::FloatingPoint ||
      instruction.info->group == cpu::InstructionGroup::Vector)
    return;

  // Conservative fallback: invalidate every GPR field this integer/control
  // instruction could plausibly write, but preserve unrelated registers.
  if (instruction.info->group == cpu::InstructionGroup::Integer) {
    gpr_[instruction.rt()] = AbstractValue::unknown();
    gpr_[instruction.ra()] = AbstractValue::unknown();
  } else if (instruction.info->group == cpu::InstructionGroup::Control) {
    gpr_[instruction.rt()] = AbstractValue::unknown();
  }
}

IndirectResolution resolve_indirect_flow(const cpu::DecodedInstruction& branch,
                                         const PpcValueTracker& tracker,
                                         std::span<const cpu::DecodedInstruction> history,
                                         const xbox::XexImage& image,
                                         std::size_t max_slice_instructions,
                                         std::size_t max_recursion_depth) {
  IndirectResolution result{};
  const bool ctr_branch = branch.mnemonic() == "bcctrx";
  const bool lr_branch = branch.mnemonic() == "bclrx";
  if (!ctr_branch && !lr_branch) return result;

  const auto& fast = ctr_branch ? tracker.ctr() : tracker.lr();
  result.targets = valid_executable_targets(image, fast);
  if (!result.targets.empty()) {
    if (fast.kind == AbstractValueKind::LoadedFromReadOnlyTable) {
      result.kind = IndirectResolverKind::ReadOnlyTable;
      result.table_address = fast.loaded_from;
    } else {
      result.kind = IndirectResolverKind::FastValue;
    }
    return result;
  }

  BackwardSlicer slicer(history, image, max_slice_instructions, max_recursion_depth);
  const auto sliced = slicer.resolve_special(ctr_branch ? 9u : 8u);
  result.instructions_examined = slicer.examined();
  result.targets = valid_executable_targets(image, sliced);
  if (result.targets.empty()) return result;
  if (sliced.kind == AbstractValueKind::LoadedFromReadOnlyTable) {
    result.kind = IndirectResolverKind::ReadOnlyTable;
    result.table_address = sliced.loaded_from;
  } else {
    result.kind = IndirectResolverKind::BackwardSlice;
  }
  return result;
}

}  // namespace xenon::recomp::value_analysis

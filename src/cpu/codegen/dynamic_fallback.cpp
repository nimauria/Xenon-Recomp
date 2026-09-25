#include "xenon/cpu/dynamic_fallback.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_map>

#include "xenon/cpu/aot_semantics.hpp"

namespace xenon::cpu {
namespace {

constexpr std::uint32_t kGuestPageSize = 0x1000u;
constexpr GuestAddress kGuestPageMask = ~(kGuestPageSize - 1u);
constexpr std::uint32_t kFallbackUnsupportedDetail = 0x58470001u;  // "XG" Gen7
constexpr std::uint32_t kFallbackInvalidTargetDetail = 0x58470002u;
constexpr std::uint32_t kFallbackSourceChangedDetail = 0x58470003u;
constexpr std::uint32_t kFallbackInstructionLimitDetail = 0x58470004u;
constexpr std::uint32_t kFallbackCallDepthDetail = 0x58470005u;
constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

[[nodiscard]] std::uint64_t fingerprint_step(std::uint64_t hash,
                                             std::uint32_t word) noexcept {
  // Keep the first runtime fingerprint address-independent. Gen 9 can add
  // deeper normalization of PC-relative immediates; even this form already
  // survives a block moving wholesale between executable revisions.
  hash ^= word;
  hash *= kFnvPrime;
  return hash;
}

[[nodiscard]] bool branch_condition(const DecodedInstruction& i,
                                    CpuState& state, bool use_ctr) noexcept {
  const auto bo = i.bo();
  bool ctr_ok = true;
  bool cond_ok = true;
  if (use_ctr && (bo & 0b00100u) == 0u) {
    state.ctr -= 1u;
    const bool want_zero = (bo & 0b00010u) != 0u;
    ctr_ok = want_zero ? state.ctr == 0u : state.ctr != 0u;
  }
  if ((bo & 0b10000u) == 0u) {
    const bool want_true = (bo & 0b01000u) != 0u;
    cond_ok = state.cr_bit(i.bi()) == want_true;
  }
  return ctr_ok && cond_ok;
}

void write_compare(CpuState& state, unsigned field, std::uint64_t lhs,
                   std::uint64_t rhs, bool logical, bool word) noexcept {
  bool lt = false;
  bool gt = false;
  if (word) {
    if (logical) {
      const auto a = static_cast<std::uint32_t>(lhs);
      const auto b = static_cast<std::uint32_t>(rhs);
      lt = a < b;
      gt = a > b;
    } else {
      const auto a = static_cast<std::int32_t>(lhs);
      const auto b = static_cast<std::int32_t>(rhs);
      lt = a < b;
      gt = a > b;
    }
  } else if (logical) {
    lt = lhs < rhs;
    gt = lhs > rhs;
  } else {
    const auto a = static_cast<std::int64_t>(lhs);
    const auto b = static_cast<std::int64_t>(rhs);
    lt = a < b;
    gt = a > b;
  }
  const std::uint8_t nibble = static_cast<std::uint8_t>(
      (lt ? 0x8u : gt ? 0x4u : 0x2u) | (state.xer_so() ? 0x1u : 0u));
  state.set_cr_field(field, nibble);
}

[[nodiscard]] std::uint32_t mask32(unsigned mb, unsigned me) noexcept {
  mb &= 31u;
  me &= 31u;
  std::uint32_t mask = 0u;
  const auto set_arch_bit = [&mask](unsigned bit) {
    mask |= std::uint32_t{1} << (31u - bit);
  };
  if (mb <= me) {
    for (unsigned bit = mb; bit <= me; ++bit) set_arch_bit(bit);
  } else {
    for (unsigned bit = mb; bit < 32u; ++bit) set_arch_bit(bit);
    for (unsigned bit = 0u; bit <= me; ++bit) set_arch_bit(bit);
  }
  return mask;
}

struct ScalarAccess {
  unsigned bytes{};
  bool sign{};
  bool update{};
  bool indexed{};
  bool little_endian{};
};

[[nodiscard]] std::optional<ScalarAccess> load_access(std::string_view m) {
#define X(N,B,S,U,I) if (m == N) return ScalarAccess{B,S,U,I,false}
  X("lbz",1,false,false,false); X("lbzu",1,false,true,false);
  X("lbzx",1,false,false,true); X("lbzux",1,false,true,true);
  X("lha",2,true,false,false); X("lhau",2,true,true,false);
  X("lhax",2,true,false,true); X("lhaux",2,true,true,true);
  X("lhz",2,false,false,false); X("lhzu",2,false,true,false);
  X("lhzx",2,false,false,true); X("lhzux",2,false,true,true);
  X("lwa",4,true,false,false); X("lwax",4,true,false,true);
  X("lwaux",4,true,true,true); X("lwz",4,false,false,false);
  X("lwzu",4,false,true,false); X("lwzx",4,false,false,true);
  X("lwzux",4,false,true,true); X("ld",8,false,false,false);
  X("ldu",8,false,true,false); X("ldx",8,false,false,true);
  X("ldux",8,false,true,true);
#undef X
  if (m == "lhbrx") return ScalarAccess{2,false,false,true,true};
  if (m == "lwbrx") return ScalarAccess{4,false,false,true,true};
  if (m == "ldbrx") return ScalarAccess{8,false,false,true,true};
  return std::nullopt;
}

[[nodiscard]] std::optional<ScalarAccess> store_access(std::string_view m) {
#define X(N,B,U,I) if (m == N) return ScalarAccess{B,false,U,I,false}
  X("stb",1,false,false); X("stbu",1,true,false);
  X("stbx",1,false,true); X("stbux",1,true,true);
  X("sth",2,false,false); X("sthu",2,true,false);
  X("sthx",2,false,true); X("sthux",2,true,true);
  X("stw",4,false,false); X("stwu",4,true,false);
  X("stwx",4,false,true); X("stwux",4,true,true);
  X("std",8,false,false); X("stdu",8,true,false);
  X("stdx",8,false,true); X("stdux",8,true,true);
#undef X
  if (m == "sthbrx") return ScalarAccess{2,false,false,true,true};
  if (m == "stwbrx") return ScalarAccess{4,false,false,true,true};
  if (m == "stdbrx") return ScalarAccess{8,false,false,true,true};
  return std::nullopt;
}

[[nodiscard]] GuestAddress effective_address(const DecodedInstruction& i,
                                             const CpuState& state,
                                             const ScalarAccess& access) noexcept {
  std::uint64_t base = 0u;
  if (access.indexed) {
    if (access.update || i.ra() != 0u) base = state.gpr[i.ra()];
    return static_cast<GuestAddress>(base + state.gpr[i.rb()]);
  }
  if (access.update || i.ra() != 0u) base = state.gpr[i.ra()];
  const auto displacement = i.info && i.info->format == InstructionFormat::DS
                                ? static_cast<std::int64_t>(i.ds_displacement())
                                : static_cast<std::int64_t>(i.simm16());
  return static_cast<GuestAddress>(base + static_cast<std::uint64_t>(displacement));
}

[[nodiscard]] std::uint64_t load_scalar(MemoryAccessContext& memory,
                                        GuestAddress address,
                                        const ScalarAccess& access) {
  std::uint64_t value = 0u;
  switch (access.bytes) {
    case 1: value = memory.read8(address); break;
    case 2: value = access.little_endian ? memory.read16_le(address)
                                         : memory.read16_be(address); break;
    case 4: value = access.little_endian ? memory.read32_le(address)
                                         : memory.read32_be(address); break;
    case 8: value = access.little_endian ? memory.read64_le(address)
                                         : memory.read64_be(address); break;
    default: break;
  }
  if (!access.sign) return value;
  if (access.bytes == 2u)
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(
        static_cast<std::int16_t>(value)));
  if (access.bytes == 4u)
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(
        static_cast<std::int32_t>(value)));
  return value;
}

void store_scalar(MemoryAccessContext& memory, GuestAddress address,
                  const ScalarAccess& access, std::uint64_t value) {
  switch (access.bytes) {
    case 1: memory.write8(address, static_cast<std::uint8_t>(value)); break;
    case 2:
      if (access.little_endian) memory.write16_le(address, static_cast<std::uint16_t>(value));
      else memory.write16_be(address, static_cast<std::uint16_t>(value));
      break;
    case 4:
      if (access.little_endian) memory.write32_le(address, static_cast<std::uint32_t>(value));
      else memory.write32_be(address, static_cast<std::uint32_t>(value));
      break;
    case 8:
      if (access.little_endian) memory.write64_le(address, value);
      else memory.write64_be(address, value);
      break;
    default: break;
  }
}

[[nodiscard]] bool execute_simple(const DecodedInstruction& i,
                                  ExecutionContext& context) {
  auto& state = context.state;
  auto& mem = context.memory_access;
  const auto m = i.mnemonic();

  if (const auto access = load_access(m)) {
    const auto ea = effective_address(i, state, *access);
    state.gpr[i.rt()] = load_scalar(mem, ea, *access);
    if (access->update) state.gpr[i.ra()] = ea;
    return true;
  }
  if (const auto access = store_access(m)) {
    const auto ea = effective_address(i, state, *access);
    store_scalar(mem, ea, *access, state.gpr[i.rs()]);
    if (access->update) state.gpr[i.ra()] = ea;
    return true;
  }

  if (m == "addi" || m == "addis") {
    const auto base = i.ra() ? state.gpr[i.ra()] : 0u;
    std::int64_t imm = i.simm16();
    if (m == "addis") imm <<= 16;
    state.gpr[i.rt()] = base + static_cast<std::uint64_t>(imm);
    return true;
  }
  if (m == "ori" || m == "oris" || m == "xori" || m == "xoris" ||
      m == "andix" || m == "andisx") {
    std::uint64_t imm = i.uimm16();
    if (m == "oris" || m == "xoris" || m == "andisx") imm <<= 16u;
    const auto src = state.gpr[i.rs()];
    std::uint64_t value = 0u;
    if (m == "ori" || m == "oris") value = src | imm;
    else if (m == "xori" || m == "xoris") value = src ^ imm;
    else value = src & imm;
    state.gpr[i.ra()] = value;
    if (m == "andix" || m == "andisx") state.update_cr0_signed(value);
    return true;
  }
  if (m == "addx" || m == "subfx") {
    const auto a = state.gpr[i.ra()];
    const auto b = state.gpr[i.rb()];
    const auto value = m == "addx" ? a + b : b - a;
    state.gpr[i.rt()] = value;
    if (i.rc()) state.update_cr0_signed(value);
    return true;
  }
  if (m == "andx" || m == "andcx" || m == "orx" || m == "orcx" ||
      m == "xorx" || m == "eqvx" || m == "nandx" || m == "norx") {
    const auto a = state.gpr[i.rs()];
    const auto b = state.gpr[i.rb()];
    std::uint64_t value = 0u;
    if (m == "andx") value = a & b;
    else if (m == "andcx") value = a & ~b;
    else if (m == "orx") value = a | b;
    else if (m == "orcx") value = a | ~b;
    else if (m == "xorx") value = a ^ b;
    else if (m == "eqvx") value = ~(a ^ b);
    else if (m == "nandx") value = ~(a & b);
    else value = ~(a | b);
    state.gpr[i.ra()] = value;
    if (i.rc()) state.update_cr0_signed(value);
    return true;
  }
  if (m == "negx") {
    const auto value = std::uint64_t{0} - state.gpr[i.ra()];
    state.gpr[i.rt()] = value;
    if (i.rc()) state.update_cr0_signed(value);
    return true;
  }
  if (m == "extsbx" || m == "extshx" || m == "extswx") {
    const auto src = state.gpr[i.rs()];
    std::uint64_t value = 0u;
    if (m == "extsbx") value = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int8_t>(src)));
    else if (m == "extshx") value = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int16_t>(src)));
    else value = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(src)));
    state.gpr[i.ra()] = value;
    if (i.rc()) state.update_cr0_signed(value);
    return true;
  }
  if (m == "cmp" || m == "cmpl" || m == "cmpi" || m == "cmpli") {
    const bool logical = m == "cmpl" || m == "cmpli";
    const bool immediate = m == "cmpi" || m == "cmpli";
    const bool word = ((i.word >> 21u) & 1u) == 0u;
    const auto rhs = immediate
                         ? (logical ? std::uint64_t{i.uimm16()}
                                    : static_cast<std::uint64_t>(static_cast<std::int64_t>(i.simm16())))
                         : state.gpr[i.rb()];
    write_compare(state, i.crfd(), state.gpr[i.ra()], rhs, logical, word);
    return true;
  }
  if (m == "rlwinmx" || m == "rlwimix" || m == "rlwnmx") {
    const auto src = static_cast<std::uint32_t>(state.gpr[i.rs()]);
    const auto shift = m == "rlwnmx" ? static_cast<unsigned>(state.gpr[i.rb()] & 31u)
                                      : i.sh32();
    const auto rotated = std::rotl(src, static_cast<int>(shift));
    const auto mask = mask32(i.mb32(), i.me32());
    auto result = rotated & mask;
    if (m == "rlwimix")
      result = (static_cast<std::uint32_t>(state.gpr[i.ra()]) & ~mask) | result;
    state.gpr[i.ra()] = result;
    if (i.rc()) state.update_cr0_signed(result);
    return true;
  }
  if (m == "slwx" || m == "srwx" || m == "srawx" || m == "sldx" ||
      m == "srdx" || m == "sradx") {
    const auto value = aot::ppc_shift(m, state.gpr[i.rs()], state.gpr[i.rb()]);
    state.gpr[i.ra()] = value;
    if (m == "srawx" || m == "sradx")
      state.set_xer_ca(aot::ppc_shift_carry(m, state.gpr[i.rs()], state.gpr[i.rb()]));
    if (i.rc()) state.update_cr0_signed(value);
    return true;
  }
  if (m == "srawix" || m == "sradix") {
    const auto shift = m == "srawix" ? i.sh32() : i.sh64_md();
    const auto value = aot::ppc_shift(m, state.gpr[i.rs()], shift);
    state.gpr[i.ra()] = value;
    state.set_xer_ca(aot::ppc_shift_carry(m, state.gpr[i.rs()], shift));
    if (i.rc()) state.update_cr0_signed(value);
    return true;
  }
  if (m == "cntlzwx" || m == "cntlzdx") {
    const auto src = state.gpr[i.rs()];
    const auto value = m == "cntlzwx"
                           ? static_cast<std::uint64_t>(
                                 std::countl_zero(static_cast<std::uint32_t>(src)))
                           : static_cast<std::uint64_t>(std::countl_zero(src));
    state.gpr[i.ra()] = value;
    if (i.rc()) state.update_cr0_signed(value);
    return true;
  }
  if (m == "crand" || m == "crandc" || m == "creqv" || m == "crnand" ||
      m == "crnor" || m == "cror" || m == "crorc" || m == "crxor") {
    const auto bt = (i.word >> 21u) & 31u;
    const auto ba = (i.word >> 16u) & 31u;
    const auto bb = (i.word >> 11u) & 31u;
    const bool a = state.cr_bit(ba);
    const bool b = state.cr_bit(bb);
    bool result = false;
    if (m == "crand") result = a && b;
    else if (m == "crandc") result = a && !b;
    else if (m == "creqv") result = a == b;
    else if (m == "crnand") result = !(a && b);
    else if (m == "crnor") result = !(a || b);
    else if (m == "cror") result = a || b;
    else if (m == "crorc") result = a || !b;
    else result = a != b;
    state.set_cr_bit(bt, result);
    return true;
  }
  if (m == "mcrf") {
    const auto bf = (i.word >> 23u) & 7u;
    const auto bfa = (i.word >> 18u) & 7u;
    state.set_cr_field(bf, state.cr_field(bfa));
    return true;
  }
  if (m == "mcrxr") {
    const auto field = static_cast<std::uint8_t>(
        (state.xer_so() ? 8u : 0u) | (state.xer_ov() ? 4u : 0u) |
        (state.xer_ca() ? 2u : 0u));
    state.set_cr_field(i.crfd(), field);
    state.xer &= ~(xer_bits::SO | xer_bits::OV | xer_bits::CA);
    return true;
  }
  if (m == "mfcr") {
    state.gpr[i.rt()] = state.cr;
    return true;
  }
  if (m == "mtcrf") {
    const auto fxm = (i.word >> 12u) & 0xFFu;
    const auto source = static_cast<std::uint32_t>(state.gpr[i.rs()]);
    for (unsigned field = 0; field < 8u; ++field) {
      if ((fxm & (0x80u >> field)) == 0u) continue;
      const auto shift = (7u - field) * 4u;
      state.cr = (state.cr & ~(0xFu << shift)) | (source & (0xFu << shift));
    }
    return true;
  }
  if (m == "mfspr" || m == "mftb") {
    const auto spr = i.spr();
    std::uint64_t value = 0u;
    if (spr == 1u) value = state.xer;
    else if (spr == 8u) value = state.lr;
    else if (spr == 9u) value = state.ctr;
    else if (spr == 256u) value = state.vrsave;
    else if (spr == 268u) value = context.runtime.read_time_base(state);
    else if (spr == 269u) value = context.runtime.read_time_base(state) >> 32u;
    else if (spr == 287u) value = state.pvr;
    else value = context.runtime.read_spr(spr, state);
    state.gpr[i.rt()] = value;
    return true;
  }
  if (m == "mtspr") {
    const auto spr = i.spr();
    const auto value = state.gpr[i.rs()];
    if (spr == 1u) state.xer = static_cast<std::uint32_t>(value);
    else if (spr == 8u) state.lr = value;
    else if (spr == 9u) state.ctr = value;
    else if (spr == 256u) state.vrsave = static_cast<std::uint32_t>(value);
    else context.runtime.write_spr(spr, value, state);
    return true;
  }
  if (m == "sync") {
    mem.barrier(BarrierKind::Sync);
    return true;
  }
  if (m == "eieio") {
    mem.barrier(BarrierKind::Eieio);
    return true;
  }
  if (m == "isync") {
    mem.barrier(BarrierKind::InstructionSync);
    return true;
  }
  if (m == "dcbz" || m == "dcbz128") {
    const auto ea = static_cast<GuestAddress>((i.ra() ? state.gpr[i.ra()] : 0u) + state.gpr[i.rb()]);
    mem.zero_cache_block(ea, m == "dcbz128" ? 128u : 32u);
    return true;
  }
  if (m == "icbi") {
    const auto ea = static_cast<GuestAddress>((i.ra() ? state.gpr[i.ra()] : 0u) + state.gpr[i.rb()]);
    mem.instruction_cache_invalidate(ea);
    return true;
  }
  // Cache hints have no architecturally visible data result in this fallback.
  if (m == "dcbf" || m == "dcbi" || m == "dcbst" || m == "dcbt" || m == "dcbtst")
    return true;

  return false;
}

}  // namespace

DynamicFallbackExecutor::DynamicFallbackExecutor(DynamicFallbackConfig config,
                                                 ObservationCallback observer)
    : config_(config), observer_(std::move(observer)) {
  config_.max_instructions_per_dispatch =
      std::max<std::uint32_t>(1u, config_.max_instructions_per_dispatch);
  config_.max_nested_calls = std::max<std::uint32_t>(1u, config_.max_nested_calls);
}

void DynamicFallbackExecutor::bind(ExecutionContext& context) noexcept {
  context.dynamic_fallback_executor = this;
  context.dynamic_fallback = &DynamicFallbackExecutor::callback;
}

DynamicFallbackResult DynamicFallbackExecutor::callback(
    void* executor, ExecutionContext& context, GuestAddress target,
    CompiledLookupKind kind) {
  if (!executor) return {};
  return static_cast<DynamicFallbackExecutor*>(executor)->try_execute(
      context, target, kind);
}

DynamicFallbackResult DynamicFallbackExecutor::try_execute(
    ExecutionContext& context, GuestAddress target, CompiledLookupKind kind) {
  const auto site = context.state.cia;
  auto run_result = run(context, target, kind, 0u);
  if (run_result.public_result.handled) {
    executed_blocks_.fetch_add(1u, std::memory_order_relaxed);
    executed_instructions_.fetch_add(run_result.instructions,
                                     std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(pc_hits_mutex_);
      ++pc_hits_[target];
    }
    if (observer_) {
      observer_(DynamicFallbackObservation{
          target, site, run_result.exit, kind, run_result.reason,
          run_result.instructions, run_result.fingerprint});
    }
  }
  return run_result.public_result;
}

std::size_t DynamicFallbackExecutor::fallback_unique_pc_count() const {
  std::lock_guard<std::mutex> lock(pc_hits_mutex_);
  return pc_hits_.size();
}

std::vector<std::pair<GuestAddress, std::uint64_t>>
DynamicFallbackExecutor::fallback_hot_pcs(std::size_t top_n) const {
  std::vector<std::pair<GuestAddress, std::uint64_t>> hits;
  {
    std::lock_guard<std::mutex> lock(pc_hits_mutex_);
    hits.reserve(pc_hits_.size());
    for (const auto& [pc, count] : pc_hits_) hits.emplace_back(pc, count);
  }
  std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) {
    if (a.second != b.second) return a.second > b.second;
    return a.first < b.first;  // Deterministic tie-break for reproducible reports.
  });
  if (hits.size() > top_n) hits.resize(top_n);
  return hits;
}

DynamicFallbackExecutor::RunResult DynamicFallbackExecutor::run(
    ExecutionContext& context, GuestAddress target, CompiledLookupKind kind,
    std::uint32_t depth) {
  RunResult out{};
  out.exit = target;
  out.fingerprint = kFnvOffset;

  if ((target & 3u) != 0u) {
    out.reason = DynamicFallbackStopReason::InvalidTarget;
    return out;
  }

  const auto first_stamp = context.memory.executable_page_stamp(target);
  if (!first_stamp.executable()) return out;  // Not ours; imports may handle it.

  out.public_result.handled = true;
  if (depth > config_.max_nested_calls) {
    out.reason = DynamicFallbackStopReason::CallDepthLimit;
    out.public_result.result = {FlowReason::Trap, target, kFallbackCallDepthDetail};
    return out;
  }
  const auto entry_return = static_cast<GuestAddress>(context.state.lr & ~3ull);
  std::unordered_map<GuestAddress, ExecutablePageStamp> page_stamps;
  Decoder decoder;
  GuestAddress pc = target;

  const auto trap_result = [&](DynamicFallbackStopReason reason,
                               std::uint32_t detail) {
    out.reason = reason;
    out.exit = pc;
    out.public_result.result = {FlowReason::Trap, pc, detail};
  };

  const auto validate_page = [&](GuestAddress address) -> bool {
    const auto page = address & kGuestPageMask;
    const auto current = context.memory.executable_page_stamp(address);
    if (!current.executable()) return false;
    const auto [it, inserted] = page_stamps.emplace(page, current);
    if (!inserted && it->second != current) {
      source_invalidations_.fetch_add(1u, std::memory_order_relaxed);
      trap_result(DynamicFallbackStopReason::SourceChanged,
                  kFallbackSourceChangedDetail);
      return false;
    }
    return true;
  };

  const auto finish_call = [&](const ExecutionResult& result,
                               GuestAddress expected_return,
                               GuestAddress& next_pc) -> std::optional<ExecutionResult> {
    if (result.reason == FlowReason::Return) {
      if (result.next_address == expected_return) {
        next_pc = expected_return;
        return std::nullopt;
      }
      return result;
    }
    if (result.reason == FlowReason::Fallthrough) {
      next_pc = expected_return;
      return std::nullopt;
    }
    return result;
  };

  const auto execute_call = [&](GuestAddress call_target,
                                GuestAddress expected_return,
                                GuestAddress& next_pc) -> std::optional<ExecutionResult> {
    if (auto* native = context.lookup_compiled(call_target, CompiledLookupKind::Call)) {
      const auto result = native(context);
      return finish_call(result, expected_return, next_pc);
    }
    if (context.memory.executable_page_stamp(call_target).executable()) {
      auto nested = run(context, call_target, CompiledLookupKind::Call, depth + 1u);
      out.instructions += nested.instructions;
      out.fingerprint ^= nested.fingerprint + 0x9E3779B97F4A7C15ull +
                         (out.fingerprint << 6u) + (out.fingerprint >> 2u);
      if (nested.public_result.handled) {
        // Nested dynamically discovered callees are first-class learned facts,
        // not hidden inside the outer observation. Instruction totals remain
        // accounted once by the outer dispatch; block counts/observations are
        // emitted here for each nested entry.
        executed_blocks_.fetch_add(1u, std::memory_order_relaxed);
        if (observer_) {
          observer_(DynamicFallbackObservation{
              call_target, pc, nested.exit, CompiledLookupKind::Call,
              nested.reason, nested.instructions, nested.fingerprint});
        }
        return finish_call(nested.public_result.result, expected_return, next_pc);
      }
    }
    const auto result = context.runtime.call(call_target, context.state, context.memory);
    return finish_call(result, expected_return, next_pc);
  };

  for (std::uint32_t local_count = 0u;
       local_count < config_.max_instructions_per_dispatch; ++local_count) {
    if (!validate_page(pc)) {
      if (out.reason != DynamicFallbackStopReason::SourceChanged)
        trap_result(DynamicFallbackStopReason::InvalidTarget,
                    kFallbackInvalidTargetDetail);
      return out;
    }

    context.state.cia = pc;
    context.state.nia = pc + 4u;
    const auto word = context.memory.fetch32_be(pc);
    // Revalidate after the fetch as well. A host thread may rewrite/remap the
    // executable page between the pre-fetch stamp check and the actual read;
    // observations must never bless a mixed-generation block.
    if (!validate_page(pc)) {
      if (out.reason != DynamicFallbackStopReason::SourceChanged)
        trap_result(DynamicFallbackStopReason::InvalidTarget,
                    kFallbackInvalidTargetDetail);
      return out;
    }
    out.fingerprint = fingerprint_step(out.fingerprint, word);
    ++out.instructions;
    const auto insn = decoder.decode(pc, word);
    if (!insn.valid()) {
      unsupported_instructions_.fetch_add(1u, std::memory_order_relaxed);
      trap_result(DynamicFallbackStopReason::UnsupportedInstruction,
                  kFallbackUnsupportedDetail);
      return out;
    }

    const auto mnemonic = insn.mnemonic();
    GuestAddress next_pc = pc + 4u;

    if (mnemonic == "bx") {
      const auto branch_target = insn.direct_branch_target() & ~3u;
      if (insn.lk()) {
        context.state.lr = pc + 4u;
        if (const auto escaped = execute_call(branch_target, pc + 4u, next_pc)) {
          out.reason = escaped->reason == FlowReason::Trap
                           ? DynamicFallbackStopReason::Trap
                           : DynamicFallbackStopReason::CompiledHandoff;
          out.exit = escaped->next_address;
          out.public_result.result = *escaped;
          return out;
        }
      } else {
        if (auto* native = context.lookup_compiled(branch_target,
                                                   CompiledLookupKind::Branch)) {
          out.reason = DynamicFallbackStopReason::CompiledHandoff;
          out.exit = branch_target;
          out.public_result.result = native(context);
          return out;
        }
        next_pc = branch_target;
      }
    } else if (mnemonic == "bcx") {
      const bool taken = branch_condition(insn, context.state, true);
      if (insn.lk()) context.state.lr = pc + 4u;
      if (taken) {
        const auto branch_target = insn.direct_branch_target() & ~3u;
        if (insn.lk()) {
          if (const auto escaped = execute_call(branch_target, pc + 4u, next_pc)) {
            out.reason = escaped->reason == FlowReason::Trap
                             ? DynamicFallbackStopReason::Trap
                             : DynamicFallbackStopReason::CompiledHandoff;
            out.exit = escaped->next_address;
            out.public_result.result = *escaped;
            return out;
          }
        } else {
          if (auto* native = context.lookup_compiled(branch_target,
                                                     CompiledLookupKind::Branch)) {
            out.reason = DynamicFallbackStopReason::CompiledHandoff;
            out.exit = branch_target;
            out.public_result.result = native(context);
            return out;
          }
          next_pc = branch_target;
        }
      }
    } else if (mnemonic == "bclrx" || mnemonic == "bcctrx") {
      const bool use_ctr = mnemonic == "bclrx";
      const auto raw_target = mnemonic == "bclrx" ? context.state.lr : context.state.ctr;
      const auto branch_target = static_cast<GuestAddress>(raw_target & ~3ull);
      const bool taken = branch_condition(insn, context.state, use_ctr);
      if (insn.lk()) context.state.lr = pc + 4u;
      if (taken) {
        if (insn.lk()) {
          if (const auto escaped = execute_call(branch_target, pc + 4u, next_pc)) {
            out.reason = escaped->reason == FlowReason::Trap
                             ? DynamicFallbackStopReason::Trap
                             : DynamicFallbackStopReason::CompiledHandoff;
            out.exit = escaped->next_address;
            out.public_result.result = *escaped;
            return out;
          }
        } else if (mnemonic == "bclrx" && kind == CompiledLookupKind::Call &&
                   branch_target == entry_return) {
          // NIA is architecturally the taken return target. Gen 8 differential
          // verification caught the fallback returning the correct
          // ExecutionResult while leaving CpuState::nia at pc+4.
          context.state.nia = branch_target;
          out.reason = DynamicFallbackStopReason::Returned;
          out.exit = branch_target;
          out.public_result.result = {FlowReason::Return, branch_target, 0u};
          return out;
        } else {
          if (auto* native = context.lookup_compiled(branch_target,
                                                     CompiledLookupKind::Branch)) {
            out.reason = DynamicFallbackStopReason::CompiledHandoff;
            out.exit = branch_target;
            out.public_result.result = native(context);
            return out;
          }
          next_pc = branch_target;
        }
      }
    } else if (mnemonic == "sc") {
      const auto result = context.runtime.syscall((insn.word >> 5u) & 0x7Fu,
                                                  context.state, context.memory);
      out.reason = DynamicFallbackStopReason::Syscall;
      out.exit = result.next_address;
      out.public_result.result = result;
      return out;
    } else if (mnemonic == "td" || mnemonic == "tdi" || mnemonic == "tw" ||
               mnemonic == "twi") {
      const auto to = (insn.word >> 21u) & 31u;
      const auto ra = (insn.word >> 16u) & 31u;
      const auto rb = (insn.word >> 11u) & 31u;
      const bool word_form = mnemonic == "tw" || mnemonic == "twi";
      const bool immediate = mnemonic == "tdi" || mnemonic == "twi";
      const auto rhs = immediate
                           ? static_cast<std::uint64_t>(static_cast<std::int64_t>(
                                 static_cast<std::int16_t>(insn.word & 0xFFFFu)))
                           : context.state.gpr[rb];
      if (aot::trap_condition(to, context.state.gpr[ra], rhs, word_form)) {
        const auto result = context.runtime.trap(to, context.state, context.memory);
        out.reason = DynamicFallbackStopReason::Trap;
        out.exit = result.next_address;
        out.public_result.result = result;
        return out;
      }
    } else if (!execute_simple(insn, context)) {
      unsupported_instructions_.fetch_add(1u, std::memory_order_relaxed);
      trap_result(DynamicFallbackStopReason::UnsupportedInstruction,
                  kFallbackUnsupportedDetail);
      return out;
    }

    pc = next_pc;
  }

  trap_result(DynamicFallbackStopReason::InstructionLimit,
              kFallbackInstructionLimitDetail);
  return out;
}

}  // namespace xenon::cpu

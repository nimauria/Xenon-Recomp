#include "xenon/cpu/semantic_verifier.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <typeinfo>

namespace xenon::cpu::verify {
namespace {

[[nodiscard]] std::uint64_t splitmix64(std::uint64_t value) noexcept {
  value += 0x9E3779B97F4A7C15ull;
  value = (value ^ (value >> 30u)) * 0xBF58476D1CE4E5B9ull;
  value = (value ^ (value >> 27u)) * 0x94D049BB133111EBull;
  return value ^ (value >> 31u);
}

[[nodiscard]] std::string hex64(std::uint64_t value) {
  std::ostringstream out;
  out << "0x" << std::hex << std::setw(16) << std::setfill('0') << value;
  return out.str();
}

[[nodiscard]] std::string hex32(std::uint32_t value) {
  std::ostringstream out;
  out << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
  return out.str();
}

[[nodiscard]] std::uint64_t read_scalar(MemoryPort& memory, GuestAddress address,
                                        unsigned bytes) {
  switch (bytes) {
    case 1: return memory.read8(address);
    case 2: return memory.read16_be(address);
    case 4: return memory.read32_be(address);
    case 8: return memory.read64_be(address);
    default: throw std::logic_error("reference scalar load width");
  }
}

void write_scalar(MemoryPort& memory, GuestAddress address, unsigned bytes,
                  std::uint64_t value) {
  switch (bytes) {
    case 1: memory.write8(address, static_cast<std::uint8_t>(value)); break;
    case 2: memory.write16_be(address, static_cast<std::uint16_t>(value)); break;
    case 4: memory.write32_be(address, static_cast<std::uint32_t>(value)); break;
    case 8: memory.write64_be(address, value); break;
    default: throw std::logic_error("reference scalar store width");
  }
}

[[nodiscard]] GuestAddress dform_ea(const DecodedInstruction& i,
                                    const CpuState& state) noexcept {
  const auto base = i.ra() ? state.gpr[i.ra()] : 0ull;
  return static_cast<GuestAddress>(base + static_cast<std::int64_t>(i.simm16()));
}

[[nodiscard]] GuestAddress dsform_ea(const DecodedInstruction& i,
                                     const CpuState& state) noexcept {
  const auto base = i.ra() ? state.gpr[i.ra()] : 0ull;
  return static_cast<GuestAddress>(base + static_cast<std::int64_t>(i.ds_displacement()));
}

void compare_to_cr(CpuState& state, unsigned field, std::uint64_t lhs,
                   std::uint64_t rhs, bool logical, bool word) noexcept {
  std::uint8_t nibble = 0;
  if (logical) {
    if (word) {
      const auto a = static_cast<std::uint32_t>(lhs);
      const auto b = static_cast<std::uint32_t>(rhs);
      nibble = a < b ? 0x8u : a > b ? 0x4u : 0x2u;
    } else {
      nibble = lhs < rhs ? 0x8u : lhs > rhs ? 0x4u : 0x2u;
    }
  } else if (word) {
    const auto a = static_cast<std::int32_t>(lhs);
    const auto b = static_cast<std::int32_t>(rhs);
    nibble = a < b ? 0x8u : a > b ? 0x4u : 0x2u;
  } else {
    const auto a = static_cast<std::int64_t>(lhs);
    const auto b = static_cast<std::int64_t>(rhs);
    nibble = a < b ? 0x8u : a > b ? 0x4u : 0x2u;
  }
  if (state.xer_so()) nibble |= 0x1u;
  state.set_cr_field(field, nibble);
}


[[nodiscard]] CapturedOutcome capture_reference(
    std::span<const DecodedInstruction> instructions, CpuState& state,
    MemoryPort& memory, RuntimeServices& runtime) noexcept {
  CapturedOutcome out{};
  try {
    out.result = ReferenceExecutor::execute_block(instructions, state, memory, runtime);
  } catch (const std::out_of_range& e) {
    out.exception = ExceptionClass::OutOfRange; out.exception_text = e.what();
  } catch (const std::logic_error& e) {
    out.exception = ExceptionClass::Logic; out.exception_text = e.what();
  } catch (const std::runtime_error& e) {
    out.exception = ExceptionClass::Runtime; out.exception_text = e.what();
  } catch (const std::exception& e) {
    out.exception = ExceptionClass::Standard; out.exception_text = e.what();
  } catch (...) {
    out.exception = ExceptionClass::Unknown;
  }
  return out;
}

[[nodiscard]] CapturedOutcome capture_candidate(
    CompiledEntry candidate, CpuState& state, MemoryPort& memory,
    RuntimeServices& runtime) noexcept {
  CapturedOutcome out{};
  try {
    out.result = candidate(state, memory, runtime);
  } catch (const std::out_of_range& e) {
    out.exception = ExceptionClass::OutOfRange; out.exception_text = e.what();
  } catch (const std::logic_error& e) {
    out.exception = ExceptionClass::Logic; out.exception_text = e.what();
  } catch (const std::runtime_error& e) {
    out.exception = ExceptionClass::Runtime; out.exception_text = e.what();
  } catch (const std::exception& e) {
    out.exception = ExceptionClass::Standard; out.exception_text = e.what();
  } catch (...) {
    out.exception = ExceptionClass::Unknown;
  }
  return out;
}

[[nodiscard]] bool same_result(const CapturedOutcome& expected,
                               const CapturedOutcome& actual) noexcept {
  if (expected.exception != actual.exception) return false;
  if (expected.exception != ExceptionClass::None) return true;
  return expected.result.reason == actual.result.reason &&
         expected.result.next_address == actual.result.next_address &&
         expected.result.detail == actual.result.detail;
}

[[nodiscard]] std::string first_state_difference(const CpuState& e,
                                                 const CpuState& a) {
  for (std::size_t i = 0; i < e.gpr.size(); ++i)
    if (e.gpr[i] != a.gpr[i]) return "gpr[" + std::to_string(i) + "] expected=" + hex64(e.gpr[i]) + " actual=" + hex64(a.gpr[i]);
  for (std::size_t i = 0; i < e.fpr_bits.size(); ++i)
    if (e.fpr_bits[i] != a.fpr_bits[i]) return "fpr_bits[" + std::to_string(i) + "] expected=" + hex64(e.fpr_bits[i]) + " actual=" + hex64(a.fpr_bits[i]);
  for (std::size_t i = 0; i < e.vr.size(); ++i) {
    if (e.vr[i].bytes != a.vr[i].bytes) {
      for (std::size_t b = 0; b < 16; ++b)
        if (e.vr[i].bytes[b] != a.vr[i].bytes[b])
          return "vr[" + std::to_string(i) + "].byte[" + std::to_string(b) + "] expected=" + hex32(e.vr[i].bytes[b]) + " actual=" + hex32(a.vr[i].bytes[b]);
    }
  }
#define CHECK64(F) if (e.F != a.F) return #F " expected=" + hex64(e.F) + " actual=" + hex64(a.F)
#define CHECK32(F) if (e.F != a.F) return #F " expected=" + hex32(e.F) + " actual=" + hex32(a.F)
  CHECK64(lr); CHECK64(ctr); CHECK32(cr); CHECK32(xer); CHECK32(fpscr); CHECK32(vscr);
  CHECK64(msr); CHECK32(vrsave); CHECK32(pvr); CHECK64(time_base); CHECK32(cia); CHECK32(nia);
#undef CHECK64
#undef CHECK32
  if (e.reservation.valid != a.reservation.valid) return "reservation.valid";
  if (e.reservation.width != a.reservation.width) return "reservation.width";
  if (e.reservation.address != a.reservation.address) return "reservation.address";
  if (e.reservation.observed_value != a.reservation.observed_value) return "reservation.observed_value";
  if (e.reservation.token != a.reservation.token) return "reservation.token";
  return {};
}

[[nodiscard]] std::optional<GuestAddress> first_memory_difference(
    const FlatMemory& expected, const FlatMemory& actual) noexcept {
  const auto n = std::min(expected.data().size(), actual.data().size());
  for (std::size_t i = 0; i < n; ++i)
    if (expected.data()[i] != actual.data()[i])
      return static_cast<GuestAddress>(expected.base() + i);
  if (expected.data().size() != actual.data().size()) return expected.base();
  return std::nullopt;
}

struct TrialResult {
  bool mismatch{};
  CpuState expected_state{};
  CpuState actual_state{};
  FlatMemory expected_memory{1};
  FlatMemory actual_memory{1};
  CapturedOutcome expected{};
  CapturedOutcome actual{};
  std::string state_difference{};
  std::optional<GuestAddress> memory_difference{};

  TrialResult(GuestAddress base, std::size_t size)
      : expected_memory(size, base), actual_memory(size, base) {}
};

[[nodiscard]] TrialResult execute_trial(const SemanticCase& c,
                                        const CpuState& initial_state,
                                        const std::vector<std::uint8_t>& initial_memory,
                                        GuestAddress memory_base) {
  TrialResult result(memory_base, initial_memory.size());
  result.expected_state = initial_state;
  result.actual_state = initial_state;
  result.expected_memory.data() = initial_memory;
  result.actual_memory.data() = initial_memory;
  NullRuntimeServices expected_runtime;
  NullRuntimeServices actual_runtime;
  result.expected = capture_reference(c.instructions, result.expected_state,
                                      result.expected_memory, expected_runtime);
  result.actual = capture_candidate(c.candidate, result.actual_state,
                                    result.actual_memory, actual_runtime);
  result.state_difference = first_state_difference(result.expected_state,
                                                   result.actual_state);
  result.memory_difference = first_memory_difference(result.expected_memory,
                                                     result.actual_memory);
  result.mismatch = !same_result(result.expected, result.actual) ||
                    !result.state_difference.empty() ||
                    result.memory_difference.has_value();
  return result;
}

void randomize_state(CpuState& state, DeterministicRng& rng) noexcept {
  for (auto& value : state.gpr) value = rng.next_u64();
  for (auto& value : state.fpr_bits) value = rng.next_u64();
  for (auto& value : state.vr)
    for (auto& byte : value.bytes) byte = rng.next_u8();
  state.lr = rng.next_u64();
  state.ctr = rng.next_u64();
  state.cr = rng.next_u32();
  state.xer = rng.next_u32();
  state.fpscr = rng.next_u32();
  state.vscr = rng.next_u32();
  state.msr = rng.next_u64();
  state.vrsave = rng.next_u32();
  state.pvr = rng.next_u32();
  state.time_base = rng.next_u64();
  state.cia = rng.next_u32() & ~3u;
  state.nia = rng.next_u32() & ~3u;
  state.reservation.valid = (rng.next_u64() & 1u) != 0;
  state.reservation.width = (rng.next_u64() & 1u) ? 4u : 8u;
  state.reservation.address = rng.next_u32() & ~3u;
  state.reservation.observed_value = rng.next_u64();
  state.reservation.token = rng.next_u64();
}

[[nodiscard]] std::string mismatch_summary(const TrialResult& result) {
  if (result.expected.exception != result.actual.exception) {
    return std::string("exception class expected=") + exception_class_name(result.expected.exception) +
           " actual=" + exception_class_name(result.actual.exception);
  }
  if (result.expected.exception == ExceptionClass::None &&
      !same_result(result.expected, result.actual)) {
    return std::string("flow expected=") + std::string(flow_reason_name(result.expected.result.reason)) +
           "/" + hex32(result.expected.result.next_address) +
           " actual=" + std::string(flow_reason_name(result.actual.result.reason)) +
           "/" + hex32(result.actual.result.next_address);
  }
  if (!result.state_difference.empty()) return result.state_difference;
  if (result.memory_difference) return "memory differs at " + hex32(*result.memory_difference);
  return "semantic mismatch";
}

[[nodiscard]] bool still_fails(const SemanticCase& c, const CpuState& state,
                               const std::vector<std::uint8_t>& memory,
                               GuestAddress base) {
  return execute_trial(c, state, memory, base).mismatch;
}

void minimize_failure(const SemanticCase& c, CpuState& state,
                      std::vector<std::uint8_t>& memory, GuestAddress base,
                      std::size_t max_steps) {
  std::size_t steps = 0;
  auto try_u64 = [&](std::uint64_t& value) {
    if (!value || steps >= max_steps) return;
    ++steps; const auto old = value; value = 0;
    if (!still_fails(c, state, memory, base)) value = old;
  };
  auto try_u32 = [&](std::uint32_t& value) {
    if (!value || steps >= max_steps) return;
    ++steps; const auto old = value; value = 0;
    if (!still_fails(c, state, memory, base)) value = old;
  };
  for (auto& value : state.gpr) try_u64(value);
  for (auto& value : state.fpr_bits) try_u64(value);
  for (auto& vector : state.vr) {
    if (steps >= max_steps) break;
    const auto old = vector;
    if (std::all_of(vector.bytes.begin(), vector.bytes.end(), [](auto b){ return b == 0; })) continue;
    ++steps; vector = {};
    if (!still_fails(c, state, memory, base)) vector = old;
  }
  try_u64(state.lr); try_u64(state.ctr); try_u32(state.cr); try_u32(state.xer);
  try_u32(state.fpscr); try_u32(state.vscr); try_u64(state.msr);
  try_u32(state.vrsave); try_u64(state.time_base);

  // Coarse memory minimization is deliberately bounded.  Repro files retain
  // the exact minimized memory image, so a CI mismatch can always be replayed.
  constexpr std::size_t chunk = 64;
  for (std::size_t offset = 0; offset < memory.size() && steps < max_steps; offset += chunk) {
    const auto end = std::min(memory.size(), offset + chunk);
    bool any = false;
    for (std::size_t i = offset; i < end; ++i) any |= memory[i] != 0;
    if (!any) continue;
    std::vector<std::uint8_t> old(memory.begin() + static_cast<std::ptrdiff_t>(offset),
                                  memory.begin() + static_cast<std::ptrdiff_t>(end));
    ++steps;
    std::fill(memory.begin() + static_cast<std::ptrdiff_t>(offset),
              memory.begin() + static_cast<std::ptrdiff_t>(end), 0);
    if (!still_fails(c, state, memory, base))
      std::copy(old.begin(), old.end(), memory.begin() + static_cast<std::ptrdiff_t>(offset));
  }
}

[[nodiscard]] std::filesystem::path write_repro(
    const SemanticCase& c, const SemanticMismatch& mismatch,
    GuestAddress memory_base, const std::filesystem::path& directory) {
  if (directory.empty()) return {};
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) return {};
  auto safe = c.name;
  for (auto& ch : safe) if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-')) ch = '_';
  const auto path = directory / (safe + "-seed-" + hex64(mismatch.trial_seed).substr(2) + ".json");
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return {};
  out << "{\n  \"schema\": \"xenon.semantic-repro.v1\",\n";
  out << "  \"case\": \"" << safe << "\",\n";
  out << "  \"trial_index\": " << mismatch.trial_index << ",\n";
  out << "  \"trial_seed\": \"" << hex64(mismatch.trial_seed) << "\",\n";
  out << "  \"memory_base\": \"" << hex32(memory_base) << "\",\n";
  out << "  \"instructions\": [";
  for (std::size_t i = 0; i < c.instructions.size(); ++i) {
    if (i) out << ',';
    out << "{\"address\":\"" << hex32(c.instructions[i].address)
        << "\",\"word\":\"" << hex32(c.instructions[i].word) << "\"}";
  }
  out << "],\n  \"gpr\": [";
  for (std::size_t i = 0; i < mismatch.minimized_state.gpr.size(); ++i) {
    if (i) out << ',';
    out << '"' << hex64(mismatch.minimized_state.gpr[i]) << '"';
  }
  out << "],\n  \"fpr_bits\": [";
  for (std::size_t i = 0; i < mismatch.minimized_state.fpr_bits.size(); ++i) {
    if (i) out << ',';
    out << '"' << hex64(mismatch.minimized_state.fpr_bits[i]) << '"';
  }
  out << "],\n  \"special\": {\"lr\":\"" << hex64(mismatch.minimized_state.lr)
      << "\",\"ctr\":\"" << hex64(mismatch.minimized_state.ctr)
      << "\",\"cr\":\"" << hex32(mismatch.minimized_state.cr)
      << "\",\"xer\":\"" << hex32(mismatch.minimized_state.xer)
      << "\",\"fpscr\":\"" << hex32(mismatch.minimized_state.fpscr)
      << "\",\"vscr\":\"" << hex32(mismatch.minimized_state.vscr) << "\"},\n";
  out << "  \"memory_hex\": \"";
  for (const auto byte : mismatch.minimized_memory)
    out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
  out << "\"\n}\n";
  return path;
}

[[nodiscard]] VerificationReport run_seed(const SemanticCase& c,
                                          std::uint64_t trial_seed,
                                          std::size_t trial_index,
                                          const VerificationConfig& config) {
  VerificationReport report{};
  report.trials_run = 1;
  DeterministicRng rng(trial_seed);
  CpuState initial{};
  randomize_state(initial, rng);
  std::vector<std::uint8_t> memory(config.memory_size);
  for (auto& byte : memory) byte = rng.next_u8();
  FlatMemory setup_memory(config.memory_size, config.memory_base);
  setup_memory.data() = memory;
  if (c.setup) c.setup(initial, setup_memory, rng);
  memory = setup_memory.data();

  auto result = execute_trial(c, initial, memory, config.memory_base);
  if (!result.mismatch) return report;

  SemanticMismatch mismatch{};
  mismatch.case_name = c.name;
  mismatch.trial_index = trial_index;
  mismatch.trial_seed = trial_seed;
  mismatch.summary = mismatch_summary(result);
  mismatch.first_state_difference = result.state_difference;
  mismatch.first_memory_difference = result.memory_difference;
  mismatch.expected = result.expected;
  mismatch.actual = result.actual;
  mismatch.minimized_state = initial;
  mismatch.minimized_memory = memory;
  if (config.minimize_failure)
    minimize_failure(c, mismatch.minimized_state, mismatch.minimized_memory,
                     config.memory_base, config.max_minimization_steps);
  mismatch.repro_path = write_repro(c, mismatch, config.memory_base,
                                    config.repro_directory);
  report.mismatch = std::move(mismatch);
  return report;
}

}  // namespace

DeterministicRng::DeterministicRng(std::uint64_t seed) noexcept
    : state_(seed ? seed : 0xD1B54A32D192ED03ull) {}

std::uint64_t DeterministicRng::next_u64() noexcept {
  // xorshift64*: compact, deterministic and independent of stdlib RNG details.
  std::uint64_t x = state_;
  x ^= x >> 12u;
  x ^= x << 25u;
  x ^= x >> 27u;
  state_ = x;
  return x * 0x2545F4914F6CDD1Dull;
}

const char* exception_class_name(ExceptionClass value) noexcept {
  switch (value) {
    case ExceptionClass::None: return "none";
    case ExceptionClass::OutOfRange: return "out_of_range";
    case ExceptionClass::Logic: return "logic_error";
    case ExceptionClass::Runtime: return "runtime_error";
    case ExceptionClass::Standard: return "std_exception";
    case ExceptionClass::Unknown: return "unknown";
  }
  return "unknown";
}

bool ReferenceExecutor::supports(const DecodedInstruction& i) noexcept {
  const auto m = i.mnemonic();
  return m == "addi" || m == "addis" || m == "ori" || m == "oris" ||
         m == "xori" || m == "xoris" || m == "andix" || m == "andisx" ||
         m == "addx" || m == "subfx" || m == "andx" || m == "andcx" ||
         m == "orx" || m == "xorx" || m == "norx" || m == "extsbx" ||
         m == "extshx" || m == "extswx" || m == "cmp" || m == "cmpl" ||
         m == "cmpi" || m == "cmpli" || m == "cntlzwx" || m == "cntlzdx" ||
         m == "lbz" || m == "lhz" || m == "lwz" || m == "ld" ||
         m == "stb" || m == "sth" || m == "stw" || m == "std" ||
         m == "fmrx" || m == "fnegx" || m == "fabsx" ||
         m == "vand" || m == "vor" || m == "vxor" || m == "bx" ||
         ((m == "bclrx" || m == "bcctrx") && i.bo() == 20u && !i.lk());
}

ExecutionResult ReferenceExecutor::execute_one(const DecodedInstruction& i,
                                               CpuState& state,
                                               MemoryPort& memory,
                                               RuntimeServices&) {
  if (!supports(i))
    throw std::logic_error("Gen 8 reference semantics unsupported: " + std::string(i.mnemonic()));

  state.cia = i.address;
  state.nia = static_cast<GuestAddress>(i.address + 4u);
  const auto m = i.mnemonic();

  if (m == "addi" || m == "addis") {
    const auto base = i.ra() ? state.gpr[i.ra()] : 0ull;
    std::int64_t immediate = i.simm16();
    if (m == "addis") immediate <<= 16;
    state.gpr[i.rt()] = base + static_cast<std::uint64_t>(immediate);
  } else if (m == "ori" || m == "oris" || m == "xori" || m == "xoris" ||
             m == "andix" || m == "andisx") {
    std::uint64_t immediate = i.uimm16();
    if (m == "oris" || m == "xoris" || m == "andisx") immediate <<= 16u;
    const auto source = state.gpr[i.rs()];
    std::uint64_t value{};
    if (m == "ori" || m == "oris") value = source | immediate;
    else if (m == "xori" || m == "xoris") value = source ^ immediate;
    else value = source & immediate;
    state.gpr[i.ra()] = value;
    if (m == "andix" || m == "andisx") state.update_cr0_signed(value);
  } else if (m == "addx" || m == "subfx") {
    const auto a = state.gpr[i.ra()];
    const auto b = state.gpr[i.rb()];
    const auto value = m == "addx" ? a + b : b - a;
    state.gpr[i.rt()] = value;
    if (i.rc()) state.update_cr0_signed(value);
  } else if (m == "andx" || m == "andcx" || m == "orx" || m == "xorx" || m == "norx") {
    const auto a = state.gpr[i.rs()];
    const auto b = state.gpr[i.rb()];
    std::uint64_t value{};
    if (m == "andx") value = a & b;
    else if (m == "andcx") value = a & ~b;
    else if (m == "orx") value = a | b;
    else if (m == "xorx") value = a ^ b;
    else value = ~(a | b);
    state.gpr[i.ra()] = value;
    if (i.rc()) state.update_cr0_signed(value);
  } else if (m == "extsbx" || m == "extshx" || m == "extswx") {
    const auto source = state.gpr[i.rs()];
    std::uint64_t value{};
    if (m == "extsbx") value = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int8_t>(source)));
    else if (m == "extshx") value = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int16_t>(source)));
    else value = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(source)));
    state.gpr[i.ra()] = value;
    if (i.rc()) state.update_cr0_signed(value);
  } else if (m == "cmp" || m == "cmpl" || m == "cmpi" || m == "cmpli") {
    const bool logical = m == "cmpl" || m == "cmpli";
    const bool immediate = m == "cmpi" || m == "cmpli";
    const bool word = ((i.word >> 21u) & 1u) == 0u;
    const auto rhs = immediate
                         ? (logical ? std::uint64_t{i.uimm16()}
                                    : static_cast<std::uint64_t>(static_cast<std::int64_t>(i.simm16())))
                         : state.gpr[i.rb()];
    compare_to_cr(state, i.crfd(), state.gpr[i.ra()], rhs, logical, word);
  } else if (m == "cntlzwx" || m == "cntlzdx") {
    const auto source = state.gpr[i.rs()];
    const auto value = m == "cntlzwx"
                           ? static_cast<std::uint64_t>(std::countl_zero(static_cast<std::uint32_t>(source)))
                           : static_cast<std::uint64_t>(std::countl_zero(source));
    state.gpr[i.ra()] = value;
    if (i.rc()) state.update_cr0_signed(value);
  } else if (m == "lbz" || m == "lhz" || m == "lwz" || m == "ld") {
    const unsigned bytes = m == "lbz" ? 1u : m == "lhz" ? 2u : m == "lwz" ? 4u : 8u;
    const auto address = m == "ld" ? dsform_ea(i, state) : dform_ea(i, state);
    state.gpr[i.rt()] = read_scalar(memory, address, bytes);
  } else if (m == "stb" || m == "sth" || m == "stw" || m == "std") {
    const unsigned bytes = m == "stb" ? 1u : m == "sth" ? 2u : m == "stw" ? 4u : 8u;
    const auto address = m == "std" ? dsform_ea(i, state) : dform_ea(i, state);
    write_scalar(memory, address, bytes, state.gpr[i.rs()]);
  } else if (m == "fmrx" || m == "fnegx" || m == "fabsx") {
    auto bits = state.fpr_bits[i.rb()];
    if (m == "fnegx") bits ^= (1ull << 63u);
    else if (m == "fabsx") bits &= ~(1ull << 63u);
    state.fpr_bits[i.frt()] = bits;
    // Rc=0 is the current reference corpus. Fail closed if coverage expands
    // before CR1/FPSCR record-form semantics are independently modeled.
    if (i.rc()) throw std::logic_error("Gen 8 reference FPR record form not modeled");
  } else if (m == "vand" || m == "vor" || m == "vxor") {
    const auto& a = state.vr[i.va5()];
    const auto& b = state.vr[i.vb5()];
    Vector128 value{};
    for (std::size_t lane = 0; lane < value.bytes.size(); ++lane) {
      if (m == "vand") value.bytes[lane] = a.bytes[lane] & b.bytes[lane];
      else if (m == "vor") value.bytes[lane] = a.bytes[lane] | b.bytes[lane];
      else value.bytes[lane] = a.bytes[lane] ^ b.bytes[lane];
    }
    state.vr[i.vd5()] = value;
  } else if (m == "bx") {
    const auto target = i.direct_branch_target();
    if (i.lk()) state.lr = static_cast<std::uint64_t>(i.address + 4u);
    state.nia = target;
    return {FlowReason::Branch, target, 0};
  } else if (m == "bclrx" || m == "bcctrx") {
    // Only canonical unconditional, unlinked LR/CTR forms are currently
    // claimed as independently verified reference coverage. Conditional/link
    // forms fail closed in supports() until they receive their own corpus.
    const auto raw_target = m == "bclrx" ? state.lr : state.ctr;
    const auto target = static_cast<GuestAddress>(raw_target & ~3ull);
    state.nia = target;
    return {m == "bclrx" ? FlowReason::Return : FlowReason::Branch, target, 0};
  }

  state.nia = static_cast<GuestAddress>(i.address + 4u);
  return {FlowReason::Fallthrough, state.nia, 0};
}

ExecutionResult ReferenceExecutor::execute_block(
    std::span<const DecodedInstruction> instructions, CpuState& state,
    MemoryPort& memory, RuntimeServices& runtime) {
  if (instructions.empty()) throw std::logic_error("Gen 8 reference empty block");
  ExecutionResult result{};
  for (const auto& instruction : instructions) {
    result = execute_one(instruction, state, memory, runtime);
    if (result.reason != FlowReason::Fallthrough) return result;
  }
  return result;
}

VerificationReport SemanticVerifier::run(const SemanticCase& c,
                                         const VerificationConfig& config) const {
  if (!c.candidate) throw std::invalid_argument("semantic verifier candidate is null");
  if (c.instructions.empty()) throw std::invalid_argument("semantic verifier case has no instructions");
  for (const auto& instruction : c.instructions) {
    if (!ReferenceExecutor::supports(instruction))
      throw std::invalid_argument("semantic verifier reference does not support " + std::string(instruction.mnemonic()));
  }
  VerificationReport aggregate{};
  for (std::size_t trial = 0; trial < config.trials; ++trial) {
    const auto seed = splitmix64(config.seed ^ splitmix64(trial + 1u));
    auto one = run_seed(c, seed, trial, config);
    ++aggregate.trials_run;
    if (!one.ok()) { aggregate.mismatch = std::move(one.mismatch); break; }
  }
  return aggregate;
}

VerificationReport SemanticVerifier::replay(const SemanticCase& c,
                                            std::uint64_t trial_seed,
                                            const VerificationConfig& config) const {
  if (!c.candidate) throw std::invalid_argument("semantic verifier candidate is null");
  return run_seed(c, trial_seed, 0, config);
}

}  // namespace xenon::cpu::verify

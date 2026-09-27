#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/semantic_verifier.hpp"
#include "../verification/generated_fuzz_interface.hpp"
#include "../verification/semantic_corpus.hpp"

using namespace xenon::cpu;
using namespace xenon::cpu::verify;
using namespace xenon::cpu::verify::test_corpus;

#define DECLARE(name) ExecutionResult name(CpuState&, MemoryPort&, RuntimeServices&)
DECLARE(sem_addi); DECLARE(sem_addis); DECLARE(sem_ori); DECLARE(sem_xoris);
DECLARE(sem_andi); DECLARE(sem_add_record); DECLARE(sem_subf_record);
DECLARE(sem_extsw); DECLARE(sem_cmpi); DECLARE(sem_cmpld); DECLARE(sem_cntlzw);
DECLARE(sem_lwz); DECLARE(sem_stw); DECLARE(sem_ld); DECLARE(sem_std);
DECLARE(sem_fmr); DECLARE(sem_fneg); DECLARE(sem_vxor); DECLARE(sem_vor);
DECLARE(sem_branch); DECLARE(sem_blr); DECLARE(sem_bcctr);
DECLARE(sem_block_scalar); DECLARE(sem_block_memory);
DECLARE(sem_block_fp_vector);
#undef DECLARE

static CompiledEntry candidate_for(std::string_view name) {
#define MAP(N) if (name == #N) return &N
  MAP(sem_addi); MAP(sem_addis); MAP(sem_ori); MAP(sem_xoris); MAP(sem_andi);
  MAP(sem_add_record); MAP(sem_subf_record); MAP(sem_extsw); MAP(sem_cmpi);
  MAP(sem_cmpld); MAP(sem_cntlzw); MAP(sem_lwz); MAP(sem_stw); MAP(sem_ld);
  MAP(sem_std); MAP(sem_fmr); MAP(sem_fneg); MAP(sem_vxor); MAP(sem_vor);
  MAP(sem_branch); MAP(sem_blr); MAP(sem_bcctr);
  MAP(sem_block_scalar); MAP(sem_block_memory);
  MAP(sem_block_fp_vector);
#undef MAP
  return nullptr;
}

static const OpcodeInfo* find_opcode(std::string_view mnemonic) {
  for (const auto& op : Decoder::opcode_catalog())
    if (op.mnemonic == mnemonic) return &op;
  return nullptr;
}

static void memory_setup(CpuState& state, FlatMemory& memory, DeterministicRng&) {
  // Keep all D/DS accesses in-bounds while retaining randomized surrounding
  // memory.  The actual load/store values still come from the random image.
  state.gpr[3] = static_cast<std::uint64_t>(memory.base() + 0x180u);
}

int main() {
  Decoder decoder;
  SemanticVerifier verifier;
  VerificationConfig config{};
  config.seed = 0x47454E3856455249ull; // "GEN8VERI"
  config.trials = 128;
  config.memory_size = 0x800u;
  config.repro_directory = std::filesystem::current_path() / "semantic-repros";

  std::size_t total_trials = 0;
  std::size_t case_count = 0;
  for (const auto& spec : cases()) {
    SemanticCase c{};
    c.name = spec.name;
    c.candidate = candidate_for(spec.name);
    c.setup = spec.memory_setup ? &memory_setup : nullptr;
    if (!c.candidate) {
      std::cerr << "missing native candidate for " << spec.name << "\n";
      return 2;
    }
    for (const auto& instruction : spec.instructions) {
      const auto* info = find_opcode(instruction.mnemonic);
      if (!info) return 3;
      auto decoded = decoder.decode(instruction.address,
                                    info->pattern | instruction.operand_bits);
      if (!decoded.valid() || !ReferenceExecutor::supports(decoded)) {
        std::cerr << "reference coverage missing for " << instruction.mnemonic << "\n";
        return 4;
      }
      c.instructions.push_back(decoded);
    }

    // Give short blocks more randomized executions because they are the Gen 8
    // basic-block fuzzing surface; isolated opcodes remain bounded for CI.
    auto local = config;
    if (c.instructions.size() > 1) local.trials = 256;
    const auto report = verifier.run(c, local);
    total_trials += report.trials_run;
    ++case_count;
    if (!report.ok()) {
      const auto& mismatch = *report.mismatch;
      std::cerr << "semantic mismatch in " << mismatch.case_name
                << " trial=" << mismatch.trial_index
                << " seed=0x" << std::hex << mismatch.trial_seed << std::dec
                << "\n" << mismatch.summary << "\n";
      if (!mismatch.first_state_difference.empty())
        std::cerr << mismatch.first_state_difference << "\n";
      if (!mismatch.repro_path.empty())
        std::cerr << "repro: " << mismatch.repro_path.string() << "\n";
      return 5;
    }
  }

  for (const auto& generated : generated_semantic_fuzz_cases()) {
    SemanticCase c{};
    c.name = generated.name;
    c.candidate = generated.candidate;
    c.setup = generated.memory_setup ? &memory_setup : nullptr;
    for (std::size_t index = 0; index < generated.instruction_count; ++index) {
      const auto& encoded = generated.instructions[index];
      auto decoded = decoder.decode(encoded.address, encoded.word);
      if (!decoded.valid() || !ReferenceExecutor::supports(decoded)) {
        std::cerr << "generated reference coverage missing at " << generated.name
                  << " instruction=" << index << "\n";
        return 6;
      }
      c.instructions.push_back(decoded);
    }
    auto local = config;
    local.trials = 64;
    const auto report = verifier.run(c, local);
    total_trials += report.trials_run;
    ++case_count;
    if (!report.ok()) {
      const auto& mismatch = *report.mismatch;
      std::cerr << "generated semantic mismatch in " << mismatch.case_name
                << " trial=" << mismatch.trial_index
                << " seed=0x" << std::hex << mismatch.trial_seed << std::dec
                << "\n" << mismatch.summary << "\n";
      if (!mismatch.repro_path.empty())
        std::cerr << "repro: " << mismatch.repro_path.string() << "\n";
      return 7;
    }
  }

  std::cout << "xenon_cpu_semantic_verification: " << case_count
            << " cases, " << total_trials << " deterministic trials: ok\n";
  return 0;
}

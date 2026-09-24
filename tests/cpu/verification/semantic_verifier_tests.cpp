#include <cassert>
#include <filesystem>
#include <iostream>

#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/semantic_verifier.hpp"

using namespace xenon::cpu;
using namespace xenon::cpu::verify;

static ExecutionResult bad_addi(CpuState& state, MemoryPort&, RuntimeServices&) {
  state.cia = 0x8000u;
  state.gpr[5] = state.gpr[3] + 8u; // Oracle expects +7.
  state.nia = 0x8004u;
  return {FlowReason::Fallthrough, 0x8004u, 0};
}

static const OpcodeInfo* find_opcode(std::string_view mnemonic) {
  for (const auto& op : Decoder::opcode_catalog())
    if (op.mnemonic == mnemonic) return &op;
  return nullptr;
}

int main() {
  Decoder decoder;
  const auto* addi = find_opcode("addi");
  assert(addi);
  const auto instruction = decoder.decode(
      0x8000u, addi->pattern | (5u << 21) | (3u << 16) | 7u);
  assert(instruction.valid());
  assert(ReferenceExecutor::supports(instruction));

  const auto root = std::filesystem::temp_directory_path() /
                    "xenon-gen8-semantic-verifier-test";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  SemanticCase c{"intentional_mismatch", {instruction}, &bad_addi, nullptr};
  VerificationConfig config{};
  config.seed = 0x123456789ABCDEF0ull;
  config.trials = 4;
  config.memory_size = 256;
  config.repro_directory = root;
  config.minimize_failure = true;
  config.max_minimization_steps = 96;

  SemanticVerifier verifier;
  const auto report = verifier.run(c, config);
  assert(!report.ok());
  assert(report.trials_run == 1);
  assert(report.mismatch);
  assert(report.mismatch->first_state_difference.find("gpr[5]") != std::string::npos);
  assert(!report.mismatch->repro_path.empty());
  assert(std::filesystem::exists(report.mismatch->repro_path));

  // The exact recorded trial seed must reproduce the same failure without
  // replaying the earlier RNG sequence.
  auto replay_config = config;
  replay_config.repro_directory.clear();
  replay_config.minimize_failure = false;
  const auto replay = verifier.replay(c, report.mismatch->trial_seed, replay_config);
  assert(!replay.ok());
  assert(replay.mismatch->summary == report.mismatch->summary);

  std::filesystem::remove_all(root, ec);
  std::cout << "xenon_cpu_semantic_verifier_tests: ok\n";
  return 0;
}

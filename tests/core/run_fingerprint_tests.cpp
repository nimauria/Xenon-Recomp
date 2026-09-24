// Phase 0 of the AC6 Runtime Readiness / Platform Fidelity pass:
// RunFingerprint equality/serialization and the host identity helpers.

#include "xenon/core/run_fingerprint.hpp"

#include <cassert>
#include <iostream>

using xenon::core::RunFingerprint;

namespace {

RunFingerprint make_sample() {
  RunFingerprint fingerprint{};
  fingerprint.effective_xex_sha1 = "0123456789abcdef0123456789abcdef01234567";
  fingerprint.tu_identity = "1.0.0.0+1.0.1.0";
  fingerprint.xenon_build_id = "dev";
  fingerprint.game_module_build_id = "gracemeria-dev";
  fingerprint.analysis_schema_version = "v11";
  fingerprint.analysis_hash = "abc123";
  fingerprint.generated_code_hash = "def456";
  fingerprint.runtime_config_hash = "cfg789";
  fingerprint.gpu_backend = "vulkan";
  fingerprint.host_os = xenon::core::host_os_identifier();
  fingerprint.host_cpu_arch = xenon::core::host_cpu_arch_identifier();
  fingerprint.diagnostic_mode = "default";
  return fingerprint;
}

void test_equality_is_field_wise() {
  RunFingerprint a = make_sample();
  RunFingerprint b = make_sample();
  assert(a == b);

  b.gpu_backend = "d3d12";
  assert(!(a == b));

  b = make_sample();
  b.analysis_hash = "different";
  assert(!(a == b));
}

void test_to_json_round_trips_every_field() {
  const RunFingerprint fingerprint = make_sample();
  const auto json = fingerprint.to_json();

  assert(json.get_string("effectiveXexSha1") == fingerprint.effective_xex_sha1);
  assert(json.get_string("tuIdentity") == fingerprint.tu_identity);
  assert(json.get_string("xenonBuildId") == fingerprint.xenon_build_id);
  assert(json.get_string("gameModuleBuildId") == fingerprint.game_module_build_id);
  assert(json.get_string("analysisSchemaVersion") == fingerprint.analysis_schema_version);
  assert(json.get_string("analysisHash") == fingerprint.analysis_hash);
  assert(json.get_string("generatedCodeHash") == fingerprint.generated_code_hash);
  assert(json.get_string("runtimeConfigHash") == fingerprint.runtime_config_hash);
  assert(json.get_string("gpuBackend") == fingerprint.gpu_backend);
  assert(json.get_string("hostOs") == fingerprint.host_os);
  assert(json.get_string("hostCpuArch") == fingerprint.host_cpu_arch);
  assert(json.get_string("diagnosticMode") == fingerprint.diagnostic_mode);
}

void test_host_identity_helpers_are_known_values() {
  const std::string os = xenon::core::host_os_identifier();
  const std::string arch = xenon::core::host_cpu_arch_identifier();
  assert(!os.empty() && os != "unknown");
  assert(!arch.empty() && arch != "unknown");

  // Deterministic across calls within the same process/build.
  assert(os == xenon::core::host_os_identifier());
  assert(arch == xenon::core::host_cpu_arch_identifier());
}

}  // namespace

int main() {
  std::cout << "Testing RunFingerprint...\n";

  test_equality_is_field_wise();
  test_to_json_round_trips_every_field();
  test_host_identity_helpers_are_known_values();

  std::cout << "All run fingerprint tests passed!\n";
  return 0;
}

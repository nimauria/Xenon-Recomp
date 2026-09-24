#include "xenon/core/run_fingerprint.hpp"

namespace xenon::core {

JsonValue RunFingerprint::to_json() const {
  JsonValue value = JsonValue::make_object();
  value.set("effectiveXexSha1", effective_xex_sha1);
  value.set("tuIdentity", tu_identity);
  value.set("xenonBuildId", xenon_build_id);
  value.set("gameModuleBuildId", game_module_build_id);
  value.set("analysisSchemaVersion", analysis_schema_version);
  value.set("analysisHash", analysis_hash);
  value.set("generatedCodeHash", generated_code_hash);
  value.set("runtimeConfigHash", runtime_config_hash);
  value.set("gpuBackend", gpu_backend);
  value.set("hostOs", host_os);
  value.set("hostCpuArch", host_cpu_arch);
  value.set("diagnosticMode", diagnostic_mode);
  return value;
}

std::string host_os_identifier() {
#if defined(_WIN32)
  return "windows";
#elif defined(__linux__)
  return "linux";
#elif defined(__APPLE__)
  return "macos";
#else
  return "unknown";
#endif
}

std::string host_cpu_arch_identifier() {
#if defined(_M_X64) || defined(__x86_64__)
  return "x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__)
  return "arm64";
#else
  return "unknown";
#endif
}

}  // namespace xenon::core

// Minimal native "game module" shared library for
// tests/core/title_update_integration_tests.cpp. Exports the real Xenon
// module ABI (see docs/runtime/RUNTIME_HOST.md "Native extension contract") plus the
// optional Xenon_SupportedExecutableRevisions() identity export
// XenonSession::load_native_extension() checks (see src/core/session.cpp).
//
// This module never binds any compiled code (compiled_lookup stays null) -
// these tests only exercise load_game()/load_native_extension()'s effective-
// executable-identity gating, never start()/guest execution, so there is
// nothing to actually run. The declared-revisions string is read from an
// environment variable at call time rather than baked in at compile time, so
// one built binary can be reused for every positive/negative test case
// (each computed from a synthetic XEX built at test runtime, whose hash is
// not known at this file's compile time) without a nested per-case build.

#include "xenon/cpu/runtime.hpp"

#include <cstdlib>
#include <string>

#if defined(_WIN32)
#define XENON_TEST_MODULE_EXPORT extern "C" __declspec(dllexport)
#else
#define XENON_TEST_MODULE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

XENON_TEST_MODULE_EXPORT void Xenon_BindCompiledRegistry(xenon::cpu::ExecutionContext& context) {
  context.compiled_registry = nullptr;
  context.compiled_lookup = nullptr;
}

XENON_TEST_MODULE_EXPORT const char* Xenon_SupportedExecutableRevisions() {
  static std::string value;
#if defined(_WIN32)
  char* buffer = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&buffer, &size, "XENON_TEST_SUPPORTED_REVISIONS") == 0 && buffer != nullptr) {
    value = buffer;
    std::free(buffer);
  } else {
    value.clear();
  }
#else
  const char* env = std::getenv("XENON_TEST_SUPPORTED_REVISIONS");
  value = env != nullptr ? env : "";
#endif
  return value.c_str();
}

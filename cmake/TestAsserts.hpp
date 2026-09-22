#pragma once

// Test binaries must retain standard assert checks in every configuration.
#if defined(NDEBUG)
#undef NDEBUG
#endif

#if defined(_WIN32) && defined(_DEBUG)
// The default Debug CRT assert() failure path pops up an interactive
// "Debug Assertion Failed" dialog box and blocks indefinitely waiting for
// Abort/Retry/Ignore input - which is indistinguishable from a genuine hang
// in any headless/automated run (this project lost real wall-clock time to
// exactly this: a real compile error surfaced inside a test's own
// std::system() call, and the resulting assert() failure just sat there
// with near-zero CPU usage instead of failing fast). Route assertion/CRT
// error reports to stderr and abort immediately instead, matching how a
// Release build (or any non-MSVC toolchain) already behaves.
#include <crtdbg.h>
namespace xenon::test_asserts_detail {
struct DisableAssertDialog {
  DisableAssertDialog() noexcept {
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
  }
};
inline const DisableAssertDialog kDisableAssertDialog{};
}  // namespace xenon::test_asserts_detail
#endif

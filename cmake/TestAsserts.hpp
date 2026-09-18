#pragma once

// Test binaries must retain standard assert checks in every configuration.
#if defined(NDEBUG)
#undef NDEBUG
#endif

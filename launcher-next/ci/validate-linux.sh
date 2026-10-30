#!/bin/sh
set -eu
if [ -n "${CEF_ROOT:-}" ]; then
  cmake -S /work -B /out -G Ninja -DCMAKE_BUILD_TYPE=Release -DXENON_NEXT_BUILD_DESKTOP=ON -DCEF_ROOT="$CEF_ROOT"
else
  cmake -S /work -B /out -G Ninja -DCMAKE_BUILD_TYPE=Release -DXENON_NEXT_BUILD_DESKTOP=OFF
fi
cmake --build /out --parallel "${BUILD_JOBS:-4}"
ctest --test-dir /out --output-on-failure

# Building and testing Xenon

The configured CMake graph and executable tests are the source of truth for a
given host. Build artifacts belong under `build/` and are not committed.

## Normal Linux development build

Install Ninja and the dependencies described in `DEPENDENCIES.md`. The normal
debug preset enables tests, memory, graphics, audio, and the runtime host.
Qt 6.6+ is needed to include the launcher; pass its installation prefix when
configuring. Native Vulkan and DXC targets require their SDKs.

```sh
python3 tools/development/build_accountability.py prepare --build-dir build/linux-x64-debug
cmake --preset linux-x64-debug -DCMAKE_PREFIX_PATH=/path/to/qt/gcc_64
cmake --build build/linux-x64-debug --parallel 4
ctest --test-dir build/linux-x64-debug --output-on-failure
python3 tools/development/build_accountability.py check --build-dir build/linux-x64-debug
```

The audit reads the configured CMake File API graph. It reports production
sources without a target, test programs without a CTest entry, and explicit
platform or dependency exclusions. Run it after each configuration whose
source coverage matters.

## Vulkan tests on headless Linux

Vulkan backend tests need a Vulkan device. On a machine without a GPU,
Mesa's software rasterizer provides one:

```sh
sudo apt-get install mesa-vulkan-drivers vulkan-tools
vulkaninfo --summary   # expect deviceName = llvmpipe
```

## Checking Windows graphics code on Linux

D3D12 sources cannot be built on Linux, but they can be syntax-checked with
MinGW GCC and Microsoft's DirectX-Headers (MIT licensed, not vendored):

```sh
sudo apt-get install g++-mingw-w64-x86-64
git clone --depth 1 --branch v1.614.1 https://github.com/microsoft/DirectX-Headers.git /tmp/directx-headers
x86_64-w64-mingw32-g++-posix -std=c++20 -fsyntax-only -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
  -D__REQUIRED_RPCNDR_H_VERSION__=475 -DXENON_HAS_DXC=1 \
  -I /tmp/directx-headers/include/directx -I /tmp/directx-headers/include \
  -I include -I src src/graphics/d3d12/<file>.cpp
```

MinGW returns COM structs differently from MSVC, so every call to
`GetCPUDescriptorHandleForHeapStart()`, `GetGPUDescriptorHandleForHeapStart()`
or `ID3D12Resource::GetDesc()` is reported as an error. Treat that set as the
baseline and compare it before and after a change. Any other error is real.

`linux-x64-sanitizers` is a smaller Linux configuration for host-independent
memory, CPU, kernel, graphics frontend, and recomp tests. It compiles with
AddressSanitizer and UndefinedBehaviorSanitizer. LeakSanitizer may require a
host where process tracing does not interfere with it.

## Refactor starting point

The campaign started from `dev` commit `ef85bfc20fe2e586ffab3042dc9f2fbaad7db3eb`
on 7 October 2026. Before any source edits, the normal Linux debug preset
could not configure because Ninja was absent. A Makefiles configuration then
failed to provision SDL2 and XMA-capable FFmpeg from incomplete cached Git
checkouts. With audio, native Vulkan/DXC, launcher, and runtime host disabled,
CMake configured **117 tests**. That reduced build stopped on an undeclared
`audio_thread_` reference in `src/core/session.cpp` when audio was disabled.
Its log contained **32 warning lines** before the compile failure, including
unused functions and integer conversion/constant warnings in generated CPU
code. The complete pre-change CTest suite could not run; no pass count is
claimed for that revision.

Source inspection at that revision found 204 `src/**/*.cpp` files and 162
`tests/**/*.cpp` files. Twenty-two standalone test programs were absent from
CMake registration. Four launcher filesystem `.cpp` files were exact copies
of already compiled production sources; the launcher filesystem test was an
older copy of the compiled test with fewer assertions. Platform-dependent
audio, native renderer, launcher, and runtime-host files were excluded by the
reduced configuration. The source-accountability checker now distinguishes
those exclusions from unowned files.

This baseline records what was observed. It does not establish correctness
for a title run, Windows/D3D12, or any configuration that did not build.

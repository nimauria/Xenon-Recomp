@echo off
setlocal

echo ============================================================
echo Xenon Recomp - Core Bootstrap Setup
echo ============================================================
echo.

REM ============================================================
REM Ensure required directories exist
REM ============================================================

if not exist src\core mkdir src\core
if not exist include\xenon\core mkdir include\xenon\core

REM ============================================================
REM Root CMakeLists.txt
REM ============================================================

(
echo cmake_minimum_required^(VERSION 3.25^)
echo.
echo project^(
echo     XenonRecomp
echo     VERSION 0.1.0
echo     LANGUAGES C CXX
echo ^)
echo.
echo set^(CMAKE_CXX_STANDARD 20^)
echo set^(CMAKE_CXX_STANDARD_REQUIRED ON^)
echo set^(CMAKE_CXX_EXTENSIONS OFF^)
echo.
echo list^(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake"^)
echo.
echo include^(CompilerOptions OPTIONAL^)
echo include^(Platforms OPTIONAL^)
echo include^(Architectures OPTIONAL^)
echo include^(Warnings OPTIONAL^)
echo.
echo option^(XENON_BUILD_TESTS "Build Xenon Recomp tests" OFF^)
echo option^(XENON_ENABLE_GRAPHICS "Enable graphics subsystem" OFF^)
echo option^(XENON_ENABLE_AUDIO "Enable audio subsystem" OFF^)
echo option^(XENON_ENABLE_INPUT "Enable input subsystem" OFF^)
echo option^(XENON_ENABLE_NETWORK "Enable networking subsystem" OFF^)
echo.
echo add_library^(xenon_core STATIC^)
echo.
echo target_sources^(xenon_core
echo     PRIVATE
echo         src/core/runtime.cpp
echo ^)
echo.
echo target_include_directories^(xenon_core
echo     PUBLIC
echo         "${CMAKE_CURRENT_SOURCE_DIR}/include"
echo ^)
echo.
echo target_compile_features^(xenon_core PUBLIC cxx_std_20^)
echo.
echo add_library^(Xenon::Core ALIAS xenon_core^)
echo.
echo if^(XENON_BUILD_TESTS^)
echo     enable_testing^()
echo endif^()
) > CMakeLists.txt

REM ============================================================
REM Public Runtime Header
REM ============================================================

(
echo #pragma once
echo.
echo #include ^<cstdint^>
echo.
echo namespace xenon
echo {
echo.
echo struct RuntimeConfig
echo {
echo     bool enable_logging = true;
echo     bool enable_graphics = false;
echo     bool enable_audio = false;
echo     bool enable_input = false;
echo     bool enable_network = false;
echo };
echo.
echo class Runtime
echo {
echo public:
echo     Runtime^(^);
echo     ~Runtime^(^);
echo.
echo     Runtime^(const Runtime^&^) = delete;
echo     Runtime^& operator=^(const Runtime^&^) = delete;
echo.
echo     bool initialize^(const RuntimeConfig^& config^);
echo     void shutdown^(^);
echo.
echo     bool is_initialized^(^) const noexcept;
echo.
echo private:
echo     bool initialized_ = false;
echo     RuntimeConfig config_{};
echo };
echo.
echo } // namespace xenon
) > include\xenon\core\runtime.hpp

REM ============================================================
REM Runtime Implementation
REM ============================================================

(
echo #include "xenon/core/runtime.hpp"
echo.
echo #include ^<iostream^>
echo.
echo namespace xenon
echo {
echo.
echo Runtime::Runtime^(^) = default;
echo.
echo Runtime::~Runtime^(^)
echo {
echo     shutdown^(^);
echo }
echo.
echo bool Runtime::initialize^(const RuntimeConfig^& config^)
echo {
echo     if ^(initialized_^)
echo     {
echo         return true;
echo     }
echo.
echo     config_ = config;
echo.
echo     if ^(config_.enable_logging^)
echo     {
echo         std::cout ^<^< "[Xenon] Initializing runtime...\n";
echo     }
echo.
echo     initialized_ = true;
echo.
echo     if ^(config_.enable_logging^)
echo     {
echo         std::cout ^<^< "[Xenon] Runtime initialized.\n";
echo     }
echo.
echo     return true;
echo }
echo.
echo void Runtime::shutdown^(^)
echo {
echo     if ^(!initialized_^)
echo     {
echo         return;
echo     }
echo.
echo     if ^(config_.enable_logging^)
echo     {
echo         std::cout ^<^< "[Xenon] Shutting down runtime...\n";
echo     }
echo.
echo     initialized_ = false;
echo }
echo.
echo bool Runtime::is_initialized^(^) const noexcept
echo {
echo     return initialized_;
echo }
echo.
echo } // namespace xenon
) > src\core\runtime.cpp

REM ============================================================
REM CMake Presets
REM ============================================================

(
echo {
echo   "version": 6,
echo   "cmakeMinimumRequired": {
echo     "major": 3,
echo     "minor": 25,
echo     "patch": 0
echo   },
echo   "configurePresets": [
echo     {
echo       "name": "base",
echo       "hidden": true,
echo       "generator": "Ninja",
echo       "binaryDir": "${sourceDir}/build/${presetName}",
echo       "cacheVariables": {
echo         "CMAKE_EXPORT_COMPILE_COMMANDS": "ON"
echo       }
echo     },
echo     {
echo       "name": "windows-x64-debug",
echo       "inherits": "base",
echo       "displayName": "Windows x64 Debug",
echo       "cacheVariables": {
echo         "CMAKE_BUILD_TYPE": "Debug"
echo       }
echo     },
echo     {
echo       "name": "windows-x64-release",
echo       "inherits": "base",
echo       "displayName": "Windows x64 Release",
echo       "cacheVariables": {
echo         "CMAKE_BUILD_TYPE": "Release"
echo       }
echo     }
echo   ]
echo }
) > CMakePresets.json

REM ============================================================
REM Finish
REM ============================================================

echo.
echo ============================================================
echo Xenon Core bootstrap created successfully.
echo ============================================================
echo.
echo Created:
echo.
echo   CMakeLists.txt
echo   CMakePresets.json
echo   include\xenon\core\runtime.hpp
echo   src\core\runtime.cpp
echo.
echo Network support remains DISABLED.
echo Graphics, audio and input remain DISABLED.
echo.
echo Xenon::Core and xenon_core are now available to Gracemeria.
echo ============================================================

endlocal
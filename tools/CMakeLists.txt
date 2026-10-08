# Developer and preparation tools. Executables stay at the build root, where
# scripts and the launcher's side-by-side lookup expect them.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")

foreach(xenon_tool recomp-driver ppc-disasm ir-dump module-inspector)
  add_executable(${xenon_tool} recomp_tools.cpp)
  target_compile_definitions(${xenon_tool} PRIVATE XENON_TOOL_NAME="${xenon_tool}")
  target_link_libraries(${xenon_tool} PRIVATE xenon_recomp)
  xenon_enable_warnings(${xenon_tool})
endforeach()

# import-scanner additionally links xenon_core: for Project Gracemeria import
# auditing (see docs/xam/XAM_V1.md), it cross-references every import against a
# real, minimally-initialized XenonSession export_registry_ - the exact same
# production registry XamSession::register_exports() populates - so its
# "implemented/not implemented" verdicts reflect production resolution, not
# a second, hand-maintained list. Deliberately game-agnostic: it inspects
# whatever XEX is passed on the command line, never a hard-coded title.
add_executable(import-scanner recomp_tools.cpp)
target_compile_definitions(import-scanner PRIVATE XENON_TOOL_NAME="import-scanner" XENON_TOOL_HAS_CORE=1)
target_link_libraries(import-scanner PRIVATE xenon_recomp xenon_core)
xenon_enable_warnings(import-scanner)

# xenon-prepare: the out-of-process automatic game preparation worker (see
# docs/development/GAME_PREPARATION.md). Reads default.xex directly out of a disc image/
# directory (xenon_filesystem, transitively linked via xenon_recomp ->
# xenon_xbox_kernel_io -> xenon_kernel -> xenon_filesystem), analyzes and
# generates native source (xenon_recomp), then drives a nested CMake build
# and promotes the result through the artifact cache. XENON_SOURCE_ROOT lets
# it default --recomp-root to this checkout, matching xenon_recomp_driver_tests.
add_executable(xenon-prepare xenon_prepare.cpp)
target_link_libraries(xenon-prepare PRIVATE xenon_recomp)
target_compile_definitions(xenon-prepare PRIVATE XENON_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
target_compile_definitions(xenon-prepare PRIVATE
  XENON_NATIVE_COMPILER="${CMAKE_CXX_COMPILER}"
  XENON_CMAKE_COMMAND="${CMAKE_COMMAND}")

if(NOT WIN32)
  target_link_libraries(xenon-prepare PRIVATE ${CMAKE_DL_LIBS})
endif()
xenon_enable_warnings(xenon-prepare)

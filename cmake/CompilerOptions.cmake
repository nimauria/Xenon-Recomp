# Every executable opts in to Windows long paths. The OS policy
# (LongPathsEnabled) only takes effect for processes whose manifest declares
# longPathAware; preparation caches nest content-addressed keys deeply enough
# to pass 260 characters under an ordinary user cache directory.
function(xenon_embed_windows_manifest)
  if(NOT MSVC)
    return()
  endif()

  get_property(_directories DIRECTORY "${XENON_SOURCE_DIR}" PROPERTY SUBDIRECTORIES)
  list(PREPEND _directories "${XENON_SOURCE_DIR}")
  while(_directories)
    list(POP_FRONT _directories _directory)
    get_property(_targets DIRECTORY "${_directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_target IN LISTS _targets)
      get_target_property(_type "${_target}" TYPE)
      if(_type STREQUAL "EXECUTABLE")
        target_sources("${_target}" PRIVATE
          "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/windows/long-path-aware.manifest")
      endif()
    endforeach()
    get_property(_children DIRECTORY "${_directory}" PROPERTY SUBDIRECTORIES)
    list(APPEND _directories ${_children})
  endwhile()
endfunction()

function(xenon_enable_guest_fp_semantics target visibility)
  if(MSVC)
    target_compile_options(${target} ${visibility} /fp:strict)
  else()
    # FPSCR rounding and exception state is guest-visible. Generated code must
    # preserve the host floating environment rather than contract operations.
    target_compile_options(${target} ${visibility}
      -frounding-math
      -ffp-contract=off
      -fno-fast-math)
  endif()
endfunction()

function(xenon_enable_test_asserts target)
  if(MSVC)
    # CMake's Release flags define NDEBUG. Undefine it in a forced header
    # instead of passing /UNDEBUG, which produces MSVC warning D9025 for every
    # test target while still keeping assertions active.
    target_compile_options(${target} PRIVATE
      "/FI${CMAKE_CURRENT_FUNCTION_LIST_DIR}/TestAsserts.hpp")
  else()
    target_compile_options(${target} PRIVATE
      -include "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/TestAsserts.hpp")
  endif()
endfunction()

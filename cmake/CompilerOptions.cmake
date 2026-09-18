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

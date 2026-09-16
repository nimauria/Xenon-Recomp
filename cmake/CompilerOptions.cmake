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
    target_compile_options(${target} PRIVATE /UNDEBUG)
  else()
    target_compile_options(${target} PRIVATE -UNDEBUG)
  endif()
endfunction()

# -----------------------------------------------------------------------------
# CPU: Xenon PPC/VMX128 frontend -> Xenon IR -> optimizer -> native C++ AOT.
add_library(xenon_cpu STATIC
  # PowerPC frontend: decode and lift guest instructions to Xenon IR.
  ppc/decoder/decoder.cpp
  ppc/lifter/lifter.cpp
  ppc/lifter/integer.cpp
  ppc/lifter/control_flow.cpp
  ppc/lifter/memory.cpp
  ppc/lifter/floating_point.cpp
  ppc/lifter/vector.cpp
  ppc/state/state.cpp
  # IR, verification and optimization.
  ir/ir.cpp
  ir/verifier.cpp
  optimizer/optimizer.cpp
  optimizer/passes/value_simplification.cpp
  optimizer/passes/redundancy_elimination.cpp
  optimizer/passes/state_promotion.cpp
  optimizer/passes/control_flow_simplification.cpp
  verification/semantic_verifier.cpp
  # Code generation: function pipeline, C++ AOT emission, AOT runtime
  # semantics and the dynamic-fallback interpreter.
  codegen/function_compiler.cpp
  codegen/emission/backend_cpp_aot.cpp
  codegen/emission/instruction_emission.cpp
  codegen/aot/aot_semantics.cpp
  codegen/fallback/dynamic_fallback.cpp
  codegen/fallback/interpreter/scalar_memory.cpp
  codegen/fallback/interpreter/integer.cpp
  codegen/fallback/interpreter/system.cpp
  codegen/fallback/interpreter/floating_point.cpp
  codegen/fallback/interpreter/vector.cpp
  # Execution support shared with the runtime.
  execution/memory_port.cpp
  execution/external_calls.cpp
  execution/executable_code_cache.cpp
  execution/flat_memory.cpp
)
target_include_directories(xenon_cpu
  PUBLIC "${XENON_SOURCE_DIR}/include"
  # Private headers are included as "cpu/<area>/<header>.hpp".
  PRIVATE "${XENON_SOURCE_DIR}/src"
)
target_compile_features(xenon_cpu PUBLIC cxx_std_20)
xenon_enable_guest_fp_semantics(xenon_cpu PUBLIC)
xenon_enable_warnings(xenon_cpu)
if(MSVC)
  # decoder.cpp builds its opcode dispatch table via a consteval bucket-sort
  # over the full PPC opcode catalog; this legitimately exceeds MSVC's
  # default constexpr evaluation step budget (1,048,576).
  target_compile_options(xenon_cpu PRIVATE /constexpr:steps100000000)
endif()
add_library(Xenon::CPU ALIAS xenon_cpu)

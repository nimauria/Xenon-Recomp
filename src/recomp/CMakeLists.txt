# Whole-game static recompilation driver and its diagnostic utilities.
# Gen 10 producer identities follow implementation changes even if a developer
# forgets a manual ABI bump. CMake reconfigures when any producer input changes.
file(GLOB_RECURSE _graph_ir_inputs CONFIGURE_DEPENDS
  "${XENON_SOURCE_DIR}/src/cpu/ppc/*"
  "${XENON_SOURCE_DIR}/src/cpu/ir/*"
  "${XENON_SOURCE_DIR}/src/cpu/optimizer/*"
  "${XENON_SOURCE_DIR}/include/xenon/cpu/*.hpp")
list(APPEND _graph_ir_inputs "${XENON_SOURCE_DIR}/src/cpu/codegen/function_compiler.cpp")
set(_graph_ir_identity "")
foreach(_input IN LISTS _graph_ir_inputs)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_input}")
  file(SHA256 "${_input}" _hash)
  string(APPEND _graph_ir_identity "${_hash};")
endforeach()
string(SHA256 XENON_GRAPH_IR_PRODUCER "${_graph_ir_identity}")
# The generated-source producer is exactly the C++ AOT emitter.
file(GLOB _graph_codegen_inputs CONFIGURE_DEPENDS
  "${XENON_SOURCE_DIR}/src/cpu/codegen/emission/*")
list(SORT _graph_codegen_inputs)
set(_graph_codegen_identity "")
foreach(_input IN LISTS _graph_codegen_inputs)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_input}")
  file(SHA256 "${_input}" _hash)
  string(APPEND _graph_codegen_identity "${_hash};")
endforeach()
string(SHA256 XENON_GRAPH_CODEGEN_PRODUCER "${_graph_codegen_identity}")
find_package(Threads REQUIRED)

add_library(xenon_recomp STATIC
  # Orchestration: load_and_analyze() and generate_project().
  driver/analysis_pipeline.cpp
  driver/project_generation.cpp
  driver/driver_support.cpp
  driver/worker_pool.cpp
  # Static analysis.
  analysis/executable_ranges.cpp
  analysis/discovery/candidate_filters.cpp
  analysis/discovery/discovery_sources.cpp
  analysis/discovery/discovery_waves.cpp
  analysis/discovery/instruction_classification.cpp
  analysis/discovery/pointer_tables.cpp
  analysis/discovery/recovery.cpp
  analysis/discovery/seed_collection.cpp
  analysis/control_flow/candidate_analysis.cpp
  analysis/control_flow/function_chunks.cpp
  analysis/control_flow/local_cfg_closure.cpp
  analysis/control_flow/primary_scan.cpp
  analysis/control_flow/return_inference.cpp
  analysis/control_flow/switch_tables.cpp
  analysis/value/ppc_value_analysis.cpp
  analysis/ownership/entry_reconciliation.cpp
  analysis/ownership/function_ownership.cpp
  analysis/validation/codegen_validation.cpp
  analysis/diagnostics/analysis_diagnostics.cpp
  analysis/diagnostics/cpu_coverage.cpp
  # Module hints and learned knowledge.
  hints/analysis_schema.cpp
  hints/analysis_schema_json.cpp
  hints/module_hint_provider.cpp
  hints/module_hints.cpp
  knowledge/adaptive_observations.cpp
  knowledge/knowledge_base.cpp
  knowledge/knowledge_matching.cpp
  # Generated-project emission.
  compilation/codegen/function_emission.cpp
  compilation/graph/compilation_graph.cpp
  compilation/project/generated_project_files.cpp
  compilation/project/generated_registry.cpp
  compilation/project/generation_staging.cpp
  compilation/shards/shard_assembly.cpp
  # Caching, reporting and title intake.
  caching/artifact_cache.cpp
  caching/configuration_identity.cpp
  reporting/report_json.cpp
  reporting/report_text.cpp
  intake/game_intake.cpp
  # Runtime support linked into generated game modules.
  runtime/native_replacements.cpp
  runtime/runtime_helpers.cpp
)
target_compile_definitions(xenon_recomp PRIVATE
  XENON_GRAPH_IR_PRODUCER="${XENON_GRAPH_IR_PRODUCER}"
  XENON_GRAPH_CODEGEN_PRODUCER="${XENON_GRAPH_CODEGEN_PRODUCER}")

target_include_directories(xenon_recomp
  PUBLIC "${XENON_SOURCE_DIR}/include"
  # Private headers are included as "recomp/<area>/<header>.hpp".
  PRIVATE "${XENON_SOURCE_DIR}/src")
target_link_libraries(xenon_recomp PUBLIC xenon_cpu xenon_xbox_kernel_io Threads::Threads)
target_compile_features(xenon_recomp PUBLIC cxx_std_20)
xenon_enable_warnings(xenon_recomp)
add_library(Xenon::Recomp ALIAS xenon_recomp)

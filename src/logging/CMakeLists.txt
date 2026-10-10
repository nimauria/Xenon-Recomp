# -----------------------------------------------------------------------------
# Logging: the AC6 Runtime Readiness pass's shared diagnostic backbone (Phase
# 0). A standalone library (not folded directly into xenon_core) so any other
# static library that needs it - GPU/graphics included - can link it
# directly without depending on the rest of xenon_core, and without risking an
# ODR violation from two libraries each compiling their own copy of
# Logger::instance()'s function-local static singleton.
add_library(xenon_logging STATIC
  diagnostic_events.cpp
  logger.cpp
  probe_log.cpp
)
target_include_directories(xenon_logging PUBLIC "${XENON_SOURCE_DIR}/include")
target_compile_features(xenon_logging PUBLIC cxx_std_20)
xenon_enable_warnings(xenon_logging)
add_library(Xenon::Logging ALIAS xenon_logging)

PROJECT XENON — GEN5 ADAPTIVE ANALYSIS HARDENING

This generation is layered on GEN1-GEN4. It contains complete changed files, not patches.

Implemented:
- Transactional root generation with generation-status.json so failed generation cannot masquerade as fresh output.
- Analysis report schema v3 and analysis engine revision v5; artifact ABI bumped to v3.
- Central optional UTF-8 BOM stripping in the JSON parser.
- Exact branch provenance: site, target, linked, indirect, terminal, conditional, fallthrough.
- Tail-call promotion restricted to terminal, unconditional, non-linked direct branches.
- Runtime/adaptive evidence scoped to the exact effective executable SHA-1 revision.
- Runtime observation ingestion bounded and validated; session learning bounded to 65,536 distinct misses.
- Region+Entry integrity validation before C++ emission.
- Richer analysis metrics for semantic functions, compiled regions, guest entries, alternate entries, adaptive rejection and integrity checks.
- Regression coverage for BOM JSON, revision-scoped observations and transactional failed regeneration.
- Design/research notes documenting lessons generalized from XenonRecomp/Xenia/ReXGlue/Dead Rising and from N64Recomp, angr, rev.ng, LLVM BOLT and XenosRecomp.

Validation in this environment:
- xenon_recomp compiled successfully.
- xenon_core compiled successfully.
- discovery_quality_tests: all tests passed.
- analysis_schema_tests: all tests passed.
- codegen_ownership_tests: all tests passed.
- Full nested recomp_driver_tests was not completed here because it launches a fresh native generated-module build and exceeded the execution window; no assertion failure was observed before timeout.

No AC6-specific production addresses or title special-cases were added.

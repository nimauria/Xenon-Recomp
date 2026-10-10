# Phase 1 CI validation findings

This file records observed CI results separately from the [kernel export metadata reconciliation](KERNEL_REFERENCE.md). A failed test is not automatically a confirmed runtime bug; its cause still needs investigation.

## Existing branch runs inspected

- [Run 37846434911](https://github.com/nimauria/Xenon-Recomp/actions/runs/37846434911) at commit `d7600d1` was still in progress when inspected on 2026-10-08. The coverage and sanitizer jobs had succeeded; Windows and Linux native jobs had not completed. No overall passing status can be claimed for this run.
- [Run 37838330635](https://github.com/nimauria/Xenon-Recomp/actions/runs/37838330635) at commit `c6a6c32` completed with a failing Windows test step. The Linux and sanitizer jobs succeeded. Windows reported 5 failures out of 148 CTest cases:
  - `xenon_dependency_bootstrap_tests`: the Windows line-ending FFmpeg patch fixture failed `git apply --check`. This is a dependency-tooling test failure.
  - `xenon_backend_capability_tests`: a Direct3D 12 pipeline initialization assertion failed. The log also stated there was no Vulkan device on that runner; that statement is separate from the D3D12 assertion.
  - `xenon_compilation_graph_tests`, `xenon_prepare_worker_tests`, and `xenon_xbox_threading_exports_tests`: CTest timed out each at 900 seconds. The threading output reached `slist_concurrent`, consistent with the previously documented intermittent SList hang. The other two timeout causes remain unconfirmed.

The Phase 1 workflow split retains all these tests and the 900-second per-test timeout. It does not suppress or reclassify failures. The first separate Windows, Linux, and overall workflow runs were created for commit `9919f19`; their conclusions must be read from Actions after they finish.

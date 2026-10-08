# README CI badges

GitHub Actions badges report a workflow, not an individual job. The README therefore links to three workflow badges pinned to `development-restructure` push runs:

| Badge | Workflow | Required work |
| --- | --- | --- |
| Windows MSVC / CL | `.github/workflows/windows.yml` | MSVC `cl.exe` discovery, pinned dependency bootstrap, CMake configure/build, D3D12 and runtime targets, full CTest run, source ownership audit |
| Linux | `.github/workflows/linux.yml` | Pinned dependency bootstrap, CMake configure/build, Vulkan and runtime/launcher targets, full CTest run, source ownership audit |
| Overall CI | `.github/workflows/ci.yml` | Coverage tooling and stale assets, focused ASan/UBSan tests, and same-commit Windows/Linux results |

The platform workflows each run the native build once. The overall workflow queries GitHub's Actions API for the corresponding platform workflow runs with the same event and source commit (`pull_request.head.sha` for PRs). It succeeds only after both complete successfully and their required native jobs report success. Missing, skipped, failed, cancelled, or timed-out platform runs never count as passing. A manual dispatch of overall CI needs matching manual platform runs at the same commit; otherwise it times out and fails.

CTest writes a JUnit file. `tools/coverage/check_ctest.py` checks that it contains at least one case and no skipped, failed, or errored cases. CTest itself fails on test assertions and timeouts; neither workflow filters out the documented intermittent SList/threading hang.

Badges reflect the most recent matching workflow run on the branch. They do not certify unpushed commits, unfinished runs, or other branches. The static dashboard describes reviewed source evidence; live CI results are reported by GitHub Actions.

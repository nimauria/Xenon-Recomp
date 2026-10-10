# Contributing to Xenon Recomp

Xenon is a solo-led, experimental project that is still changing quickly. There is no formal contributor programme yet, but issues, research notes and focused pull requests are welcome.

## Before you start

- Read the [README](README.md) for the project's current state. No commercial title is playable yet, and most work is in runtime correctness.
- Read [`docs/architecture/PROJECT_STRUCTURE.md`](docs/architecture/PROJECT_STRUCTURE.md) for source ownership, CMake targets and dependency rules, plus the subsystem document under [`docs/`](docs/) for the area you are changing.
- For a large or architectural change, open an issue first so the approach can be agreed before you write the code.

## Useful contributions

- **Bug reports** with the platform, build configuration, steps to reproduce and the relevant log or test output.
- **Research notes** on Xbox 360 hardware or system behaviour, with their sources.
- **Focused pull requests** that fix one problem or add one capability, with tests.
- **Documentation corrections** where a document no longer matches the code.

## Pull request expectations

- Keep each pull request to one coherent change. Structural moves and behaviour changes go in separate commits.
- Add or update focused tests for new behaviour where practical. Never weaken, skip or extend the timeout of a test just to make CI pass.
- Build and run the tests locally first, as described in [`docs/development/TESTING.md`](docs/development/TESTING.md), and run the source-ownership audit (`tools/development/build_accountability.py`).
- Keep diagnostics clear. Unsupported states should be reported, not hidden behind plausible but wrong results.
- Update the relevant documentation in the same change. Documents describe what the code does today, not the intended end state.
- Record any third-party research the change relies on in [`docs/development/RESEARCH_PROVENANCE.md`](docs/development/RESEARCH_PROVENANCE.md). Third-party code keeps its licence and attribution; see [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

## Engineering principles

- **Keep Xbox behaviour generic.** Behaviour that belongs to the Xbox 360 hardware or system software is implemented once, in the shared runtime.
- **Keep title knowledge in modules.** Never add hard-coded game checks to the runtime to make one title work. A fix found through one game should become a reusable correction, with regression tests.
- **Keep one memory truth.** CPU, GPU, DMA, XEX loading, kernel services and devices agree on guest-memory ownership and coherency.
- **Keep native backends native.** Vulkan and D3D12 consume canonical Xenon state rather than implementing divergent Xbox semantics.
- **Preserve layer boundaries.** Qt belongs to the launcher, game patches belong to modules, and host APIs do not leak into guest-semantic layers.
- **Respect the scope.** Xenon is an Xbox 360 project only. Support for other consoles is out of scope.

## AI-assisted contributions

AI tools have played a part in accelerating progress on this project. AI-assisted contributions are accepted on the same terms as any other: every AI-assisted change is audited and checked before it is added or approved. It is reviewed against the code it touches and the Xbox 360 behaviour it claims to implement, and it must meet the same testing and documentation standards. You are responsible for understanding and verifying everything you submit.

## Content you must not submit

Never submit proprietary Xbox 360 material: game executables or data, firmware, operating-system files, encryption keys, title updates, DLC or Microsoft-owned software. Do not include such material in tests, fixtures, logs or captures either. Test fixtures must be synthetic or freely licensed.

## Licence

By contributing, you agree that your contribution is released under the project's [MIT License](LICENSE).

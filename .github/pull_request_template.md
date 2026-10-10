## Summary

<!-- What does this change do, and why? Link any related issue. -->

## Testing

<!-- What did you build and run? Include the preset and platform, and the CTest results. -->

## Checklist

- [ ] The change is one coherent piece of work; structural moves and behaviour changes are in separate commits.
- [ ] New or changed behaviour has focused tests, and no test was weakened, skipped or given a longer timeout to make it pass.
- [ ] The tests and the source-ownership audit (`tools/development/build_accountability.py check`) pass locally.
- [ ] Generic Xbox 360 behaviour is in the shared runtime; nothing game-specific was added to Xenon itself.
- [ ] Affected documentation is updated to describe what the code now does.
- [ ] Third-party research or code is recorded in `docs/development/RESEARCH_PROVENANCE.md` or `THIRD_PARTY_NOTICES.md`.
- [ ] Any AI-assisted parts have been reviewed and verified by me, as described in `CONTRIBUTING.md`.
- [ ] The change contains no proprietary Xbox 360 content, keys or firmware.

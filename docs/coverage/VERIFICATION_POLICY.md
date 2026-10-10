# Coverage verification policy

This policy decides when the dashboard may show an operation as **verified** and who may change a classification. The [coverage-refresh automation](AUTOMATION.md) enforces the parts that a machine can check. It never makes the human decisions.

## States

| State | Meaning |
| --- | --- |
| Listed (recognized) | The operation appears in a source-declared inventory (kernel registration, PPC decoder catalog, Xenos shader form, PM4 enum). Listing alone says nothing about behaviour. |
| Unassessed | Listed, but no reviewed entry exists in `tools/coverage/coverage.json`. Unknown, not missing. |
| Unimplemented | Reviewed: Xenon explicitly rejects or lacks the operation. |
| Stub | Reviewed: a deliberate placeholder (for example a success-returning no-op). |
| Partial | Reviewed: real behaviour with a documented limitation. |
| Implemented, unverified | Reviewed: an implementation is traced, but no qualifying passing behaviour test exists. |
| Verified | All four conditions below hold. |

"Classified" counts every listed operation that has a reviewed state.

## Conditions for verified

An operation is shown as verified only when:

1. **An implementation exists.** The manifest entry names an implementation file under `src/` or `include/`, and its implementation token is present in that file.
2. **A semantic test asserts its behaviour.** The entry names a test source under `tests/`, a token in it (normally the test function, which must also be invoked), and the CTest target that compiles that source. A test that only checks decoding or registration does not qualify. Use `implemented_unverified` until a behaviour test exists.
3. **The required test has a successful result.** The named CTest target passed in the recorded Windows/Linux CI evidence: on at least one platform whose results contain it, with no failure, timeout or skip on any platform. A target missing from both platforms' results does not qualify.
4. **The mapping was reviewed.** The entry reached `tools/coverage/coverage.json` through a human-authored, reviewed change on `development-restructure`. The manifest is the approval record. The automation cannot write to it: its pull requests may only touch the generated files in `docs/coverage/`.

The generator checks conditions 1–3 on every run. When condition 3 fails, the dashboard shows the operation as *implemented, unverified* and lists the reason in [AUDIT.md](AUDIT.md) as a validation problem. It is never shown as unimplemented and never removed from the manifest. When the test passes again, the next refresh shows it as verified again with no manifest change.

## Evidence freshness

CI results are tied to the commit they ran on. A refresh first waits, within a bounded time, for the Windows and Linux runs of the assessed commit. If they are not available, it uses the newest completed run on the branch and records that run's commit. `AUDIT.md` then reports the evidence as **stale** (`ci_stale`). Stale passing evidence still counts, because it is a real recorded pass, but it is flagged until fresh results replace it. If no results can be read at all, the previously recorded outcomes are kept and the problem is reported (`ci_unavailable`). Missing results never count as passes.

## Audit candidates

The refresh scans source and tests for likely implementations, declared stubs/partials, IR-only PM4 packets and tests that mention an operation. These are recorded in [`audit-candidates.json`](audit-candidates.json) and [AUDIT.md](AUDIT.md). **They are never applied automatically.** To act on one, a reviewer inspects the code and test, then edits `coverage.json` by hand.

A test may declare which operations it covers with a comment such as `// coverage-evidence: kernel:time:KeQuerySystemTime`. A tag is a stable, reviewable link from a test to an operation identifier. It makes discovery reliable, but it still does not change a classification. A tag naming an unknown identifier is reported as a broken test reference.

## Conflicts

If the source declares a limitation (for example `desc.partial = true` or `ExportRequirement::Stubbed`), or the scan finds no implementation, while the manifest claims `verified` or `implemented_unverified`, the conflict is reported as `classification_conflict`. The reviewed state is kept until a human decides.

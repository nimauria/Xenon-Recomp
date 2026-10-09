import copy
import io
import json
import os
import re
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path
from unittest.mock import patch

import audit
import collect_ci_evidence
import evidence
import generate
import refresh

SVG_NS = "{http://www.w3.org/2000/svg}"
COMMIT = "a" * 40
OTHER = "b" * 40


def passing_evidence(targets, commit=COMMIT, platforms=("linux", "windows")):
    return {"schema": 1, "platforms": {platform: {"commit": commit, "run_id": 1, "url": "u", "source": "junit",
                                                  "conclusion": "success",
                                                  "tests": {target: "passed" for target in targets}}
                                       for platform in platforms}}


class Base(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.items = generate.inventory()
        cls.approved = generate.load_manifest(generate.MANIFEST, cls.items)
        cls.targets = sorted({row["test_target"] for row in cls.approved.values() if row.get("test_target")})
        cls.model = generate.build(ci_evidence=passing_evidence(cls.targets), prior={})
        content = generate.snapshot(cls.model)
        cls.snapshot = dict(content, metadata={"assessed_commit": COMMIT, "worktree": "clean", "ci": cls.model.ci})

    def manifest_file(self, rows, directory):
        path = Path(directory) / "coverage.json"
        path.write_text(json.dumps({"schema": 1, "entries": rows}), encoding="utf-8")
        return path

    def patched_source(self, path, transform):
        original = generate.source

        def changed(name):
            data = original(name)
            return transform(data) if name == path else data
        return patch.object(generate, "source", side_effect=changed)


class InventoryChangeTests(Base):
    def test_added_and_removed_entries(self):
        with self.patched_source("src/cpu/ppc/decoder/opcode_catalog.inc",
                                 lambda data: data.replace('X(0x38000000u, "addi", D, Integer, General)\n', "")
                                 + 'X(0x12345678u, "audit_new_opcode", D, Integer, General)\n'):
            model = generate.build(generate.MANIFEST, passing_evidence(self.targets), {}, strict=False)
        report = audit.diff(self.snapshot, dict(generate.snapshot(model)))
        self.assertIn("ppc:audit_new_opcode", report["added"])
        self.assertIn("ppc:addi", report["removed"])
        self.assertIn({"id": "ppc:addi", "now": "removed from source inventory"}, report["lost_verification"])
        self.assertTrue(any(row["kind"] == "unknown_operation" and row["id"] == "ppc:addi" for row in model.problems))
        self.assertTrue(audit.changed(report))

    def test_renamed_entry_is_matched_by_source_key(self):
        with self.patched_source("include/xenon/gpu/types.hpp",
                                 lambda data: data.replace("MemWriteCounter =", "MemWriteCount =")):
            model = generate.build(generate.MANIFEST, passing_evidence(self.targets), {}, strict=False)
        report = audit.diff(self.snapshot, generate.snapshot(model))
        self.assertEqual([row["from"] for row in report["renamed"]], ["pm4:type3:MemWriteCounter"])
        self.assertEqual(report["renamed"][0]["to"], "pm4:type3:MemWriteCount")
        self.assertEqual(report["added"], [])
        self.assertEqual(report["removed"], [])

    def test_changed_implementation_path(self):
        current = copy.deepcopy(self.snapshot)
        record = next(row for row in current["operations"] if row["id"] == "ppc:addi")
        record["signals"]["lifter"] = ["src/cpu/ppc/lifter/moved.cpp"]
        moved = next(row for row in current["operations"] if row["id"] == "kernel:registered:KeTlsAlloc")
        moved["source"] = "src/xbox/exports/xboxkrnl_new_home.cpp"
        report = audit.diff(self.snapshot, current)
        self.assertEqual(report["implementation_changed"], ["kernel:registered:KeTlsAlloc", "ppc:addi"])
        self.assertEqual(report["added"] + report["removed"], [])

    def test_new_and_altered_stubs(self):
        with self.patched_source("src/xbox/exports/xboxkrnl_time_exports.cpp",
                                 lambda data: data.replace('{0x0A8u, "KeStallExecutionProcessor", &ke_stall_execution_processor}',
                                                           '{0x0A8u, "KeStallExecutionProcessor", &ke_stall_execution_processor, true, "audit"}')):
            model = generate.build(generate.MANIFEST, passing_evidence(self.targets), {}, strict=False)
        report = audit.diff(self.snapshot, generate.snapshot(model))
        self.assertEqual([row["id"] for row in report["stub_changes"]], ["kernel:time:KeStallExecutionProcessor"])
        conflict = [row for row in model.problems if row["kind"] == "classification_conflict"]
        self.assertEqual([row["id"] for row in conflict], ["kernel:time:KeStallExecutionProcessor"])
        # The reviewed classification is not changed by the source declaration.
        self.assertEqual(model.entries["kernel:time:KeStallExecutionProcessor"]["state"], "verified")

    def test_changed_test_evidence_and_lost_verification(self):
        failing = passing_evidence(self.targets)
        failing["platforms"]["windows"]["tests"]["xenon_tls_export_tests"] = "timeout"
        model = generate.build(generate.MANIFEST, failing, {}, strict=False)
        report = audit.diff(self.snapshot, generate.snapshot(model))
        lost = {row["id"] for row in report["lost_verification"]}
        self.assertEqual(lost, {f"kernel:registered:KeTls{name}" for name in ("Alloc", "Free", "GetValue", "SetValue")})
        self.assertTrue(lost <= set(report["test_evidence_changed"]))
        for ident in lost:
            self.assertEqual(model.entries[ident]["state"], "implemented_unverified")

    def test_source_signals(self):
        signals = self.model.signals
        self.assertEqual(signals["kernel:time:KeDelayExecutionThread"]["declared"], "partial")
        self.assertEqual(signals["kernel:crypto:XeKeysConsolePrivateKeySign"]["declared"], "stubbed+partial")
        self.assertNotIn("declared", signals["kernel:registered:_snprintf"])
        self.assertIn("src/cpu/ppc/lifter/integer.cpp", signals["ppc:addi"]["lifter"])
        self.assertEqual(signals["shader:texture:16"]["lowering"], "rejected")
        self.assertEqual(signals["shader:texture:17"]["lowering"], "conditional")
        self.assertEqual(signals["shader:vector:0"]["lowering"], "branch")
        self.assertEqual(signals["pm4:type3:MemWrite"]["handler"], "dedicated")
        self.assertEqual(signals["pm4:type3:MemWriteCounter"]["handler"], "passthrough")


class ManifestValidationTests(Base):
    def test_broken_test_reference_is_reported_and_demoted(self):
        row = dict(self.approved["ppc:addi"], test_token="smoke_missing_test")
        with tempfile.TemporaryDirectory() as directory:
            path = self.manifest_file([row], directory)
            with self.assertRaisesRegex(generate.CoverageError, "test token absent"):
                generate.validate_manifest(path, self.items)
            entries, problems = generate.validate_manifest(path, self.items, strict=False)
        self.assertEqual(entries["ppc:addi"]["state"], "implemented_unverified")
        self.assertNotIn("test", entries["ppc:addi"])
        self.assertEqual([(row["kind"], row["id"]) for row in problems], [("broken_test_reference", "ppc:addi")])

    def test_missing_test_file_and_unknown_tag(self):
        row = dict(self.approved["ppc:addi"], test="tests/cpu/removed_test.cpp")
        with tempfile.TemporaryDirectory() as directory:
            entries, problems = generate.validate_manifest(self.manifest_file([row], directory), self.items, strict=False)
        self.assertEqual(problems[0]["kind"], "broken_test_reference")
        targets = {"xenon_fake_tests": {"tests/fake.cpp"}}
        text = "// coverage-evidence: ppc:addi\n// coverage-evidence: ppc:not_an_opcode\nvoid test_x() {}\n"
        discovered, tagged, tag_problems = evidence.discover_tests(self.items, targets, lambda path: text)
        self.assertEqual(tagged, {"ppc:addi": ["xenon_fake_tests"]})
        self.assertEqual([(row["kind"], row["id"]) for row in tag_problems],
                         [("broken_test_reference", "ppc:not_an_opcode")])

    def test_duplicate_identifiers(self):
        row = self.approved["ppc:addi"]
        with tempfile.TemporaryDirectory() as directory:
            path = self.manifest_file([row, row], directory)
            with self.assertRaisesRegex(generate.CoverageError, "duplicate coverage identifier"):
                generate.validate_manifest(path, self.items)
            entries, problems = generate.validate_manifest(path, self.items, strict=False)
        self.assertEqual(list(entries), ["ppc:addi"])
        self.assertEqual(problems[0]["kind"], "duplicate_identifier")
        with self.assertRaisesRegex(generate.CoverageError, "duplicate source inventory identifiers"):
            with patch.object(generate, "enum_values", side_effect=lambda path, enum: [("Same", 0), ("Same", 1)]):
                generate.inventory()

    def test_invalid_manifests(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "coverage.json"
            for text in ("{", json.dumps({"schema": 1}), json.dumps({"schema": 1, "entries": [], "extra": 1}),
                         json.dumps([1, 2])):
                path.write_text(text, encoding="utf-8")
                with self.subTest(text=text):
                    for strict in (True, False):
                        with self.assertRaises(generate.CoverageError):
                            generate.validate_manifest(path, self.items, strict=strict)
            entries, problems = generate.validate_manifest(
                self.manifest_file([{"id": "ppc:addi", "state": "stub", "implementation": "src/cpu/ppc/lifter/integer.cpp"},
                                    {"state": "verified"}], directory), self.items, strict=False)
        self.assertEqual(entries, {})
        self.assertEqual(sorted(row["kind"] for row in problems), ["invalid_manifest_entry", "invalid_manifest_entry"])


class PromotionTests(Base):
    def test_verified_requires_a_recorded_passing_result(self):
        model = generate.build(generate.MANIFEST, None, {}, strict=True)
        self.assertEqual(generate.counts(model.items, model.entries)["verified"], 0)
        self.assertEqual(generate.counts(model.items, model.approved)["verified"],
                         generate.counts(self.items, self.approved)["verified"])
        kinds = {row["kind"] for row in model.problems if row.get("id") in model.approved}
        self.assertEqual(kinds, {"ci_missing"})

    def test_passing_executables_do_not_promote_unreviewed_operations(self):
        everything = passing_evidence(sorted(generate.ctest_targets()))
        model = generate.build(generate.MANIFEST, everything, {}, strict=True)
        self.assertEqual(generate.counts(model.items, model.entries), generate.counts(self.items, self.model.entries))
        for ident, row in model.entries.items():
            self.assertEqual(row["state"], self.approved[ident]["state"])
        self.assertEqual(generate.counts(model.items, model.entries)["unassessed"],
                         len(model.items) - len(self.approved))
        self.assertTrue(any(row["kind"] == "test_evidence_found" for row in model.candidates))

    def test_candidates_never_change_classification(self):
        unassessed = {row["id"] for row in self.model.records if row["manifest_state"] == "unassessed"}
        for row in self.model.candidates:
            if row["kind"] != "verification_candidate":
                self.assertIn(row["id"], unassessed)
        for record in self.model.records:
            if record["manifest_state"] == "unassessed":
                self.assertEqual(record["state"], "unassessed")

    def test_partial_with_passing_test_stays_partial(self):
        self.assertEqual(self.model.entries["kernel:time:KeDelayExecutionThread"]["state"], "partial")
        self.assertEqual(self.model.entries["kernel:registered:RtlFreeAnsiString"]["state"], "implemented_unverified")

    def test_failure_is_a_validation_problem_not_unimplemented(self):
        failing = passing_evidence(self.targets)
        failing["platforms"]["linux"]["tests"]["xenon_cpu_native_smoke"] = "failed"
        model = generate.build(generate.MANIFEST, failing, {}, strict=True)
        self.assertEqual(model.entries["ppc:addi"]["state"], "implemented_unverified")
        self.assertTrue(any(row["kind"] == "ci_failed" and row["id"] == "ppc:addi" for row in model.problems))

    def test_proposal_cannot_touch_the_manifest(self):
        self.assertNotIn("tools/coverage/coverage.json", refresh.ALLOWED_PATHS)
        self.assertTrue(all(path.startswith("docs/coverage/") for path in refresh.ALLOWED_PATHS))


class CiEvidenceTests(Base):
    def test_junit_and_log_parsing(self):
        junit = ('<testsuite><testcase name="a" status="run"/><testcase name="b" status="fail">'
                 '<failure message="Timeout"/></testcase><testcase name="c" status="fail"><failure message="Failed"/>'
                 '</testcase><testcase name="d" status="notrun"><skipped/></testcase></testsuite>')
        self.assertEqual(evidence.parse_junit(junit), {"a": "passed", "b": "timeout", "c": "failed", "d": "not_run"})
        with self.assertRaises(ValueError):
            evidence.parse_junit("<testsuite/>")
        log = ("2026-10-09T17:00:13Z   1/3 Test   #1: xenon_a .....   Passed    0.01 sec\n"
               "2026-10-09T17:07:17Z \x1b[31m 2/3 Test  #50: xenon_b ......***Failed    6.70 sec\x1b[0m\n"
               "2026-10-09T17:23:08Z 3/3 Test #101: xenon_c ...***Timeout 900.01 sec\n"
               "      Start  12: xenon_d\n")
        self.assertEqual(evidence.parse_ctest_log(log), {"xenon_a": "passed", "xenon_b": "failed", "xenon_c": "timeout"})

    def test_missing_platform_results_carry_forward_without_inventing_passes(self):
        prior = {"metadata": {"ci": {"windows": {"commit": OTHER, "source": "log"}}},
                 "test_results": {"t": {"windows": "passed", "linux": "passed"}}}
        fresh = {"schema": 1, "platforms": {"linux": {"unavailable": "pending"}, "windows": {"unavailable": "api error"}}}
        results, ci, problems = evidence.resolve_results({"t", "u"}, fresh, prior)
        self.assertEqual(results, {"t": {"windows": "passed"}, "u": {}})
        self.assertEqual(ci, {"windows": {"commit": OTHER, "source": "log"}})
        self.assertEqual([row["kind"] for row in problems], ["ci_unavailable", "ci_unavailable"])
        self.assertEqual(evidence.verification_outcome("u", results)[:2], (False, "ci_missing"))
        self.assertEqual(evidence.verification_outcome("t", results)[0], True)

    def test_absent_and_not_run_results(self):
        self.assertEqual(evidence.verification_outcome("t", {"t": {"linux": "absent", "windows": "passed"}})[0], True)
        self.assertEqual(evidence.verification_outcome("t", {"t": {"linux": "absent"}})[1], "ci_missing")
        self.assertEqual(evidence.verification_outcome("t", {"t": {"linux": "not_run", "windows": "passed"}})[1], "ci_not_run")

    def test_stale_evidence_is_reported(self):
        metadata = {"assessed_commit": COMMIT, "ci": {"linux": {"commit": OTHER}, "windows": {"commit": COMMIT}}}
        problems = generate.metadata_problems(metadata)
        self.assertEqual([(row["kind"], row["detail"][:5]) for row in problems], [("ci_stale", "linux")])
        self.assertEqual(generate.metadata_problems({"assessed_commit": COMMIT, "ci": {}})[0]["kind"], "ci_missing")

    def test_collector_waits_then_falls_back_to_newest_completed_run(self):
        api = FakeActions()
        api.runs["linux.yml"] = [{"id": 5, "head_sha": COMMIT, "head_branch": "dev", "status": "in_progress"},
                                 {"id": 4, "head_sha": OTHER, "head_branch": "dev", "status": "completed",
                                  "conclusion": "success"}]
        api.runs["windows.yml"] = [{"id": 7, "head_sha": COMMIT, "head_branch": "dev", "status": "completed",
                                    "conclusion": "failure"}]
        clock = iter(range(0, 10000, 70))
        data = collect_ci_evidence.collect(api, "dev", COMMIT, wait_seconds=120, sleep=lambda seconds: None,
                                           clock=lambda: next(clock))
        self.assertEqual(data["platforms"]["linux"]["commit"], OTHER)
        self.assertIn("pending", data["platforms"]["linux"]["selection"])
        self.assertEqual(data["platforms"]["linux"]["tests"], {"xenon_a": "passed"})
        self.assertEqual(data["platforms"]["windows"]["source"], "junit")
        self.assertEqual(data["platforms"]["windows"]["tests"], {"xenon_a": "failed"})
        self.assertGreaterEqual(api.listings, 4)


class FakeActions:
    repository = "owner/repo"

    def __init__(self):
        self.runs = {}
        self.listings = 0

    def get(self, path, **query):
        match = re.search(r"workflows/(\w+\.yml)/runs", path)
        if match:
            self.listings += 1
            runs = [run for run in self.runs.get(match.group(1), [])
                    if run["head_branch"] == query["branch"] and run["head_sha"] == query.get("head_sha", run["head_sha"])]
            return {"workflow_runs": runs}
        run_id = int(re.search(r"runs/(\d+)/", path).group(1))
        if path.endswith("/artifacts"):
            return {"artifacts": [{"id": 70, "name": "ctest-results-windows"}] if run_id == 7 else []}
        return {"jobs": [{"id": run_id * 10, "name": "linux" if run_id < 6 else "windows"}]}

    def download(self, path):
        if path.endswith("/zip"):
            buffer = io.BytesIO()
            with zipfile.ZipFile(buffer, "w") as archive:
                archive.writestr("ctest-results.xml", '<testsuite><testcase name="xenon_a"><failure message="x"/></testcase></testsuite>')
            return buffer.getvalue()
        return b"1/1 Test #1: xenon_a ....   Passed 0.1 sec\n"


class DeterminismTests(Base):
    def test_deterministic_generation(self):
        first = generate.generate(generate.MANIFEST, passing_evidence(self.targets), COMMIT, prior={})[2]
        second = generate.generate(generate.MANIFEST, passing_evidence(self.targets), COMMIT, prior={})[2]
        self.assertEqual(first, second)
        json.loads(first[generate.INVENTORY_JSON])
        json.loads(first[generate.CANDIDATES_JSON])

    def test_no_change_update_keeps_metadata_and_bytes(self):
        _, metadata, outputs = generate.generate(generate.MANIFEST, passing_evidence(self.targets), COMMIT, prior={})
        prior = json.loads(outputs[generate.INVENTORY_JSON])
        self.assertEqual(metadata["assessed_commit"], COMMIT)
        later = passing_evidence(self.targets, commit=OTHER)
        _, again, repeated = generate.generate(generate.MANIFEST, later, OTHER, prior=prior)
        self.assertEqual(again["assessed_commit"], COMMIT)
        self.assertEqual(outputs, repeated)

    def test_meaningful_change_renews_metadata(self):
        _, _, outputs = generate.generate(generate.MANIFEST, passing_evidence(self.targets), COMMIT, prior={})
        prior = json.loads(outputs[generate.INVENTORY_JSON])
        failing = passing_evidence(self.targets, commit=OTHER)
        failing["platforms"]["linux"]["tests"]["xenon_gpu_frontend_tests"] = "failed"
        _, metadata, changed = generate.generate(generate.MANIFEST, failing, OTHER, prior=prior)
        self.assertEqual(metadata["assessed_commit"], OTHER)
        self.assertNotEqual(outputs[generate.SUMMARY_SVG], changed[generate.SUMMARY_SVG])

    def test_fresh_evidence_resolves_stale_metadata(self):
        stale = passing_evidence(self.targets, commit=OTHER, platforms=("linux",))
        stale["platforms"]["windows"] = passing_evidence(self.targets)["platforms"]["windows"]
        _, metadata, outputs = generate.generate(generate.MANIFEST, stale, COMMIT, prior={})
        self.assertTrue(generate.metadata_problems(metadata))
        prior = json.loads(outputs[generate.INVENTORY_JSON])
        _, renewed, _ = generate.generate(generate.MANIFEST, passing_evidence(self.targets, commit="c" * 40),
                                          "c" * 40, prior=prior)
        self.assertEqual(renewed["assessed_commit"], "c" * 40)
        self.assertEqual(generate.metadata_problems(renewed), [])

    def test_metadata_only_drift_does_not_block_ci(self):
        _, _, outputs = generate.generate(generate.MANIFEST, passing_evidence(self.targets), COMMIT, prior={})
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            mapping = {path: root / path.name for path in outputs}
            for path, target in mapping.items():
                target.write_text(outputs[path].replace(COMMIT[:7], "1234567").replace(COMMIT[:12], "123456789012"),
                                  encoding="utf-8")
            relocated = {mapping[path]: text for path, text in outputs.items()}
            with patch.object(generate, "ROOT", root), \
                    patch.object(generate, "DASHBOARD", tuple(mapping[path] for path in generate.DASHBOARD)):
                blocking, refreshable = generate.classify_drift(relocated)
                self.assertEqual(blocking, [])
                self.assertIn("summary.svg", refreshable)
                mapping[generate.SUMMARY_SVG].write_text(outputs[generate.SUMMARY_SVG].replace("verified", "hidden"),
                                                         encoding="utf-8")
                self.assertEqual(generate.classify_drift(relocated)[0], ["summary.svg"])


class ConsistencyTests(Base):
    def test_committed_svgs_reports_and_inventory_agree(self):
        inventory = json.loads(generate.INVENTORY_JSON.read_text(encoding="utf-8"))
        candidates = json.loads(generate.CANDIDATES_JSON.read_text(encoding="utf-8"))
        self.assertEqual(inventory["content_digest"], candidates["content_digest"])
        self.assertEqual(len(inventory["operations"]), len(self.items))
        counts = inventory["counts"]
        total = {state: sum(row[state] for row in counts.values()) for state in generate.STATE_ORDER}
        listed = sum(row["listed"] for row in counts.values())
        summary = generate.SUMMARY_SVG.read_text(encoding="utf-8")
        self.assertIn(f"{total['verified']}/{listed} verified by tests", summary)
        self.assertIn(f"{listed - total['unassessed']}/{listed} classified", summary)
        commit = inventory["metadata"]["assessed_commit"]
        self.assertIn(f"assessed at {commit[:7]}", summary)
        report = generate.REPORT.read_text(encoding="utf-8")
        for category, row in counts.items():
            self.assertIn(f"{row['listed']} listed  ·  {row['classified']} classified  ·  {row['verified']} verified", summary)
            chips = [f">{row[state]} {generate.CHIP_LABELS[state]}<" for state in generate.STATE_ORDER]
            for chip in chips:
                self.assertIn(chip, summary)
        table = [line for line in report.splitlines() if line.startswith("| ") and line[2:4] in {"Ke", "PP", "Sh", "PM"}]
        self.assertEqual(len(table), len(generate.CATEGORIES))
        for line, row in zip(table, (counts[category] for category in generate.CATEGORIES)):
            cells = [cell.strip() for cell in line.strip("|").split("|")]
            self.assertEqual(int(cells[1]), row["listed"])
            self.assertEqual(int(cells[2]), row["classified"])
            self.assertTrue(cells[3].startswith(f"{row['verified']} "))
            self.assertEqual([int(cell) for cell in cells[4:]],
                             [row[state] for state in ("implemented_unverified", "partial", "stub", "unimplemented", "unassessed")])
        cells = ET.fromstring(generate.SVG.read_text(encoding="utf-8")).iter(SVG_NS + "rect")
        states = [rect.find(SVG_NS + "title").text.split(": ")[1].split(";")[0] for rect in cells
                  if rect.find(SVG_NS + "title") is not None]
        self.assertEqual({state: states.count(state) for state in generate.STATE_ORDER}, total)
        recorded = {row["id"]: row["state"] for row in inventory["operations"]}
        self.assertEqual({state: list(recorded.values()).count(state) for state in generate.STATE_ORDER}, total)


class TargetTests(unittest.TestCase):
    def test_scheduled_and_manual_runs_target_the_development_branch(self):
        for event, ref in (("schedule", "refs/heads/main"), ("workflow_dispatch", "refs/heads/main"),
                           ("workflow_dispatch", "refs/heads/development-restructure")):
            with self.subTest(event=event, ref=ref):
                self.assertEqual(refresh.resolve_target(event, ref),
                                 {"branch": "development-restructure", "pin_event_sha": False})
                self.assertEqual(refresh.checkout_ref(event, ref, COMMIT), "refs/heads/development-restructure")
        self.assertEqual(refresh.checkout_ref("push", "refs/heads/development-restructure", COMMIT), COMMIT)
        for event, ref in (("push", "refs/heads/main"), ("pull_request", "refs/pull/1/merge"),
                           ("pull_request_target", "refs/heads/main"), ("workflow_run", "refs/heads/main")):
            with self.subTest(event=event), self.assertRaises(refresh.RefreshError):
                refresh.resolve_target(event, ref)
        self.assertEqual(refresh.main(["target", "--event", "schedule", "--ref", "refs/heads/main", "--sha", OTHER,
                                       "--head", COMMIT]), 0)
        self.assertEqual(refresh.main(["target", "--event", "push", "--ref", "refs/heads/development-restructure",
                                       "--sha", OTHER, "--head", COMMIT]), 1)

    def test_workflow_triggers_permissions_and_checkout(self):
        text = (generate.ROOT / ".github/workflows/coverage-refresh.yml").read_text(encoding="utf-8")
        self.assertIn("COVERAGE_TARGET_BRANCH: development-restructure", text)
        self.assertRegex(text, r"schedule:\n(?:\s*#.*\n)*\s*- cron: \"[^\"]+\"")
        self.assertIn("workflow_dispatch:", text)
        self.assertIn("    branches:\n      - development-restructure\n", text)
        self.assertNotRegex(text, r"\bpull_request(_target)?:")
        self.assertIn("permissions: {}", text)
        self.assertIn("ref: ${{ github.event_name == 'push' && github.sha || format('refs/heads/{0}', env.COVERAGE_TARGET_BRANCH) }}", text)
        audit_job, propose_job = text.split("\n  propose:\n")
        self.assertIn("contents: read", audit_job)
        self.assertNotIn("contents: write", audit_job)
        self.assertIn("persist-credentials: false", audit_job)
        self.assertIn("contents: write", propose_job)
        self.assertIn("needs.audit.outputs.valid == 'true'", propose_job)
        self.assertNotIn("--force", text)
        for workflow in ("windows.yml", "linux.yml", "ci.yml"):
            body = (generate.ROOT / ".github/workflows" / workflow).read_text(encoding="utf-8")
            self.assertNotIn("coverage-refresh", body)
            if workflow != "ci.yml":
                self.assertIn(f"name: ctest-results-{workflow[:-4]}", body)
                self.assertIn("--timeout 900", body)


def git_env(name="dev", email="dev@example.com"):
    return dict(os.environ, GIT_AUTHOR_NAME=name, GIT_AUTHOR_EMAIL=email, GIT_COMMITTER_NAME=name,
                GIT_COMMITTER_EMAIL=email, GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")


def sh(cwd, *args, env=None):
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True,
                          env=env or git_env()).stdout.strip()


class FakePulls:
    repository = "owner/repo"

    def __init__(self):
        self.pulls = []
        self.writes = []

    def get(self, path, **query):
        return [pull for pull in self.pulls if pull["state"] == "open" and query["head"] == "owner:" + pull["head"]]

    def post(self, path, body):
        self.writes.append(("POST", path, body))
        if path.endswith("/pulls"):
            pull = {"number": len(self.pulls) + 1, "state": "open", "head": body["head"], "body": body["body"],
                    "title": body["title"]}
            self.pulls.append(pull)
            return pull
        return {}

    def patch(self, path, body):
        self.writes.append(("PATCH", path, body))
        number = int(path.rsplit("/", 1)[1])
        self.pulls[number - 1].update(body)
        return self.pulls[number - 1]


class ProposalTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        root = Path(self.temporary.name)
        self.origin, self.work = root / "origin.git", root / "work"
        subprocess.run(["git", "init", "--quiet", "--bare", str(self.origin)], check=True, env=git_env())
        subprocess.run(["git", "init", "--quiet", "-b", "development-restructure", str(self.work)], check=True,
                       env=git_env())
        for path in refresh.ALLOWED_PATHS:
            target = self.work / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(f"{path} v1\n", encoding="utf-8")
        (self.work / "tools/coverage").mkdir(parents=True)
        (self.work / "tools/coverage/coverage.json").write_text("{}\n", encoding="utf-8")
        sh(self.work, "add", ".")
        sh(self.work, "commit", "--quiet", "-m", "initial")
        sh(self.work, "remote", "add", "origin", str(self.origin))
        sh(self.work, "push", "--quiet", "origin", "development-restructure")
        self.base = sh(self.work, "rev-parse", "HEAD")
        self.git = refresh.Git(self.work)
        self.api = FakePulls()

    def tearDown(self):
        self.temporary.cleanup()

    def make_patch(self, text="v2", extra=None):
        (self.work / "docs/coverage/summary.svg").write_text(f"summary {text}\n", encoding="utf-8")
        if extra:
            (self.work / extra).write_text("changed\n", encoding="utf-8")
        out = Path(self.temporary.name) / f"proposal-{text}.patch"
        try:
            refresh.make_patch(self.git, out)
        finally:
            sh(self.work, "reset", "--quiet", "--hard", "HEAD")
            sh(self.work, "clean", "-fdq")
        return out

    def pushes(self):
        return [command for command in self.git.log if command[1] == "push"]

    def test_pr_update_idempotency(self):
        proposal = self.make_patch()
        first = refresh.propose(self.git, self.api, proposal, "body v2", "development-restructure", self.base)
        self.assertIn("opened pull request #1", first)
        self.assertEqual(len(self.pushes()), 1)
        bot_head = sh(self.origin, "rev-parse", "refs/heads/bot/coverage-refresh")
        self.assertEqual(sh(self.origin, "log", "-1", "--format=%ae", bot_head), refresh.BOT_EMAIL)
        changed = sh(self.origin, "diff", "--name-only", self.base, bot_head).splitlines()
        self.assertEqual(changed, ["docs/coverage/summary.svg"])
        second = refresh.propose(self.git, self.api, proposal, "body v2", "development-restructure", self.base)
        self.assertIn("already has these generated files", second)
        self.assertIn("already current", second)
        self.assertEqual(len(self.pushes()), 1)
        self.assertEqual(sh(self.origin, "rev-parse", "refs/heads/bot/coverage-refresh"), bot_head)
        self.assertEqual([write[0] for write in self.api.writes], ["POST"])
        updated = refresh.propose(self.git, self.api, self.make_patch("v3"), "body v3", "development-restructure", self.base)
        self.assertIn("updated pull request #1", updated)
        new_head = sh(self.origin, "rev-parse", "refs/heads/bot/coverage-refresh")
        self.assertEqual(sh(self.origin, "rev-parse", f"{new_head}^1"), bot_head)  # fast-forward, no rewrite
        self.assertFalse(any("--force" in command or any(arg.startswith("+") for arg in command[2:])
                             for command in self.pushes()))

    def test_developer_commits_on_the_bot_branch_are_never_overwritten(self):
        refresh.propose(self.git, self.api, self.make_patch(), "body", "development-restructure", self.base)
        sh(self.work, "fetch", "--quiet", "origin", "bot/coverage-refresh")
        sh(self.work, "checkout", "--quiet", "-B", "bot-local", "FETCH_HEAD")
        (self.work / "docs/coverage/AUDIT.md").write_text("hand edit\n", encoding="utf-8")
        sh(self.work, "commit", "--quiet", "-am", "developer fix")
        sh(self.work, "push", "--quiet", "origin", "HEAD:refs/heads/bot/coverage-refresh")
        developer_head = sh(self.work, "rev-parse", "HEAD")
        outcome = refresh.propose(self.git, self.api, self.make_patch("v4"), "body v4", "development-restructure",
                                  self.base)
        self.assertIn("left bot/coverage-refresh untouched", outcome)
        self.assertEqual(sh(self.origin, "rev-parse", "refs/heads/bot/coverage-refresh"), developer_head)
        self.assertEqual(self.api.pulls[0]["body"], "body")

    def test_proposals_are_limited_to_generated_files(self):
        with self.assertRaisesRegex(refresh.RefreshError, "outside the generated coverage outputs"):
            self.make_patch(extra="tools/coverage/coverage.json")
        bad = Path(self.temporary.name) / "bad.patch"
        (self.work / "tools/coverage/coverage.json").write_text('{"forged": true}\n', encoding="utf-8")
        bad.write_text(sh(self.work, "diff", "--binary") + "\n", encoding="utf-8")
        sh(self.work, "checkout", "--", ".")
        with self.assertRaisesRegex(refresh.RefreshError, "may only change generated coverage files"):
            refresh.propose(self.git, self.api, bad, "body", "development-restructure", self.base)
        self.assertFalse(self.pushes())
        with self.assertRaisesRegex(refresh.RefreshError, "force"):
            self.git("push", "--force", "origin", "HEAD:refs/heads/x")

    def test_retire_closes_an_obsolete_pull_request(self):
        refresh.propose(self.git, self.api, self.make_patch(), "body", "development-restructure", self.base)
        outcome = refresh.propose(self.git, self.api, None, "", "development-restructure", self.base, retire=True)
        self.assertEqual(outcome, "closed obsolete pull request #1")
        self.assertEqual(self.api.pulls[0]["state"], "closed")
        self.assertEqual(refresh.propose(self.git, self.api, None, "", "development-restructure", self.base, retire=True),
                         "nothing to publish")

    def test_no_change_run_produces_no_patch(self):
        out = Path(self.temporary.name) / "empty.patch"
        self.assertEqual(refresh.make_patch(self.git, out), [])
        self.assertEqual(out.read_text(encoding="utf-8"), "")


class RunTests(Base):
    def test_run_reports_without_writing(self):
        with tempfile.TemporaryDirectory() as directory:
            evidence_path = Path(directory) / "ci.json"
            evidence_path.write_text(json.dumps(passing_evidence(self.targets)), encoding="utf-8")
            with patch.dict(os.environ, {"GITHUB_OUTPUT": str(Path(directory) / "out"),
                                         "GITHUB_STEP_SUMMARY": str(Path(directory) / "summary")}):
                status = refresh.run(Path(directory) / "refresh", evidence_path, COMMIT, "development-restructure",
                                     write=False)
            self.assertTrue(status["valid"])
            body = (Path(directory) / "refresh/pr-body.md").read_text(encoding="utf-8")
            self.assertTrue(body.startswith(refresh.MARKER))
            self.assertIn("No classification was changed", body)
            self.assertIn("valid=true", (Path(directory) / "out").read_text(encoding="utf-8"))
            report = json.loads((Path(directory) / "refresh/diff.json").read_text(encoding="utf-8"))
            self.assertIn("lost_verification", report)


if __name__ == "__main__":
    unittest.main()

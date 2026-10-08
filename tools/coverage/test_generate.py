import json
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest.mock import patch

import check_ci_platforms
import check_ctest
import generate


class CoverageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.items = generate.inventory()
        cls.entries = generate.load_manifest(generate.MANIFEST, cls.items)

    def temporary_manifest(self, rows):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "coverage.json"
            path.write_text(json.dumps({"schema": 1, "entries": rows}), encoding="utf-8")
            return generate.load_manifest(path, self.items)

    def test_source_inventory_and_manifest(self):
        self.assertGreater(len(self.items), 800)
        self.assertEqual(len(self.items), sum(1 for category in ("kernel", "ppc", "shader", "pm4")
                                             for item in self.items if item.category == category))
        self.assertEqual(len(generate.kernel_inventory()), len([item for item in self.items if item.category == "kernel"]))
        self.assertEqual(sum(generate.counts(self.items, self.entries).values()), len(self.items))

    def test_source_inventory_change_affects_denominator(self):
        original = generate.source

        def changed(path):
            data = original(path)
            if path == "src/cpu/ppc/decoder/opcode_catalog.inc":
                return data + '\nX(0x12345678u, "audit_new_opcode", D, Integer, General)\n'
            return data

        with patch.object(generate, "source", side_effect=changed):
            expanded = generate.inventory()
        self.assertEqual(len(expanded), len(self.items) + 1)
        self.assertIn("ppc:audit_new_opcode", {item.id for item in expanded})
        self.assertEqual(generate.counts(expanded, self.entries)["unassessed"],
                         generate.counts(self.items, self.entries)["unassessed"] + 1)

    def test_kernel_diagnostic_metadata_must_match_registration(self):
        original = generate.source

        def altered(path):
            content = original(path)
            if path == "src/xbox/export_metadata.cpp":
                return content.replace('0x012Cu, "RtlInitAnsiString"',
                                       '0x0129u, "RtlInitAnsiString"')
            return content

        with patch.object(generate, "source", side_effect=altered):
            with self.assertRaisesRegex(generate.CoverageError, "diagnostic metadata disagrees"):
                generate.kernel_inventory()

    def test_kernel_source_conflicts_and_audio_symbol_drift(self):
        original = generate.source

        def conflicting(path):
            content = original(path)
            if path == "src/xbox/exports/xboxkrnl_time_exports.cpp":
                return content + '\n{0x0FFFu, "KeQuerySystemTime", nullptr},\n'
            return content

        with patch.object(generate, "source", side_effect=conflicting):
            with self.assertRaisesRegex(generate.CoverageError, "conflicting xboxkrnl ordinals"):
                generate.kernel_inventory()

        def audio_drift(path):
            content = original(path)
            if path == "src/audio/exports.cpp":
                return content.replace('add("XAudioRenderDriverInitialize",',
                                       'add("WrongAudioName",')
            return content

        with patch.object(generate, "source", side_effect=audio_drift):
            with self.assertRaisesRegex(generate.CoverageError, "audio export name/ordinal symbol mismatch"):
                generate.kernel_inventory()

    def test_pinned_kernel_reference_reconciliation(self):
        provenance, rows = generate.load_kernel_reference()
        self.assertEqual(provenance["commit"], "997d0555dbd6358dffd2950097424993763051af")
        self.assertEqual(len(rows), 922)
        self.assertEqual(sum(row["kind"] == "variable" for row in rows), 35)
        self.assertEqual((rows[0]["ordinal"], rows[-1]["ordinal"]), (1, 931))
        result = generate.reconcile_kernel_reference(rows, generate.kernel_registration_rows())
        self.assertEqual(len(result["matched"]), 254)
        self.assertEqual(len(result["missing"]), 668)
        report = generate.render_kernel_reference_report(provenance, rows, result)
        self.assertEqual(report, generate.render_kernel_reference_report(provenance, rows, result))
        self.assertIn("254 exact Xenon source registrations (27.5%)", report)
        self.assertIn("not implementation or game-compatibility percentages", report)

    def test_invalid_kernel_reference_rows(self):
        raw = json.loads(generate.KERNEL_REFERENCE.read_text(encoding="utf-8"))
        for change in (
            lambda data: data["exports"].append(dict(data["exports"][-1])),
            lambda data: data["exports"][1].update(name=data["exports"][0]["name"]),
            lambda data: data["exports"][1].update(kind="unknown"),
            lambda data: data["exports"][1].update(ordinal="2"),
            lambda data: data["exports"].reverse(),
            lambda data: data["provenance"].update(commit="unversioned"),
        ):
            with self.subTest(change=change.__code__.co_firstlineno):
                data = json.loads(json.dumps(raw))
                change(data)
                with tempfile.TemporaryDirectory() as directory:
                    path = Path(directory) / "reference.json"
                    path.write_text(json.dumps(data), encoding="utf-8")
                    with self.assertRaises(generate.CoverageError):
                        generate.load_kernel_reference(path)

    def test_kernel_reference_disagrees_with_registration(self):
        _, rows = generate.load_kernel_reference()
        registration = generate.kernel_registration_rows()
        name = "RtlInitAnsiString"
        ordinal, path = registration[name]
        for changed in ((ordinal + 1, path), (ordinal, "kernel_variables.cpp")):
            with self.subTest(changed=changed):
                altered = dict(registration)
                altered[name] = changed
                with self.assertRaisesRegex(generate.CoverageError, "kernel reference disagreement"):
                    generate.reconcile_kernel_reference(rows, altered)
        altered = dict(registration)
        altered["NonexistentKernelExport"] = (0xFFFF, path)
        with self.assertRaisesRegex(generate.CoverageError, "kernel reference disagreement"):
            generate.reconcile_kernel_reference(rows, altered)

    def test_percentage_and_empty_category(self):
        self.assertEqual(generate.percent(1, 4), "25.0%")
        self.assertEqual(generate.percent(0, 0), "unknown")
        self.assertEqual(generate.counts([], {}), {})
        selected = self.items[:4]
        self.assertEqual(sum(generate.counts(selected, self.entries).values()), len(selected))

    def test_partial_and_unverified_not_counted_as_verified(self):
        chosen = [i for i in self.items if i.id in {
            "kernel:crypto:XeKeysConsolePrivateKeySign",
            "kernel:registered:NtAllocateVirtualMemory", "shader:vector:0"}]
        states = generate.counts(chosen, self.entries)
        self.assertEqual(states["partial"], 1)
        self.assertEqual(states["stub"], 1)
        self.assertEqual(states["implemented_unverified"], 1)
        self.assertEqual(states["verified"], 0)

    def test_missing_identifier(self):
        with self.assertRaisesRegex(generate.CoverageError, "missing from source inventory"):
            self.temporary_manifest([{"id": "ppc:does_not_exist", "state": "verified"}])

    def test_duplicate_identifier(self):
        row = self.entries["ppc:addi"]
        with self.assertRaisesRegex(generate.CoverageError, "duplicate coverage identifier"):
            self.temporary_manifest([row, row])

    def test_invalid_entry_and_missing_test(self):
        row = dict(self.entries["ppc:addi"])
        row["state"] = "magic"
        with self.assertRaisesRegex(generate.CoverageError, "invalid audited state"):
            self.temporary_manifest([row])
        row = dict(self.entries["ppc:addi"])
        row["kind"] = "stub"
        with self.assertRaisesRegex(generate.CoverageError, "invalid implementation kind"):
            self.temporary_manifest([row])
        row = dict(self.entries["ppc:addi"])
        row.pop("test")
        with self.assertRaisesRegex(generate.CoverageError, "lacks test trace"):
            self.temporary_manifest([row])
        row["state"] = []
        with self.assertRaisesRegex(generate.CoverageError, "invalid audited state"):
            self.temporary_manifest([row])

    def test_manifest_schema_test_targets_and_duplicate_evidence(self):
        row = dict(self.entries["ppc:addi"])
        row["test_target"] = "xenon_missing_test"
        with self.assertRaisesRegex(generate.CoverageError, "not registered with CTest"):
            self.temporary_manifest([row])
        row = dict(self.entries["ppc:addi"])
        row["test"] = "tests/cpu/renamed_native_smoke.cpp"
        with self.assertRaisesRegex(generate.CoverageError, "source inventory missing"):
            self.temporary_manifest([row])
        row = dict(self.entries["ppc:addi"])
        row["unexpected"] = True
        with self.assertRaisesRegex(generate.CoverageError, "unknown coverage fields"):
            self.temporary_manifest([row])
        other = dict(self.entries["kernel:registered:KeSetCurrentProcessType"])
        other["test_token"] = self.entries["kernel:registered:KeGetCurrentProcessType"]["test_token"]
        with self.assertRaisesRegex(generate.CoverageError, "duplicate test evidence"):
            self.temporary_manifest([self.entries["kernel:registered:KeGetCurrentProcessType"], other])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.json"
            path.write_text(json.dumps({"schema": 2, "entries": []}))
            with self.assertRaisesRegex(generate.CoverageError, "requires schema 1"):
                generate.load_manifest(path, self.items)
            path.write_text(json.dumps({"schema": True, "entries": []}))
            with self.assertRaisesRegex(generate.CoverageError, "requires schema 1"):
                generate.load_manifest(path, self.items)

    def test_source_trace_validation(self):
        row = dict(self.entries["ppc:addi"])
        row["implementation_token"] = "a token absent from source"
        with self.assertRaisesRegex(generate.CoverageError, "implementation token absent"):
            self.temporary_manifest([row])

    def test_deterministic_svg_and_states(self):
        first = generate.render_svg(self.items, self.entries)
        self.assertEqual(first, generate.render_svg(self.items, self.entries))
        root = ET.fromstring(first)
        self.assertEqual(root.attrib["viewBox"].split()[:3], ["0", "0", "1200"])
        self.assertIn("Unassessed", first)
        self.assertIn("Unimplemented", first)
        self.assertIn(f"{generate.counts(self.items, self.entries)['verified']}/{len(self.items)}", first)
        self.assertEqual(first.count("<title>"), len(self.items))
        compact = generate.render_summary_svg(self.items, self.entries)
        self.assertEqual(compact, generate.render_summary_svg(self.items, self.entries))
        self.assertEqual(ET.fromstring(compact).attrib["viewBox"], "0 0 1200 650")
        self.assertIn("Xbox 360 kernel exports", compact)
        self.assertIn("GPU PM4 packets", compact)
        for svg in (first, compact):
            for rect in ET.fromstring(svg).iter("{http://www.w3.org/2000/svg}rect"):
                self.assertGreater(float(rect.attrib["width"]), 0)
                self.assertGreater(float(rect.attrib["height"]), 0)

    def test_detailed_layout_grows_with_inventory(self):
        extra = [generate.Item(f"ppc:synthetic_{n}", "ppc", "Vector", f"synthetic_{n}", "test")
                 for n in range(2000)]
        root = ET.fromstring(generate.render_svg(self.items + extra, self.entries))
        self.assertGreater(int(root.attrib["viewBox"].split()[-1]),
                           int(ET.fromstring(generate.render_svg(self.items, self.entries)).attrib["viewBox"].split()[-1]))

    def test_ci_references_real_workflow_and_jobs(self):
        workflow = (generate.ROOT / ".github/workflows/ci.yml").read_text()
        windows = (generate.ROOT / ".github/workflows/windows.yml").read_text()
        linux = (generate.ROOT / ".github/workflows/linux.yml").read_text()
        readme = (generate.ROOT / "README.md").read_text()
        self.assertIn("name: Xenon CI", workflow)
        self.assertIn("platform-results:", workflow)
        self.assertIn("coverage-dashboard:", workflow)
        self.assertIn("linux-sanitizers:", workflow)
        self.assertIn("ilammy/msvc-dev-cmd@v1", windows)
        self.assertIn("where.exe cl.exe", windows)
        self.assertIn("ctest --test-dir build/windows-x64-debug", windows)
        self.assertIn("ctest --test-dir build/linux-x64-debug", linux)
        for workflow_file in ("windows.yml", "linux.yml", "ci.yml"):
            self.assertIn(f"actions/workflows/{workflow_file}/badge.svg?branch=development-restructure&amp;event=push".replace("&amp;", "&"), readme)
            self.assertTrue((generate.ROOT / ".github/workflows" / workflow_file).is_file())
        self.assertIn("[![Xenon implementation coverage summary](docs/coverage/summary.svg)](docs/coverage/dashboard.svg)", readme)
        self.assertIn("docs/coverage/REPORT.md", readme)
        self.assertEqual((generate.ROOT / "README.md").read_bytes().count(b"\r\n"),
                         (generate.ROOT / "README.md").read_bytes().count(b"\n"))
        for link in ("docs/coverage/summary.svg", "docs/coverage/dashboard.svg",
                     "docs/coverage/README.md", "docs/coverage/REPORT.md"):
            self.assertTrue((generate.ROOT / link).is_file(), link)
        self.assertTrue(generate.SUMMARY_SVG.is_file())
        self.assertEqual(generate.SUMMARY_SVG.read_text(), generate.render_summary_svg(self.items, self.entries))
        self.assertEqual(generate.SVG.read_text(), generate.render_svg(self.items, self.entries))

    def test_ctest_report_and_platform_gate(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "results.xml"
            path.write_text('<testsuite><testcase name="one"/></testsuite>')
            self.assertEqual(check_ctest.check(path), (1, 0, 0, 0))
            path.write_text('<testsuite><testcase name="one"><skipped/></testcase></testsuite>')
            self.assertEqual(check_ctest.check(path), (1, 0, 0, 1))
            path.write_text('<testsuite/>')
            with self.assertRaisesRegex(ValueError, "no test cases"):
                check_ctest.check(path)
        run = {"workflow_runs": [{"id": 42, "head_sha": "abc", "event": "push",
                                  "status": "completed", "conclusion": "success"}]}
        jobs = {"jobs": [{"name": "windows", "conclusion": "success"}]}
        with patch.object(check_ci_platforms, "get_json", side_effect=[run, jobs]):
            self.assertEqual(check_ci_platforms.workflow_result("o/r", "windows.yml", "windows", "abc", "push", "token"), "success")
        with patch.object(check_ci_platforms, "get_json", return_value={"workflow_runs": []}):
            self.assertIn("pending:", check_ci_platforms.workflow_result("o/r", "windows.yml", "windows", "abc", "push", "token"))
        run["workflow_runs"][0]["status"] = "in_progress"
        with patch.object(check_ci_platforms, "get_json", return_value=run):
            self.assertIn("pending:", check_ci_platforms.workflow_result("o/r", "windows.yml", "windows", "abc", "push", "token"))
        run["workflow_runs"][0]["status"] = "completed"
        jobs["jobs"][0]["conclusion"] = "skipped"
        with patch.object(check_ci_platforms, "get_json", side_effect=[run, jobs]):
            self.assertIn("failed:", check_ci_platforms.workflow_result("o/r", "windows.yml", "windows", "abc", "push", "token"))


if __name__ == "__main__":
    unittest.main()

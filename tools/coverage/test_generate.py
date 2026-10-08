import json
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest.mock import patch

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
        self.assertEqual(len(self.items), 864)
        self.assertEqual(len(self.entries), 25)
        self.assertEqual(len(generate.kernel_inventory()), 254)

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

    def test_partial_and_unverified_not_counted_as_verified(self):
        chosen = [i for i in self.items if i.id in {
            "kernel:crypto:XeKeysConsolePrivateKeySign", "shader:vector:0"}]
        states = generate.counts(chosen, self.entries)
        self.assertEqual(states["partial"], 1)
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
        row["state"] = "verified"
        row.pop("test")
        with self.assertRaisesRegex(generate.CoverageError, "lacks test trace"):
            self.temporary_manifest([row])
        row["state"] = []
        with self.assertRaisesRegex(generate.CoverageError, "invalid audited state"):
            self.temporary_manifest([row])

    def test_source_trace_validation(self):
        row = dict(self.entries["ppc:addi"])
        row["implementation_token"] = "a token absent from source"
        with self.assertRaisesRegex(generate.CoverageError, "implementation token absent"):
            self.temporary_manifest([row])

    def test_deterministic_svg_and_states(self):
        first = generate.render_svg(self.items, self.entries)
        self.assertEqual(first, generate.render_svg(self.items, self.entries))
        self.assertEqual(ET.fromstring(first).attrib["viewBox"], "0 0 1200 1030")
        self.assertIn("Unassessed", first)
        self.assertIn("Unimplemented", first)
        self.assertIn("≥3.9%", first)
        self.assertIn("19/864", first)

    def test_ci_references_real_workflow_and_jobs(self):
        workflow = (generate.ROOT / ".github/workflows/ci.yml").read_text()
        readme = (generate.ROOT / "README.md").read_text()
        self.assertIn("name: Xenon CI", workflow)
        for job in ("linux:", "windows:", "coverage-dashboard:"):
            self.assertIn(job, workflow)
        self.assertIn("ilammy/msvc-dev-cmd@v1", workflow)
        self.assertIn("ctest --test-dir build/windows-x64-debug", workflow)
        self.assertIn("ctest --test-dir build/linux-x64-debug", workflow)
        self.assertIn("actions/workflows/ci.yml/badge.svg?branch=development-restructure", readme)
        self.assertIn("docs/coverage/dashboard.svg", readme)


if __name__ == "__main__":
    unittest.main()

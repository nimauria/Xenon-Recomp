import json
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

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
        self.assertEqual(len(self.items), 616)
        self.assertEqual(len(self.entries), 18)

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
        self.assertIn("≥50.0%", first)
        self.assertIn("12/616", first)

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

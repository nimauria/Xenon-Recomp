"""Test-evidence discovery and CI-result handling for the coverage audit.

Everything here is pure data processing: no network access and no writes.
`collect_ci_evidence.py` fetches CI results; `generate.py` decides how they
affect the dashboard.
"""

from __future__ import annotations

import json
import re
import xml.etree.ElementTree as ET
from pathlib import Path

PLATFORMS = ("linux", "windows")
OUTCOMES = {"passed", "failed", "timeout", "not_run", "absent"}
# A comment such as `// coverage-evidence: kernel:time:KeQuerySystemTime` in a
# registered test source ties that source to a stable operation identifier.
TAG = re.compile(r"coverage-evidence:\s*([a-z0-9]+:[\w:.]+)")
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
LOG_RESULT = re.compile(
    r"Test\s+#\d+:\s+(\S+)\s+\.*\s*(?:\*\*\*)?"
    r"(Passed|Failed|Timeout|Not Run|Skipped|Disabled|Exception|SEGFAULT|Child aborted|\w+)")


def snake(name: str) -> str:
    """KeQuerySystemTime -> ke_query_system_time (test-function naming)."""
    text = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", name.strip("_"))
    return re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1_\2", text).lower()


def test_sources(targets: dict[str, set[str]], read) -> dict[str, list[tuple[str, str]]]:
    """Map each registered test source path to [(target, text)]."""
    by_path: dict[str, list[tuple[str, str]]] = {}
    texts: dict[str, str] = {}
    for target, paths in sorted(targets.items()):
        for path in sorted(paths):
            if path not in texts:
                try:
                    texts[path] = read(path)
                except (OSError, ValueError):
                    continue
            by_path.setdefault(path, []).append((target, texts[path]))
    return by_path


class SourceTokens:
    """Identifiers, string literals and qualified enum names in one test source."""

    def __init__(self, text: str):
        self.words = set(re.findall(r"\b[A-Za-z_]\w*\b", text))
        self.literals = set(re.findall(r'"([^"\n]{1,64})"', text))
        self.qualified = set(re.findall(r"\b((?:ControlFlowOpcode|Type3Opcode|PacketType)::\w+)", text))
        self.prefixed = {word for word in self.words if word.startswith(("test_", "smoke_"))}


def mentions(item, tokens: SourceTokens) -> bool:
    """Whether a test source mentions the operation by an operation-specific spelling."""
    if item.category == "kernel":
        prefix = "test_" + snake(item.name)
        return item.name in tokens.words or any(word.startswith(prefix) for word in tokens.prefixed)
    if item.category == "ppc":
        return item.name in tokens.literals or any(
            word.rstrip("0123456789") == "smoke_" + item.name for word in tokens.prefixed)
    if item.id.startswith("shader:control:"):
        return f"ControlFlowOpcode::{item.name}" in tokens.qualified
    if item.id.startswith("pm4:type3:"):
        return f"Type3Opcode::{item.name}" in tokens.qualified
    if item.id.startswith("pm4:header:"):
        return (f"PacketType::{item.name}" in tokens.qualified or
                f"make_packet_{item.name.lower()}" in tokens.words)
    # Numeric shader ALU/fetch forms have no reliable spelling; only tags count.
    return False


def discover_tests(items, targets: dict[str, set[str]], read) -> tuple[dict, dict, list[dict]]:
    """Return (discovered, tagged, problems).

    `discovered[id]` lists CTest targets whose sources mention the operation by
    one of its operation-specific spellings. `tagged[id]` lists targets whose
    sources carry an explicit `coverage-evidence:` tag. Discovery is advisory:
    it produces audit candidates and never changes a classification.
    """
    sources = test_sources(targets, read)
    known = {item.id for item in items}
    tagged: dict[str, set[str]] = {}
    discovered: dict[str, set[str]] = {}
    problems: list[dict] = []
    for path, owners in sorted(sources.items()):
        text = owners[0][1]
        owner_targets = {target for target, _ in owners}
        for ident in sorted(set(TAG.findall(text))):
            if ident not in known:
                problems.append({"severity": "error", "kind": "broken_test_reference", "id": ident,
                                 "detail": f"{path} tags an operation that is not in the source inventory"})
                continue
            tagged.setdefault(ident, set()).update(owner_targets)
        tokens = SourceTokens(text)
        for item in items:
            if mentions(item, tokens):
                discovered.setdefault(item.id, set()).update(owner_targets)
    return ({key: sorted(value) for key, value in discovered.items()},
            {key: sorted(value) for key, value in tagged.items()}, problems)


def normalize_outcome(word: str) -> str:
    word = word.strip().lower()
    if word == "passed":
        return "passed"
    if word == "timeout":
        return "timeout"
    if word in {"not run", "skipped", "disabled", "notrun"}:
        return "not_run"
    return "failed"


def parse_junit(text: str) -> dict[str, str]:
    """Read a CTest `--output-junit` report into {test name: outcome}."""
    root = ET.fromstring(text)
    results: dict[str, str] = {}
    for case in root.iter("testcase"):
        name = case.get("name")
        if not name:
            continue
        failure = case.find("failure")
        if failure is None:
            failure = case.find("error")
        if failure is not None:
            message = (failure.get("message") or "") + (failure.text or "")
            outcome = "timeout" if "timeout" in message.lower() else "failed"
        elif case.find("skipped") is not None or case.get("status") in {"notrun", "disabled"}:
            outcome = "not_run"
        else:
            outcome = "passed"
        results[name] = worst(results.get(name), outcome)
    if not results:
        raise ValueError("CTest JUnit report has no test cases")
    return results


def parse_ctest_log(text: str) -> dict[str, str]:
    """Read CTest's per-test result lines from a GitHub Actions job log."""
    results: dict[str, str] = {}
    for line in ANSI.sub("", text).splitlines():
        match = LOG_RESULT.search(line)
        if match:
            name, word = match.groups()
            results[name] = worst(results.get(name), normalize_outcome(word))
    return results


def worst(previous: str | None, outcome: str) -> str:
    order = ["passed", "not_run", "timeout", "failed"]
    if previous is None:
        return outcome
    return max(previous, outcome, key=order.index)


def resolve_results(targets: set[str], evidence: dict | None, prior: dict | None) -> tuple[dict, dict, list[dict]]:
    """Combine freshly collected CI evidence with the committed snapshot.

    Returns (results, ci_metadata, problems). `results[target][platform]` is one
    of OUTCOMES. Platforms missing from `evidence` keep the outcomes recorded
    in the committed inventory, so an unavailable API never erases evidence and
    never invents a pass.
    """
    prior = prior or {}
    prior_results = prior.get("test_results", {}) if isinstance(prior, dict) else {}
    prior_ci = (prior.get("metadata") or {}).get("ci", {}) if isinstance(prior, dict) else {}
    results: dict[str, dict[str, str]] = {target: {} for target in sorted(targets)}
    ci: dict[str, dict] = {}
    problems: list[dict] = []
    fresh = (evidence or {}).get("platforms", {}) if isinstance(evidence, dict) else {}
    for platform in PLATFORMS:
        report = fresh.get(platform)
        if isinstance(report, dict) and isinstance(report.get("tests"), dict) and report["tests"]:
            ci[platform] = {key: report[key] for key in ("commit", "run_id", "url", "source", "conclusion")
                            if key in report}
            for target in targets:
                outcome = report["tests"].get(target, "absent")
                results[target][platform] = outcome if outcome in OUTCOMES else "failed"
            continue
        if isinstance(report, dict):
            reason = report.get("unavailable") or "the run reported no CTest results"
            problems.append({"severity": "warning", "kind": "ci_unavailable", "id": None,
                             "detail": f"{platform}: {reason}"})
        if platform in prior_ci:
            ci[platform] = dict(prior_ci[platform])
            for target in targets:
                outcome = prior_results.get(target, {}).get(platform)
                if outcome in OUTCOMES:
                    results[target][platform] = outcome
    return results, ci, problems


def verification_outcome(target: str, results: dict) -> tuple[bool, str, str]:
    """Decide whether a reviewed `verified` entry's required test succeeded.

    The test must have passed on at least one platform whose report contains
    it, and must not have failed, timed out or been skipped on any platform.
    Returns (eligible, problem kind, detail).
    """
    outcomes = {platform: outcome for platform, outcome in results.get(target, {}).items()
                if outcome != "absent"}
    if not outcomes:
        return False, "ci_missing", f"no CI result recorded for CTest target {target}"
    bad = {platform: outcome for platform, outcome in sorted(outcomes.items()) if outcome != "passed"}
    if bad:
        kind = "ci_not_run" if set(bad.values()) == {"not_run"} else "ci_failed"
        detail = ", ".join(f"{platform} {outcome}" for platform, outcome in bad.items())
        return False, kind, f"CTest target {target}: {detail}"
    return True, "", ""


def read_evidence_file(path: Path | None) -> dict | None:
    if path is None:
        return None
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or data.get("schema") != 1 or not isinstance(data.get("platforms"), dict):
        raise ValueError("CI evidence file requires schema 1 and a platforms object")
    return data

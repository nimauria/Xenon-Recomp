"""Audit candidates and revision-to-revision inventory comparison.

Functions here work on the serialized operation records written to
`docs/coverage/inventory.json`, so a committed snapshot and a fresh one are
compared in exactly the same form. Candidates are suggestions for a reviewer;
nothing in this module changes a classification.
"""

from __future__ import annotations

from collections import Counter, defaultdict

AREAS = {"kernel": "Xbox kernel exports", "ppc": "PowerPC instructions",
         "shader": "Xenos shader operations", "pm4": "GPU PM4 commands"}
IMPLEMENTED_CLAIMS = {"verified", "implemented_unverified"}
CANDIDATE_TEXT = {
    "implementation_found": "Source implementation found; review and classify",
    "test_evidence_found": "Test source mentions this operation; review whether it asserts behaviour",
    "declared_limitation": "Source declares a stub or partial limitation; classify accordingly",
    "ir_only": "Parsed into typed IR but has no dedicated execution path",
    "possibly_unimplemented": "No implementation path found by the source scan",
    "verification_candidate": "Implemented and mentioned by a test; may qualify for verified after review",
}


def implementation_signal(record: dict) -> tuple[str | None, str | None]:
    """Return (kind, detail) for what the source scan found for one operation."""
    signals = record.get("signals", {})
    category = record["category"]
    if category == "kernel":
        declared = signals.get("declared")
        if declared:
            return "declared_limitation", f"registration declares {declared}"
        if signals.get("kind") == "variable":
            return "implementation_found", "guest variable export"
        if signals.get("handler"):
            return "implementation_found", f"handler {signals['handler']}"
        return "implementation_found", "registered without a statically visible handler"
    if category == "ppc":
        paths = signals.get("lifter", []) + signals.get("fallback", [])
        if paths:
            return "implementation_found", "mnemonic handled in " + ", ".join(paths)
        family = signals.get("lifter_family", []) + signals.get("fallback_family", [])
        if family:
            return "implementation_found", "mnemonic family prefix handled in " + ", ".join(family)
        return "possibly_unimplemented", "no mnemonic or family-prefix match in lifter or fallback interpreter"
    if category == "shader":
        lowering = signals.get("lowering")
        if lowering in {"branch", "accepted", "referenced", "decoder"}:
            return "implementation_found", f"lowering: {lowering}"
        if lowering == "conditional":
            return "declared_limitation", "lowering rejects this form in some shader stages"
        return "possibly_unimplemented", f"lowering: {lowering or 'none'}"
    handler = signals.get("handler")
    if handler == "dedicated":
        return "implementation_found", "dedicated command-processor execution"
    if handler == "ir_packet":
        return "ir_only", f"emitted as {signals.get('ir', 'typed')} IR only"
    return "possibly_unimplemented", "retained as raw type-3 passthrough"


def candidates(records: list[dict]) -> list[dict]:
    rows = []
    for record in records:
        ident, manifest_state = record["id"], record.get("manifest_state", "unassessed")
        tests = sorted(set(record.get("discovered_tests", [])) | set(record.get("tagged_tests", [])))
        if manifest_state == "unassessed":
            kind, detail = implementation_signal(record)
            if kind:
                rows.append({"id": ident, "category": record["category"], "kind": kind, "detail": detail})
            if tests:
                rows.append({"id": ident, "category": record["category"], "kind": "test_evidence_found",
                             "detail": "mentioned by " + ", ".join(tests)})
        elif manifest_state == "implemented_unverified" and tests and not record.get("evidence", {}).get("test"):
            rows.append({"id": ident, "category": record["category"], "kind": "verification_candidate",
                         "detail": "mentioned by " + ", ".join(tests)})
    return sorted(rows, key=lambda row: (row["category"], row["kind"], row["id"]))


def conflicts(records: list[dict]) -> list[dict]:
    """Reviewed classifications that the current source scan contradicts."""
    rows = []
    for record in records:
        manifest_state = record.get("manifest_state", "unassessed")
        if manifest_state not in IMPLEMENTED_CLAIMS:
            continue
        kind, detail = implementation_signal(record)
        if kind in {"declared_limitation", "possibly_unimplemented", "ir_only"} and not (
                record["category"] == "ppc" and kind == "possibly_unimplemented" and manifest_state != "verified"):
            rows.append({"severity": "warning", "kind": "classification_conflict", "id": record["id"],
                         "detail": f"manifest says {manifest_state}, but {detail}"})
    return rows


def _index(snapshot: dict | None) -> dict[str, dict]:
    if not isinstance(snapshot, dict):
        return {}
    return {record["id"]: record for record in snapshot.get("operations", []) if isinstance(record, dict)}


def _implementation_view(record: dict) -> tuple:
    signals = {key: value for key, value in record.get("signals", {}).items() if key != "declared"}
    return (record.get("source"), repr(sorted(signals.items())),
            record.get("evidence", {}).get("implementation"),
            record.get("evidence", {}).get("implementation_token"))


def _test_view(record: dict, results: dict) -> tuple:
    evidence = record.get("evidence", {})
    target = evidence.get("test_target")
    return (evidence.get("test"), evidence.get("test_token"), target,
            tuple(record.get("discovered_tests", [])), tuple(record.get("tagged_tests", [])),
            repr(sorted(results.get(target, {}).items())) if target else None)


def _limitation(record: dict) -> tuple:
    return (record.get("signals", {}).get("declared"), record.get("signals", {}).get("lowering")
            if record.get("signals", {}).get("lowering") in {"conditional", "rejected"} else None,
            record.get("manifest_state") if record.get("manifest_state") in {"stub", "partial"} else None)


def diff(prior: dict | None, current: dict) -> dict:
    """Compare two inventory snapshots."""
    old, new = _index(prior), _index(current)
    old_results = (prior or {}).get("test_results", {}) if isinstance(prior, dict) else {}
    new_results = current.get("test_results", {})
    added = sorted(set(new) - set(old))
    removed = sorted(set(old) - set(new))
    by_key = {(new[ident]["category"], new[ident].get("key")): ident for ident in added if new[ident].get("key")}
    renamed = []
    for ident in list(removed):
        match = by_key.get((old[ident]["category"], old[ident].get("key")))
        if match and match in added:
            renamed.append({"from": ident, "to": match, "key": old[ident]["key"]})
            removed.remove(ident)
            added.remove(match)
    pairs = [(ident, ident) for ident in sorted(set(old) & set(new))]
    pairs += [(row["from"], row["to"]) for row in renamed]
    result = {"baseline": bool(old), "added": added, "removed": removed, "renamed": renamed,
              "implementation_changed": [], "test_evidence_changed": [], "stub_changes": [],
              "classification_changed": [], "state_changed": [], "lost_verification": []}
    for before_id, after_id in pairs:
        before, after = old[before_id], new[after_id]
        if _implementation_view(before) != _implementation_view(after):
            result["implementation_changed"].append(after_id)
        if _test_view(before, old_results) != _test_view(after, new_results):
            result["test_evidence_changed"].append(after_id)
        if _limitation(before) != _limitation(after):
            result["stub_changes"].append({"id": after_id, "from": _describe_limitation(before),
                                           "to": _describe_limitation(after)})
        if before.get("manifest_state") != after.get("manifest_state"):
            result["classification_changed"].append({"id": after_id, "from": before.get("manifest_state"),
                                                     "to": after.get("manifest_state")})
        if before.get("state") != after.get("state"):
            result["state_changed"].append({"id": after_id, "from": before.get("state"), "to": after.get("state")})
            if before.get("state") == "verified":
                result["lost_verification"].append({"id": after_id, "now": after.get("state")})
    for ident in added:
        if any(_limitation(new[ident])):
            result["stub_changes"].append({"id": ident, "from": None, "to": _describe_limitation(new[ident])})
    for ident in removed:
        if old[ident].get("state") == "verified":
            result["lost_verification"].append({"id": ident, "now": "removed from source inventory"})
    old_candidates = _keyed((prior or {}).get("candidates") if isinstance(prior, dict) else None)
    new_candidates = _keyed(current.get("candidates"))
    old_problems = _keyed((prior or {}).get("problems") if isinstance(prior, dict) else None)
    new_problems = _keyed(current.get("problems"))
    result["new_candidates"] = [new_candidates[key] for key in sorted(set(new_candidates) - set(old_candidates))]
    result["resolved_candidates"] = [old_candidates[key] for key in sorted(set(old_candidates) - set(new_candidates))]
    result["new_problems"] = [new_problems[key] for key in sorted(set(new_problems) - set(old_problems))]
    result["resolved_problems"] = [old_problems[key] for key in sorted(set(old_problems) - set(new_problems))]
    result["counts"] = {"before": (prior or {}).get("counts") if isinstance(prior, dict) else None,
                        "after": current.get("counts")}
    return result


def _describe_limitation(record: dict) -> str:
    declared, lowering, state = _limitation(record)
    parts = [f"source declares {declared}" if declared else "", f"lowering {lowering}" if lowering else "",
             f"manifest {state}" if state else ""]
    return "; ".join(part for part in parts if part) or "none"


def _keyed(rows) -> dict[tuple, dict]:
    if not isinstance(rows, list):
        return {}
    return {(row.get("kind"), row.get("id") or "", row.get("detail", "")): row for row in rows if isinstance(row, dict)}


def changed(report: dict) -> bool:
    return any(report[key] for key in ("added", "removed", "renamed", "implementation_changed",
                                       "test_evidence_changed", "stub_changes", "classification_changed",
                                       "state_changed", "lost_verification", "new_candidates",
                                       "resolved_candidates", "new_problems", "resolved_problems"))


def _bullets(rows, render, limit: int) -> list[str]:
    lines = [f"- {render(row)}" for row in rows[:limit]]
    if len(rows) > limit:
        lines.append(f"- … {len(rows) - limit} more (see the attached `diff.json`)")
    return lines


def render_diff(report: dict, limit: int = 25) -> str:
    """Readable comparison for a pull-request body or job summary."""
    if not report["baseline"]:
        return ("No committed `inventory.json` existed, so this run establishes the first baseline. "
                f"{len(report['added'])} operations were recorded.\n")
    if not changed(report):
        return "No inventory, classification, evidence, candidate or problem changes since the committed snapshot.\n"
    lines: list[str] = []
    sections = (
        ("Added operations", report["added"], str),
        ("Removed operations", report["removed"], str),
        ("Renamed operations (same source key)", report["renamed"], lambda r: f"`{r['from']}` → `{r['to']}` ({r['key']})"),
        ("Previously verified operations that lost verification", report["lost_verification"],
         lambda r: f"`{r['id']}` is now {r['now']}"),
        ("Classification changes in the manifest", report["classification_changed"],
         lambda r: f"`{r['id']}`: {r['from']} → {r['to']}"),
        ("Displayed state changes", report["state_changed"], lambda r: f"`{r['id']}`: {r['from']} → {r['to']}"),
        ("New or altered stubs and partial implementations", report["stub_changes"],
         lambda r: f"`{r['id']}`: {r['from'] or 'new'} → {r['to']}"),
        ("Changed implementation paths", report["implementation_changed"], str),
        ("Changed test evidence", report["test_evidence_changed"], str),
        ("New validation problems", report["new_problems"], lambda r: f"**{r['severity']}** {r['kind']}"
         + (f" `{r['id']}`" if r.get("id") else "") + f": {r['detail']}"),
        ("Resolved validation problems", report["resolved_problems"], lambda r: f"{r['kind']}"
         + (f" `{r['id']}`" if r.get("id") else "") + f": {r['detail']}"),
        ("New audit candidates (not applied)", report["new_candidates"], lambda r: f"{r['kind']} `{r['id']}`: {r['detail']}"),
        ("Resolved audit candidates", report["resolved_candidates"], lambda r: f"{r['kind']} `{r['id']}`"),
    )
    for title, rows, render in sections:
        if rows:
            lines += [f"#### {title} ({len(rows)})", ""]
            lines += _bullets(rows, (lambda row, fn=render: f"`{row}`" if fn is str else fn(row)), limit)
            lines.append("")
    return "\n".join(lines)


def render_audit(snapshot: dict, metadata_problems: list[dict], sample: int = 8) -> str:
    """Readable summary of `audit-candidates.json`."""
    problems = sorted(snapshot["problems"] + metadata_problems,
                      key=lambda row: (["error", "warning", "info"].index(row["severity"]), row["kind"],
                                       row.get("id") or "", row["detail"]))
    rows = snapshot["candidates"]
    meta = snapshot.get("metadata", {})
    commit = meta.get("assessed_commit")
    lines = ["# Xenon coverage audit candidates", "",
             "Generated by `python3 tools/coverage/generate.py`. Candidates are discovered by scanning source and test "
             "files. **They are suggestions for review and are never applied automatically**; only a reviewed edit to "
             "`tools/coverage/coverage.json` changes a classification. See the [verification policy](VERIFICATION_POLICY.md).",
             "", f"Assessed revision: `{commit[:12] if commit else 'unknown'}`"
             + (" (with uncommitted changes)" if meta.get("worktree") == "modified" else "") + ". "
             + "CI evidence: " + (", ".join(f"{platform} `{(info.get('commit') or 'unknown')[:12]}` ({info.get('source', 'unknown')})"
                                         for platform, info in sorted(meta.get("ci", {}).items())) or "none recorded") + ".",
             "", "## Validation problems", ""]
    if problems:
        lines += ["| Severity | Kind | Operation | Detail |", "| --- | --- | --- | --- |"]
        lines += [f"| {row['severity']} | {row['kind']} | {('`' + row['id'] + '`') if row.get('id') else '—'} | {row['detail']} |"
                  for row in problems]
    else:
        lines.append("None. Every reviewed `verified` entry has a passing CTest result in the recorded CI evidence.")
    by = Counter((row["category"], row["kind"]) for row in rows)
    kinds = [kind for kind in CANDIDATE_TEXT if any(row["kind"] == kind for row in rows)]
    lines += ["", "## Candidates by area", "",
              "| Kind | " + " | ".join(AREAS.values()) + " |", "| --- |" + " ---: |" * len(AREAS)]
    for kind in kinds:
        lines.append(f"| {CANDIDATE_TEXT[kind]} | " + " | ".join(str(by[(area, kind)]) for area in AREAS) + " |")
    if not kinds:
        lines.append("| None | " + " | ".join("0" for _ in AREAS) + " |")
    grouped: dict[str, list[dict]] = defaultdict(list)
    for row in rows:
        grouped[row["kind"]].append(row)
    for kind in kinds:
        members = grouped[kind]
        lines += ["", f"### {CANDIDATE_TEXT[kind]} ({len(members)})", ""]
        lines += [f"- `{row['id']}`: {row['detail']}" for row in members[:sample]]
        if len(members) > sample:
            lines.append(f"- … {len(members) - sample} more in [`audit-candidates.json`](audit-candidates.json)")
    lines.append("")
    return "\n".join(lines)

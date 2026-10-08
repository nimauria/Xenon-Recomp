#!/usr/bin/env python3
"""Audit source-declared Xenon coverage and render a deterministic dashboard."""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from html import escape
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = Path(__file__).with_name("coverage.json")
SVG = ROOT / "docs" / "coverage" / "dashboard.svg"
REPORT = ROOT / "docs" / "coverage" / "REPORT.md"
KERNEL_REFERENCE = Path(__file__).with_name("reference") / "xenia_xboxkrnl_997d055.json"
KERNEL_REFERENCE_REPORT = ROOT / "docs" / "coverage" / "KERNEL_REFERENCE.md"
STATES = {"verified", "implemented_unverified", "partial", "unimplemented", "unassessed"}
COLORS = {
    "verified": "#3dbb79",
    "implemented_unverified": "#55a6e9",
    "partial": "#f2bc5a",
    "unimplemented": "#88919e",
    "unassessed": "#514c72",
}


class CoverageError(ValueError):
    pass


@dataclass(frozen=True)
class Item:
    id: str
    category: str
    group: str
    name: str
    source: str


def source(path: str) -> str:
    target = ROOT / path
    if not target.is_file():
        raise CoverageError(f"source inventory missing: {path}")
    return target.read_text(encoding="utf-8")


def enum_rows(path: str, enum: str) -> list[str]:
    body = re.search(r"enum class " + re.escape(enum) + r"\s*:[^{]+\{(.*?)\};", source(path), re.S)
    if not body:
        raise CoverageError(f"enum {enum} missing from {path}")
    rows = re.findall(r"^\s*(\w+)\s*=\s*(?:0x[0-9A-Fa-f]+|\d+)\s*,", body.group(1), re.M)
    if not rows:
        raise CoverageError(f"enum {enum} has no explicit rows")
    return rows


def kernel_group(name: str, path: str) -> str:
    if path.endswith("kernel_variables.cpp"):
        return "Variables"
    if path.endswith("src/audio/exports.cpp"):
        return "XMA" if name.startswith("XMA") else "Audio"
    for prefix, group in (("Rtl", "RTL"), ("Ke", "Kernel"), ("Kf", "Kernel"),
                          ("Ki", "Kernel"), ("Nt", "NT / I/O"), ("Io", "NT / I/O"),
                          ("Ex", "Executive"), ("Ob", "Objects"), ("Xex", "Modules"),
                          ("Vd", "Video"), ("Mm", "Memory"), ("Xe", "Crypto"),
                          ("Dbg", "Debug")):
        if name.startswith(prefix):
            return group
    return "Other"


def kernel_registration_rows() -> dict[str, tuple[int, str]]:
    """Read statically recoverable xboxkrnl registrations from source."""
    paths = sorted((ROOT / "src/xbox/exports").glob("xboxkrnl_*.cpp"))
    paths += sorted((ROOT / "src/core/session/exports").glob("*.cpp"))
    rows: dict[str, tuple[int, str]] = {}
    by_ordinal: dict[int, str] = {}

    def add(name: str, ordinal: int, path: str) -> None:
        prior = rows.get(name)
        if prior and prior[0] != ordinal:
            raise CoverageError(f"conflicting xboxkrnl ordinals for {name}")
        if ordinal in by_ordinal and by_ordinal[ordinal] != name:
            raise CoverageError(f"conflicting xboxkrnl names for ordinal 0x{ordinal:X}")
        if not prior:
            rows[name] = (ordinal, path)
        by_ordinal[ordinal] = name

    spec = re.compile(r'\{\s*(0x[0-9A-Fa-f]+)u?\s*,\s*"([A-Za-z_]\w*)"\s*,')
    for file in paths:
        path = str(file.relative_to(ROOT))
        for ordinal, name in spec.findall(source(path)):
            add(name, int(ordinal, 16), path)
    direct = re.compile(
        r'(?P<var>\w+)\.library\s*=\s*"xboxkrnl(?:\.exe)?"\s*;\s*'
        r'(?P=var)\.name\s*=\s*"([A-Za-z_]\w*)"\s*;\s*'
        r'(?P=var)\.ordinal\s*=\s*(0x[0-9A-Fa-f]+)u?\s*;', re.S)
    for path in ("src/xbox/exports/xboxkrnl_misc_exports.cpp",
                 "src/core/session/exports/export_registration.cpp"):
        for match in direct.finditer(source(path)):
            add(match.group(2), int(match.group(3), 16), path)

    audio_path = "src/audio/exports.cpp"
    audio = source(audio_path)
    constants = {name: int(ordinal, 16) for name, ordinal in re.findall(
        r'constexpr std::uint32_t (\w+) = (0x[0-9A-Fa-f]+);', audio)}
    calls = re.findall(r'(?:add|get_context_field)\(\s*"(\w+)"\s*,\s*ordinal::(\w+)', audio)
    if not calls or len(calls) != len(constants):
        raise CoverageError("audio export declarations and registrations differ")
    for name, symbol in calls:
        if name != symbol or symbol not in constants:
            raise CoverageError(f"audio export name/ordinal symbol mismatch: {name}")
        add(name, constants[symbol], audio_path)
    if len(rows) < 240:
        raise CoverageError("xboxkrnl static registration extraction unexpectedly small")

    # The independent diagnostic lookup table is only a sample. Every sampled
    # name/ordinal must agree with the registered source inventory.
    metadata = source("src/xbox/export_metadata.cpp")
    sample = re.findall(
        r'\{"xboxkrnl\.exe",\s*(0x[0-9A-Fa-f]+)u,\s*"([A-Za-z_]\w*)",\s*ExportKind::Function\}',
        metadata)
    if not sample:
        raise CoverageError("xboxkrnl diagnostic metadata sample missing")
    for ordinal, name in sample:
        if name not in rows or rows[name][0] != int(ordinal, 16):
            raise CoverageError(f"diagnostic metadata disagrees with registration: {name}")

    return rows


def kernel_inventory() -> list[Item]:
    items = []
    rows = kernel_registration_rows()
    for name, (_, path) in sorted(rows.items()):
        group = kernel_group(name, path)
        ident = (f"kernel:time:{name}" if path.endswith("xboxkrnl_time_exports.cpp")
                 else f"kernel:crypto:{name}" if name.startswith("XeKeysConsole")
                 else f"kernel:registered:{name}")
        items.append(Item(ident, "kernel", f"xboxkrnl.exe / {group}", name, path))
    return items


def load_kernel_reference(path: Path = KERNEL_REFERENCE) -> tuple[dict, list[dict]]:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise CoverageError(f"cannot load kernel reference: {exc}") from exc
    provenance = raw.get("provenance") if isinstance(raw, dict) else None
    rows = raw.get("exports") if isinstance(raw, dict) else None
    if (not isinstance(raw, dict) or raw.get("schema") != 1 or
            not isinstance(provenance, dict) or not isinstance(rows, list) or not rows):
        raise CoverageError("kernel reference requires schema 1, provenance, and exports")
    commit = provenance.get("commit")
    url = provenance.get("source")
    if (not isinstance(commit, str) or not re.fullmatch(r"[0-9a-f]{40}", commit) or
            not isinstance(url, str) or commit not in url):
        raise CoverageError("kernel reference provenance must pin a commit")
    seen_names: set[str] = set()
    seen_ordinals: set[int] = set()
    previous = 0
    for row in rows:
        if not isinstance(row, dict):
            raise CoverageError("kernel reference contains a non-object export")
        ordinal, name, kind = row.get("ordinal"), row.get("name"), row.get("kind")
        if (type(ordinal) is not int or ordinal <= previous or ordinal > 0xFFFF or
                not isinstance(name, str) or not re.fullmatch(r"[A-Za-z_]\w*", name) or
                kind not in {"function", "variable"}):
            raise CoverageError(f"invalid or unsorted kernel reference entry: {row}")
        if name in seen_names or ordinal in seen_ordinals:
            raise CoverageError(f"duplicate kernel reference name or ordinal: {name}")
        seen_names.add(name)
        seen_ordinals.add(ordinal)
        previous = ordinal
    return provenance, rows


def reconcile_kernel_reference(rows: list[dict],
                               registrations: dict[str, tuple[int, str]]) -> dict:
    reference_by_name = {row["name"]: row for row in rows}
    reference_by_ordinal = {row["ordinal"]: row for row in rows}
    conflicts: list[str] = []
    matched: set[str] = set()
    for name, (ordinal, path) in sorted(registrations.items()):
        expected_kind = "variable" if path.endswith("kernel_variables.cpp") else "function"
        by_name = reference_by_name.get(name)
        by_ordinal = reference_by_ordinal.get(ordinal)
        if (by_name and by_name["ordinal"] == ordinal and by_name["kind"] == expected_kind):
            matched.add(name)
        else:
            detail = (f"{name} at 0x{ordinal:04X} ({expected_kind}, {path}); "
                      f"reference name={by_name}, ordinal={by_ordinal}")
            conflicts.append(detail)
    if conflicts:
        raise CoverageError("kernel reference disagreement:\n  " + "\n  ".join(conflicts))
    missing = [row for row in rows if row["name"] not in matched]
    return {"matched": matched, "missing": missing}


def render_kernel_reference_report(provenance: dict, rows: list[dict],
                                   reconciliation: dict) -> str:
    matched = reconciliation["matched"]
    missing = reconciliation["missing"]
    by_group: dict[str, list[dict]] = defaultdict(list)
    for row in rows:
        group = "Variables" if row["kind"] == "variable" else kernel_group(row["name"], "")
        by_group[group].append(row)
    lines = ["# Xbox kernel export reference reconciliation", "",
             f"Reference: [Xenia xboxkrnl table at `{provenance['commit'][:7]}`]({provenance['source']}). This is a pinned research inventory, not an official Microsoft ABI specification or a version-qualified Xbox 360 kernel guarantee.",
             "", f"**{len(rows)} reference entries:** {len(matched)} exact Xenon source registrations ({percent(len(matched), len(rows))}); {len(missing)} entries have no matching Xenon source registration. These are registration counts, not implementation or game-compatibility percentages.",
             "", "| Subsystem | Reference entries | Exact Xenon registrations | No matching source registration |", "| --- | ---: | ---: | ---: |"]
    for group, members in sorted(by_group.items()):
        present = sum(row["name"] in matched for row in members)
        lines.append(f"| {group} | {len(members)} | {present} | {len(members)-present} |")
    lines += ["", "The absence of a matching registration means this source audit found no handler; it does not establish that the export is needed by a title, unsupported on every kernel version, or absent from a dynamic path. Optional audio registration is build-conditional. The source-derived [dashboard](README.md) keeps its own denominator and audited behavior states.",
              "", "First unmatched entries by ordinal: " + ", ".join(
                  f"`0x{row['ordinal']:04X} {row['name']}`" for row in missing[:16]) + ".", ""]
    return "\n".join(lines)


def inventory() -> list[Item]:
    items: list[Item] = kernel_inventory()

    ppc_path = "src/cpu/ppc/decoder/opcode_catalog.inc"
    ppc = source(ppc_path)
    rows = re.findall(r'^X\(0x[0-9A-Fa-f]+u,\s*"([^"]+)",\s*\w+,\s*(\w+),\s*\w+\)', ppc, re.M)
    if not rows or len(rows) != len(re.findall(r'^X\(', ppc, re.M)):
        raise CoverageError("PPC catalog contains unparseable entries")
    items.extend(Item(f"ppc:{name}", "ppc", family, name, ppc_path) for name, family in rows)

    shader_path = "include/xenon/gpu/shader.hpp"
    for name in enum_rows(shader_path, "ControlFlowOpcode"):
        items.append(Item(f"shader:control:{name}", "shader", "Control flow", name, shader_path))
    lower_path = "src/graphics/xenos/shader_translation.cpp"
    lower = source(lower_path)
    vector_bound = re.search(r'alu\.vector_opcode\s*>\s*(\d+)', lower)
    scalar_bound = re.search(r'alu\.scalar_opcode\s*==\s*(\d+)\s*\|\|\s*alu\.scalar_opcode\s*>\s*(\d+)', lower)
    if not vector_bound or not scalar_bound:
        raise CoverageError("shader ALU recognition bounds missing")
    for n in range(int(vector_bound.group(1)) + 1):
        items.append(Item(f"shader:vector:{n}", "shader", "Vector ALU", f"V{n}", lower_path))
    reserved, maximum = map(int, scalar_bound.groups())
    for n in range(maximum + 1):
        if n != reserved:
            items.append(Item(f"shader:scalar:{n}", "shader", "Scalar ALU", f"S{n}", lower_path))
    fetch_switches = list(re.finditer(r'switch \(fetch\.opcode\)\s*\{(.*?)\n\s*default:', lower, re.S))
    if not fetch_switches:
        raise CoverageError("texture fetch recognition switch missing")
    for n in re.findall(r'case\s+(\d+)\s*:', fetch_switches[-1].group(1)):
        items.append(Item(f"shader:texture:{n}", "shader", "Texture fetch", f"T{n}", lower_path))
    decoder_path = "src/graphics/xenos/shader_decoder.cpp"
    decoder = source(decoder_path)
    if not re.search(r'if \(opcode == 0\)\s*\{\s*result\.kind = ShaderInstructionKind::VertexFetch', decoder):
        raise CoverageError("vertex fetch recognition changed")
    items.append(Item("shader:vertex:0", "shader", "Vertex fetch", "VFetch 0", decoder_path))

    pm4_path = "include/xenon/gpu/types.hpp"
    for name in enum_rows(pm4_path, "PacketType"):
        items.append(Item(f"pm4:header:{name}", "pm4", "Packet headers", name, pm4_path))
    processor = source("src/graphics/xenos/command_processor.cpp")
    groups: dict[str, str] = {}
    for function, group in (("is_draw_opcode", "Draw"), ("is_shader_opcode", "Shader"),
                            ("is_sync_opcode", "Synchronization"), ("is_event_opcode", "Event"),
                            ("is_state_opcode", "State")):
        match = re.search(r'bool ' + function + r'\(.*?\)\s*noexcept\s*\{(.*?)\n\}', processor, re.S)
        if not match:
            raise CoverageError(f"PM4 classifier {function} missing")
        for name in re.findall(r'case Type3Opcode::(\w+):', match.group(1)):
            if name in groups and groups[name] != group:
                raise CoverageError(f"PM4 opcode {name} appears in two groups")
            groups[name] = group
    for name in enum_rows(pm4_path, "Type3Opcode"):
        items.append(Item(f"pm4:type3:{name}", "pm4", groups.get(name, "Other / passthrough"), name, pm4_path))
    ids = [item.id for item in items]
    if len(ids) != len(set(ids)):
        raise CoverageError("duplicate source inventory identifiers")
    return items


def load_manifest(path: Path, items: list[Item]) -> dict[str, dict]:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise CoverageError(f"cannot load coverage manifest: {exc}") from exc
    if not isinstance(raw, dict) or raw.get("schema") != 1 or not isinstance(raw.get("entries"), list):
        raise CoverageError("coverage manifest requires schema 1 and an entries array")
    allowed = {item.id: item for item in items}
    exclusions = raw.get("exclusions", [])
    if not isinstance(exclusions, list):
        raise CoverageError("exclusions must be an array")
    excluded_ids = set()
    for exclusion in exclusions:
        if not isinstance(exclusion, dict) or not isinstance(exclusion.get("id"), str):
            raise CoverageError("exclusion has no string identifier")
        ident = exclusion["id"]
        if ident in allowed or ident in excluded_ids:
            raise CoverageError(f"duplicate or included exclusion identifier: {ident}")
        excluded_ids.add(ident)
        if exclusion.get("kind") not in {"reserved", "unsupported", "host_extension"}:
            raise CoverageError(f"invalid exclusion kind for {ident}")
        path, token = exclusion.get("source"), exclusion.get("token")
        if not isinstance(path, str) or not path.startswith(("src/", "include/")) or not isinstance(token, str) or not token or token not in source(path):
            raise CoverageError(f"invalid exclusion trace for {ident}")
    entries: dict[str, dict] = {}
    for row in raw["entries"]:
        if not isinstance(row, dict) or not isinstance(row.get("id"), str):
            raise CoverageError("coverage entry has no string identifier")
        ident = row["id"]
        if ident in entries:
            raise CoverageError(f"duplicate coverage identifier: {ident}")
        if ident not in allowed:
            raise CoverageError(f"coverage identifier missing from source inventory: {ident}")
        state = row.get("state")
        if not isinstance(state, str) or state not in STATES or state == "unassessed":
            raise CoverageError(f"invalid audited state for {ident}: {state}")
        if state == "verified" and (not row.get("test") or not row.get("test_token")):
            raise CoverageError(f"verified entry lacks test trace: {ident}")
        if state in {"partial", "unimplemented"} and not isinstance(row.get("note"), str):
            raise CoverageError(f"{state} entry lacks explanation: {ident}")
        if row.get("kind") not in {None, "stub", "fallback"} or (row.get("kind") and state != "partial"):
            raise CoverageError(f"invalid implementation kind for {ident}")
        impl = row.get("implementation")
        if not isinstance(impl, str) or not impl.startswith(("src/", "include/")):
            raise CoverageError(f"implementation trace missing: {ident}")
        impl_text = source(impl)
        token = row.get("implementation_token", allowed[ident].name)
        if not isinstance(token, str) or not token:
            raise CoverageError(f"invalid implementation token for {ident}")
        if token not in impl_text:
            raise CoverageError(f"implementation token absent for {ident}: {token}")
        if row.get("test"):
            test_path = row["test"]
            if not isinstance(test_path, str) or not test_path.startswith("tests/"):
                raise CoverageError(f"invalid test path for {ident}")
            test_token = row.get("test_token", allowed[ident].name)
            if not isinstance(test_token, str) or not test_token or test_token not in source(test_path):
                raise CoverageError(f"test token absent for {ident}")
        entries[ident] = row
    return entries


def counts(items: list[Item], entries: dict[str, dict]) -> Counter:
    return Counter(entries.get(item.id, {}).get("state", "unassessed") for item in items)


def percent(numerator: int, denominator: int) -> str:
    return "unknown" if denominator == 0 else f"{100 * numerator / denominator:.1f}%"


def summary(items: list[Item], entries: dict[str, dict]) -> str:
    c = counts(items, entries)
    known = len(items) - c["unassessed"]
    return f"{len(items)} listed · {known} audited · ≥{percent(c['verified'], len(items))} test verified"


def render_groups(lines: list[str], groups: dict[str, list[Item]], entries: dict[str, dict],
                  x: int, y: int, width: int, height: int) -> None:
    if not groups:
        return
    minimum = 28
    available = height - minimum * len(groups)
    if available < 0:
        raise CoverageError("dashboard has too many groups for its panel")
    total = sum(len(members) for members in groups.values())
    assigned = 0
    used = 0
    for group, members in groups.items():
        assigned += len(members)
        extra = (available * assigned // total) - used
        used += extra
        band = minimum + extra
        c = counts(members, entries)
        short = group.removeprefix("xboxkrnl.exe / ")
        label = f"{short}  ·  {len(members)}  ·  ≥{percent(c['verified'], len(members))} verified"
        lines.append(f'<text x="{x}" y="{y+12}" fill="#dce7f2" font-size="11" font-family="sans-serif">{escape(label)}</text>')
        cols = max(1, min(len(members), 36, width // 11))
        rows = (len(members) + cols - 1) // cols
        cw = width / cols
        ch = (band - 19) / rows
        for j, item in enumerate(members):
            state = entries.get(item.id, {}).get("state", "unassessed")
            cx = x + (j % cols) * cw
            cy = y + 17 + (j // cols) * ch
            title = f"{item.id}: {state}; {item.source}"
            lines.append(f'<rect x="{cx:.2f}" y="{cy:.2f}" width="{max(1,cw-1):.2f}" height="{max(1,ch-1):.2f}" fill="{COLORS[state]}" stroke="#101721" stroke-width="0.5"><title>{escape(title)}</title></rect>')
        y += band


def render_svg(items: list[Item], entries: dict[str, dict]) -> str:
    by_category: dict[str, list[Item]] = defaultdict(list)
    for item in items:
        by_category[item.category].append(item)
    total = counts(items, entries)
    overall = f"{total['verified']}/{len(items)} (≥{percent(total['verified'], len(items))}) test verified in the listed source subset"
    lines = ['<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="1030" viewBox="0 0 1200 1030" role="img" aria-labelledby="title desc">',
             '<title id="title">Xenon source coverage audit</title>',
             '<desc id="desc">Treemaps of audited source-declared operations. Percentages are verified lower bounds; unassessed entries remain unknown.</desc>',
             '<rect width="1200" height="1030" fill="#101721"/>',
             '<text x="30" y="49" fill="#f4f7fb" font-size="30" font-family="sans-serif" font-weight="700">XENON  /  IMPLEMENTATION AUDIT</text>',
             f'<text x="30" y="76" fill="#bdc9d6" font-size="15" font-family="sans-serif">Source-declared scope • {escape(overall)} • overall hardware coverage unknown</text>']
    titles = {"kernel": "Xbox kernel exports", "ppc": "PowerPC instructions", "shader": "Xenos shader instructions", "pm4": "GPU PM4 packets"}
    for index, category in enumerate(("kernel", "ppc", "shader", "pm4")):
        x = 30 + (index % 2) * 585
        y = 105 + (index // 2) * 430
        panel = by_category[category]
        lines.append(f'<rect x="{x}" y="{y}" width="555" height="410" rx="8" fill="#1b2634" stroke="#405168"/>')
        lines.append(f'<text x="{x+16}" y="{y+30}" fill="#f4f7fb" font-size="21" font-family="sans-serif" font-weight="700">{titles[category]}</text>')
        lines.append(f'<text x="{x+16}" y="{y+52}" fill="#bdc9d6" font-size="12" font-family="sans-serif">{escape(summary(panel, entries))}</text>')
        groups: dict[str, list[Item]] = defaultdict(list)
        for item in panel:
            groups[item.group].append(item)
        if category == "kernel":
            columns: list[dict[str, list[Item]]] = [{}, {}]
            sizes = [0, 0]
            for group, members in sorted(groups.items(), key=lambda pair: (-len(pair[1]), pair[0])):
                side = 0 if sizes[0] <= sizes[1] else 1
                columns[side][group] = members
                sizes[side] += len(members)
            for side, column in enumerate(columns):
                render_groups(lines, dict(sorted(column.items())), entries,
                              x + 16 + side * 264, y + 68, 256, 325)
        else:
            render_groups(lines, groups, entries, x + 16, y + 68, 520, 325)
    legend = [("verified", "Verified by test"), ("implemented_unverified", "Implemented, unverified"),
              ("partial", "Partial / stub"), ("unimplemented", "Unimplemented"), ("unassessed", "Unassessed")]
    for i, (state, label) in enumerate(legend):
        x = 31 + i * 232
        lines.append(f'<rect x="{x}" y="984" width="18" height="18" fill="{COLORS[state]}"/>')
        lines.append(f'<text x="{x+26}" y="998" fill="#e5edf6" font-size="13" font-family="sans-serif">{escape(label)}</text>')
    lines.append('</svg>')
    return "\n".join(lines) + "\n"


def render_report(items: list[Item], entries: dict[str, dict]) -> str:
    total = counts(items, entries)
    lines = ["# Xenon coverage audit", "", "Generated by `python3 tools/coverage/generate.py` from source inventories and `coverage.json`.",
             "Percentages below are audited lower bounds against **source-declared** inventories. Unassessed entries are unknown, not unimplemented. They are not game compatibility figures.", "",
             "| Area | Listed | Audited | Verified by tests | Implemented, unverified | Partial / stub | Unimplemented | Unassessed |", "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for category, name in (("kernel", "Kernel static registrations"), ("ppc", "PPC catalog"), ("shader", "Shader frontend"), ("pm4", "PM4 source enum")):
        group = [item for item in items if item.category == category]
        c = counts(group, entries)
        lines.append(f"| {name} | {len(group)} | {len(group)-c['unassessed']} | {c['verified']} (≥{percent(c['verified'],len(group))}) | {c['implemented_unverified']} | {c['partial']} | {c['unimplemented']} | {c['unassessed']} |")
    lines += ["", f"**Combined source-declared audit lower bound:** {total['verified']}/{len(items)} (≥{percent(total['verified'],len(items))}) test verified; {total['verified']+total['implemented_unverified']}/{len(items)} (≥{percent(total['verified']+total['implemented_unverified'],len(items))}) has traced implementation. **Overall Xbox 360 implementation coverage: unknown.** The kernel inventory extracts statically recoverable ordinal/name registrations and variables from xboxkrnl export, audio, and session sources. It is not a complete console export table: operations absent from Xenon source are outside the denominator, and optional audio exports depend on build configuration. PPC counts the decoder catalog, not every possible PowerPC encoding. Shader counts control-flow enum values and ALU/fetch forms recognized by this frontend; reserved scalar opcode 41 is excluded. PM4 counts declared packet headers and type-3 opcodes, not undocumented hardware values; host extension 0x64 is excluded. A handler, decoder entry, or passthrough packet does not by itself prove implementation.",
              "", "Evidence paths and test tokens are in the manifest. `verified` means a behavior-oriented test is traced to the entry; it does not imply retail-title qualification. GitHub CI checks the generated files for drift.", ""]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail if generated files differ")
    parser.add_argument("--manifest", type=Path, default=MANIFEST)
    args = parser.parse_args()
    try:
        items = inventory()
        entries = load_manifest(args.manifest, items)
        provenance, reference = load_kernel_reference()
        reconciliation = reconcile_kernel_reference(reference, kernel_registration_rows())
        products = {SVG: render_svg(items, entries), REPORT: render_report(items, entries),
                    KERNEL_REFERENCE_REPORT: render_kernel_reference_report(
                        provenance, reference, reconciliation)}
        if args.check:
            stale = [str(path.relative_to(ROOT)) for path, data in products.items()
                     if not path.is_file() or path.read_text(encoding="utf-8") != data]
            if stale:
                raise CoverageError("stale generated assets: " + ", ".join(stale))
        else:
            for path, data in products.items():
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(data, encoding="utf-8")
        for category in ("kernel", "ppc", "shader", "pm4"):
            print(f"{category}: {summary([i for i in items if i.category == category], entries)}")
        print(f"kernel reference: {len(reconciliation['matched'])}/{len(reference)} exact registrations")
        return 0
    except CoverageError as exc:
        print(f"coverage error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

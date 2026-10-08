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


def inventory() -> list[Item]:
    items: list[Item] = []
    time_path = "src/xbox/exports/xboxkrnl_time_exports.cpp"
    time = source(time_path)
    specs = re.search(r"const TimeExportSpec kTimeExports\[\]\s*=\s*\{(.*?)\};", time, re.S)
    if not specs:
        raise CoverageError("time export specification missing")
    for name in re.findall(r'\{0x[0-9A-Fa-f]+u,\s*"([^"]+)"', specs.group(1)):
        items.append(Item(f"kernel:time:{name}", "kernel", "xboxkrnl.exe / time", name, time_path))
    misc_path = "src/xbox/exports/xboxkrnl_misc_exports.cpp"
    misc = source(misc_path)
    for name in re.findall(r'desc\.name\s*=\s*"(XeKeysConsole(?:PrivateKeySign|SignatureVerification))";', misc):
        items.append(Item(f"kernel:crypto:{name}", "kernel", "xboxkrnl.exe / crypto", name, misc_path))
    if len([i for i in items if i.category == "kernel"]) != 6:
        raise CoverageError("kernel audit subset changed; review source extraction")

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
        cursor = y + 68
        remaining = 325
        remaining_count = len(panel)
        for group_index, (group, members) in enumerate(groups.items()):
            height = remaining if group_index == len(groups) - 1 else max(28, round(325 * len(members) / len(panel)))
            height = min(height, remaining)
            remaining -= height
            remaining_count -= len(members)
            if height <= 0:
                continue
            c = counts(members, entries)
            label = f"{group}  ·  {len(members)}  ·  ≥{percent(c['verified'], len(members))} verified"
            lines.append(f'<text x="{x+16}" y="{cursor+12}" fill="#dce7f2" font-size="11" font-family="sans-serif">{escape(label)}</text>')
            cell_top = cursor + 17
            area_height = max(4, height - 20)
            cols = max(1, min(len(members), 36))
            rows = (len(members) + cols - 1) // cols
            cw = 520 / cols
            ch = area_height / rows
            for j, item in enumerate(members):
                state = entries.get(item.id, {}).get("state", "unassessed")
                cx = x + 16 + (j % cols) * cw
                cy = cell_top + (j // cols) * ch
                title = f"{item.id}: {state}; {item.source}"
                lines.append(f'<rect x="{cx:.2f}" y="{cy:.2f}" width="{max(1,cw-1):.2f}" height="{max(1,ch-1):.2f}" fill="{COLORS[state]}" stroke="#101721" stroke-width="0.5"><title>{escape(title)}</title></rect>')
            cursor += height
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
    lines = ["# Phase 1 coverage audit", "", "Generated by `python3 tools/coverage/generate.py` from source inventories and `coverage.json`.",
             "Percentages below are audited lower bounds against **source-declared** inventories. Unassessed entries are unknown, not unimplemented. They are not game compatibility figures.", "",
             "| Area | Listed | Audited | Verified by tests | Implemented, unverified | Partial / stub | Unimplemented | Unassessed |", "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for category, name in (("kernel", "Kernel subset"), ("ppc", "PPC catalog"), ("shader", "Shader frontend"), ("pm4", "PM4 source enum")):
        group = [item for item in items if item.category == category]
        c = counts(group, entries)
        lines.append(f"| {name} | {len(group)} | {len(group)-c['unassessed']} | {c['verified']} (≥{percent(c['verified'],len(group))}) | {c['implemented_unverified']} | {c['partial']} | {c['unimplemented']} | {c['unassessed']} |")
    lines += ["", f"**Combined source-declared audit lower bound:** {total['verified']}/{len(items)} (≥{percent(total['verified'],len(items))}) test verified; {total['verified']+total['implemented_unverified']}/{len(items)} (≥{percent(total['verified']+total['implemented_unverified'],len(items))}) has traced implementation. **Overall Xbox 360 implementation coverage: unknown.** The kernel inventory includes only four time exports and two crypto stubs, not a complete xboxkrnl export table. PPC counts the decoder catalog, not every possible PowerPC encoding. Shader counts control-flow enum values and ALU/fetch forms recognized by this frontend; reserved scalar opcode 41 is excluded. PM4 counts declared packet headers and type-3 opcodes, not undocumented hardware values; host extension 0x64 is excluded. A handler, decoder entry, or passthrough packet does not by itself prove implementation.",
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
        products = {SVG: render_svg(items, entries), REPORT: render_report(items, entries)}
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
        return 0
    except CoverageError as exc:
        print(f"coverage error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

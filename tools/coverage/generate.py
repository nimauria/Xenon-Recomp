#!/usr/bin/env python3
"""Audit source-declared Xenon coverage and render a deterministic dashboard."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from html import escape
from pathlib import Path

import audit
import evidence

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = Path(__file__).with_name("coverage.json")
SVG = ROOT / "docs" / "coverage" / "dashboard.svg"
SUMMARY_SVG = ROOT / "docs" / "coverage" / "summary.svg"
REPORT = ROOT / "docs" / "coverage" / "REPORT.md"
KERNEL_REFERENCE = Path(__file__).with_name("reference") / "xenia_xboxkrnl_997d055.json"
KERNEL_REFERENCE_REPORT = ROOT / "docs" / "coverage" / "KERNEL_REFERENCE.md"
INVENTORY_JSON = ROOT / "docs" / "coverage" / "inventory.json"
CANDIDATES_JSON = ROOT / "docs" / "coverage" / "audit-candidates.json"
AUDIT_REPORT = ROOT / "docs" / "coverage" / "AUDIT.md"
GENERATED = (SVG, SUMMARY_SVG, REPORT, KERNEL_REFERENCE_REPORT, INVENTORY_JSON, CANDIDATES_JSON, AUDIT_REPORT)
CATEGORIES = ("kernel", "ppc", "shader", "pm4")
STATES = {"verified", "implemented_unverified", "partial", "stub", "unimplemented", "unassessed"}
STATE_ORDER = ("verified", "implemented_unverified", "partial", "stub", "unimplemented", "unassessed")
COLORS = {
    "verified": "#3dbb79",
    "implemented_unverified": "#55a6e9",
    "partial": "#f2bc5a",
    "stub": "#d98b6a",
    "unimplemented": "#88919e",
    "unassessed": "#514c72",
}
CHIP_LABELS = {"verified": "verified", "implemented_unverified": "unverified", "partial": "partial",
               "stub": "stub", "unimplemented": "unimplemented", "unassessed": "unassessed"}


class CoverageError(ValueError):
    pass


@dataclass(frozen=True)
class Item:
    id: str
    category: str
    group: str
    name: str
    source: str
    key: str = ""


def source(path: str) -> str:
    target = (ROOT / path).resolve()
    if not target.is_relative_to(ROOT):
        raise CoverageError(f"source path escapes repository: {path}")
    if not target.is_file():
        raise CoverageError(f"source inventory missing: {path}")
    return target.read_text(encoding="utf-8")


def enum_values(path: str, enum: str) -> list[tuple[str, int]]:
    body = re.search(r"enum class " + re.escape(enum) + r"\s*:[^{]+\{(.*?)\};", source(path), re.S)
    if not body:
        raise CoverageError(f"enum {enum} missing from {path}")
    rows = re.findall(r"^\s*(\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*,", body.group(1), re.M)
    if not rows:
        raise CoverageError(f"enum {enum} has no explicit rows")
    return [(name, int(value, 0)) for name, value in rows]


def enum_rows(path: str, enum: str) -> list[str]:
    return [name for name, _ in enum_values(path, enum)]


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


def kernel_source_paths() -> list[str]:
    paths = sorted((ROOT / "src/xbox/exports").glob("xboxkrnl_*.cpp"))
    paths += sorted((ROOT / "src/core/session/exports").glob("*.cpp"))
    return [str(file.relative_to(ROOT)) for file in paths]


def kernel_registration_rows() -> dict[str, tuple[int, str]]:
    """Read statically recoverable xboxkrnl registrations from source."""
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

    spec = re.compile(r'\{\s*(0x[0-9A-Fa-f]+)u?\s*,\s*"([A-Za-z_]\w*)"\s*[,}]')
    for path in kernel_source_paths():
        contents = source(path)
        matches = spec.findall(contents)
        candidates = len(re.findall(r'\{\s*0x[0-9A-Fa-f]+u?\s*,\s*"', contents))
        if candidates != len(matches):
            raise CoverageError(f"unparseable xboxkrnl export rows in {path}")
        for ordinal, name in matches:
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
    if not rows:
        raise CoverageError("xboxkrnl static registration extraction found no rows")

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


def kernel_declarations() -> dict[str, dict]:
    """Handler symbols and source-declared stub/partial flags per export.

    Spec-table rows carry `partial` as the field after the handler; direct
    descriptors set `.requirement = ExportRequirement::Stubbed` and/or
    `.partial = true` before their `register_export` call.
    """
    found: dict[str, dict] = {}
    row = re.compile(r'\{\s*0x[0-9A-Fa-f]+u?\s*,\s*"([A-Za-z_]\w*)"\s*,\s*(&?[A-Za-z_][\w:]*|\[[^\]]*\])'
                     r'(?:[^,{}]*)?(?:,\s*(true|false)\b)?')
    for path in kernel_source_paths():
        text = source(path)
        for name, handler, partial in row.findall(text):
            info = found.setdefault(name, {})
            if handler in {"true", "false", "nullptr"}:
                continue  # a data row (e.g. printf variants), not a handler spec
            info["handler"] = "lambda" if handler.startswith("[") else handler.lstrip("&")
            if partial == "true":
                info["declared"] = "partial"
        for segment in text.split("register_export("):
            names = re.findall(r'\w+\.name\s*=\s*"([A-Za-z_]\w*)"\s*;', segment)
            if len(names) != 1:
                continue
            info = found.setdefault(names[0], {})
            handler = re.search(r'\w+\.handler\s*=\s*&?([A-Za-z_][\w:]*)', segment)
            if handler:
                info["handler"] = handler.group(1)
            stubbed = "ExportRequirement::Stubbed" in segment
            partial = re.search(r'\w+\.partial\s*=\s*true\s*;', segment) is not None
            if stubbed or partial:
                info["declared"] = "stubbed" if stubbed and not partial else (
                    "stubbed+partial" if stubbed else "partial")
    return found


def kernel_inventory() -> list[Item]:
    items = []
    rows = kernel_registration_rows()
    for name, (ordinal, path) in sorted(rows.items()):
        group = kernel_group(name, path)
        ident = (f"kernel:time:{name}" if path.endswith("xboxkrnl_time_exports.cpp")
                 else f"kernel:crypto:{name}" if name.startswith("XeKeysConsole")
                 else f"kernel:registered:{name}")
        items.append(Item(ident, "kernel", f"xboxkrnl.exe / {group}", name, path, f"ordinal:0x{ordinal:04X}"))
    return items


def load_kernel_reference(path: Path = KERNEL_REFERENCE) -> tuple[dict, list[dict]]:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise CoverageError(f"cannot load kernel reference: {exc}") from exc
    provenance = raw.get("provenance") if isinstance(raw, dict) else None
    rows = raw.get("exports") if isinstance(raw, dict) else None
    if (not isinstance(raw, dict) or type(raw.get("schema")) is not int or raw.get("schema") != 1 or
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
    rows = re.findall(r'^X\((0x[0-9A-Fa-f]+)u,\s*"([^"]+)",\s*\w+,\s*(\w+),\s*\w+\)', ppc, re.M)
    if not rows or len(rows) != len(re.findall(r'^X\(', ppc, re.M)):
        raise CoverageError("PPC catalog contains unparseable entries")
    items.extend(Item(f"ppc:{name}", "ppc", family, name, ppc_path, f"pattern:{pattern.lower()}")
                 for pattern, name, family in rows)

    shader_path = "include/xenon/gpu/shader.hpp"
    for name, value in enum_values(shader_path, "ControlFlowOpcode"):
        items.append(Item(f"shader:control:{name}", "shader", "Control flow", name, shader_path, f"control:{value}"))
    lower_path = "src/graphics/xenos/shader_translation.cpp"
    lower = source(lower_path)
    vector_bound = re.search(r'alu\.vector_opcode\s*>\s*(\d+)', lower)
    scalar_bound = re.search(r'alu\.scalar_opcode\s*==\s*(\d+)\s*\|\|\s*alu\.scalar_opcode\s*>\s*(\d+)', lower)
    if not vector_bound or not scalar_bound:
        raise CoverageError("shader ALU recognition bounds missing")
    for n in range(int(vector_bound.group(1)) + 1):
        items.append(Item(f"shader:vector:{n}", "shader", "Vector ALU", f"V{n}", lower_path, f"vector:{n}"))
    reserved, maximum = map(int, scalar_bound.groups())
    for n in range(maximum + 1):
        if n != reserved:
            items.append(Item(f"shader:scalar:{n}", "shader", "Scalar ALU", f"S{n}", lower_path, f"scalar:{n}"))
    fetch_switches = list(re.finditer(r'switch \(fetch\.opcode\)\s*\{(.*?)\n\s*default:', lower, re.S))
    if not fetch_switches:
        raise CoverageError("texture fetch recognition switch missing")
    for n in re.findall(r'case\s+(\d+)\s*:', fetch_switches[-1].group(1)):
        items.append(Item(f"shader:texture:{n}", "shader", "Texture fetch", f"T{n}", lower_path, f"texture:{n}"))
    decoder_path = "src/graphics/xenos/shader_decoder.cpp"
    decoder = source(decoder_path)
    if not re.search(r'if \(opcode == 0\)\s*\{\s*result\.kind = ShaderInstructionKind::VertexFetch', decoder):
        raise CoverageError("vertex fetch recognition changed")
    items.append(Item("shader:vertex:0", "shader", "Vertex fetch", "VFetch 0", decoder_path, "vertex:0"))

    pm4_path = "include/xenon/gpu/types.hpp"
    for name, value in enum_values(pm4_path, "PacketType"):
        items.append(Item(f"pm4:header:{name}", "pm4", "Packet headers", name, pm4_path, f"header:{value}"))
    processor = source("src/graphics/xenos/command_processor.cpp")
    groups = pm4_classifier_groups(processor)
    for name, value in enum_values(pm4_path, "Type3Opcode"):
        items.append(Item(f"pm4:type3:{name}", "pm4", groups.get(name, "Other / passthrough"), name, pm4_path,
                          f"type3:0x{value:02X}"))
    ids = [item.id for item in items]
    if len(ids) != len(set(ids)):
        raise CoverageError("duplicate source inventory identifiers")
    return items


def pm4_classifier_groups(processor: str) -> dict[str, str]:
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
    return groups


def function_body(text: str, signature: str) -> str:
    start = text.find(signature)
    if start < 0:
        raise CoverageError(f"source function missing: {signature}")
    end = text.find("\n}\n", start)
    return text[start:end if end >= 0 else len(text)]


def source_signals(items: list[Item]) -> dict[str, dict]:
    """Per-operation implementation facts found by scanning source.

    These are monitoring signals: they feed audit candidates and change
    detection, never the classification itself.
    """
    signals: dict[str, dict] = {item.id: {} for item in items}
    declarations = kernel_declarations()

    def literals(directory: str) -> tuple[dict[str, set[str]], dict[str, set[str]]]:
        """Exact mnemonic literals and `starts_with` family prefixes per file."""
        exact: dict[str, set[str]] = defaultdict(set)
        prefixes: dict[str, set[str]] = defaultdict(set)
        for file in sorted((ROOT / directory).glob("*.cpp")):
            path = str(file.relative_to(ROOT))
            text = source(path)
            for literal in set(re.findall(r'"([a-z][a-z0-9.]*)"', text)):
                exact[literal].add(path)
            for prefix in set(re.findall(r'\bstarts(?:_with)?\(\s*(?:\w+\s*,\s*)?"([a-z][a-z0-9.]*)"', text)):
                prefixes[prefix].add(path)
        return exact, prefixes

    def family(mnemonic: str, prefixes: dict[str, set[str]]) -> list[str]:
        return sorted({path for prefix, paths in prefixes.items() if mnemonic.startswith(prefix) for path in paths})

    lifter, lifter_prefixes = literals("src/cpu/ppc/lifter")
    fallback, fallback_prefixes = literals("src/cpu/codegen/fallback/interpreter")
    lower = source("src/graphics/xenos/shader_translation.cpp")
    lowered: dict[str, set[int]] = {}
    for kind, signature in (("vector", "float4 xenon_vector_op("), ("scalar", "float4 xenon_scalar_op(")):
        body = function_body(lower, signature)
        ops = {int(n) for n in re.findall(r'\bop\s*==\s*(\d+)', body)}
        for low, high in re.findall(r'\bop\s*>=\s*(\d+)\s*&&\s*op\s*<=\s*(\d+)', body):
            ops.update(range(int(low), int(high) + 1))
        lowered[kind] = ops
    fetch = list(re.finditer(r'switch \(fetch\.opcode\)\s*\{(.*?)\n\s*default:', lower, re.S))[-1].group(1)
    fetch_state: dict[int, str] = {}
    for labels, body in re.findall(r'((?:\s*case\s+\d+\s*:[^\n]*\n)+)(.*?)(?=\n\s*case\s+\d+\s*:|\Z)', fetch, re.S):
        state = ("conditional" if "unsupported" in body and re.search(r'\bif\s*\(', body)
                 else "rejected" if "unsupported" in body else "accepted")
        for n in re.findall(r'case\s+(\d+)\s*:', labels):
            fetch_state[int(n)] = state
    processor = source("src/graphics/xenos/command_processor.cpp")
    type3 = function_body(processor, "void CommandProcessor::execute_type3(")
    dedicated = set(re.findall(r'(?:header\.opcode\s*==\s*|case\s+)Type3Opcode::(\w+)', type3))
    classifier = pm4_classifier_groups(processor)
    for item in items:
        info = signals[item.id]
        if item.category == "kernel":
            if item.source.endswith("kernel_variables.cpp"):
                info["kind"] = "variable"
            info.update(declarations.get(item.name, {}))
        elif item.category == "ppc":
            for kind, exact, prefixes in (("lifter", lifter, lifter_prefixes),
                                          ("fallback", fallback, fallback_prefixes)):
                if exact.get(item.name):
                    info[kind] = sorted(exact[item.name])
                elif family(item.name, prefixes):
                    info[kind + "_family"] = family(item.name, prefixes)
        elif item.id.startswith("shader:control:"):
            info["lowering"] = "referenced" if f"ControlFlowOpcode::{item.name}" in lower else "none"
        elif item.id.startswith(("shader:vector:", "shader:scalar:")):
            kind, number = item.id.split(":")[1:]
            info["lowering"] = "branch" if int(number) in lowered[kind] else "none"
        elif item.id.startswith("shader:texture:"):
            info["lowering"] = fetch_state.get(int(item.id.rsplit(":", 1)[1]), "none")
        elif item.id.startswith("shader:vertex:"):
            info["lowering"] = "decoder"
        elif item.id.startswith("pm4:header:"):
            handler = f"execute_type{item.name.removeprefix('Type')}"
            info["handler"] = "dedicated" if f"CommandProcessor::{handler}(" in processor or (
                f"PacketType::{item.name}:" in processor) else "none"
        else:
            group = classifier.get(item.name)
            info["handler"] = ("dedicated" if item.name in dedicated or group == "Draw"
                               else "ir_packet" if group else "passthrough")
            if group:
                info["ir"] = group
    return signals


def ctest_targets() -> dict[str, set[str]]:
    """Read declared test executables and their CTest registrations from CMake."""
    targets: dict[str, set[str]] = {}
    for file in sorted((ROOT / "tests").rglob("CMakeLists.txt")):
        cmake = file.read_text(encoding="utf-8")
        relative = file.parent.relative_to(ROOT)
        registered = set(re.findall(r'\badd_test\s*\(\s*NAME\s+(\w+)', cmake))
        for match in re.finditer(r'\b(add_executable|xenon_add_standalone_test)\s*\(\s*(\w+)\s+(.*?)\)', cmake, re.S):
            command, target, body = match.groups()
            if command == "add_executable" and target not in registered:
                continue
            paths = {str(relative / path) for path in re.findall(r'[\w/.-]+\.cpp', body)}
            if target in targets:
                raise CoverageError(f"duplicate CTest target: {target}")
            targets[target] = paths
    return targets


class EntryError(CoverageError):
    """A manifest entry problem. `scope` is "entry" (drop the classification)
    or "test" (keep the implementation classification but not the test)."""

    def __init__(self, message: str, scope: str = "entry"):
        super().__init__(message)
        self.scope = scope


def check_entry(row: dict, allowed: dict[str, Item], entries: dict, test_targets: dict,
                evidence_seen: dict) -> None:
    ident = row["id"]
    unknown_fields = set(row) - {"id", "state", "kind", "implementation",
                                 "implementation_token", "test", "test_token",
                                 "test_target", "shared_test", "note"}
    if unknown_fields:
        raise EntryError(f"unknown coverage fields for {ident}: {sorted(unknown_fields)}")
    if ident in entries:
        raise EntryError(f"duplicate coverage identifier: {ident}")
    if ident not in allowed:
        raise EntryError(f"coverage identifier missing from source inventory: {ident}")
    state = row.get("state")
    if not isinstance(state, str) or state not in STATES or state == "unassessed":
        raise EntryError(f"invalid audited state for {ident}: {state}")
    if state == "verified" and (not row.get("test") or not row.get("test_token") or not row.get("test_target")):
        raise EntryError(f"verified entry lacks test trace: {ident}", "test")
    if state in {"partial", "stub", "unimplemented"} and (
            not isinstance(row.get("note"), str) or not row["note"].strip()):
        raise EntryError(f"{state} entry lacks explanation: {ident}")
    if row.get("kind") not in {None, "fallback"} or (row.get("kind") and state != "partial"):
        raise EntryError(f"invalid implementation kind for {ident}")
    impl = row.get("implementation")
    if not isinstance(impl, str) or not impl.startswith(("src/", "include/")):
        raise EntryError(f"implementation trace missing: {ident}")
    impl_text = source(impl)
    token = row.get("implementation_token", allowed[ident].name)
    if not isinstance(token, str) or not token:
        raise EntryError(f"invalid implementation token for {ident}")
    if token not in impl_text:
        raise EntryError(f"implementation token absent for {ident}: {token}")
    if "shared_test" in row and row["shared_test"] is not True:
        raise EntryError(f"invalid shared_test value for {ident}")
    if row.get("test"):
        test_path = row["test"]
        if not isinstance(test_path, str) or not test_path.startswith("tests/"):
            raise EntryError(f"invalid test path for {ident}", "test")
        test_token = row.get("test_token", allowed[ident].name)
        try:
            test_text = source(test_path)
        except CoverageError as exc:
            raise EntryError(str(exc), "test") from exc
        if not isinstance(test_token, str) or not test_token or test_token not in test_text:
            raise EntryError(f"test token absent for {ident}", "test")
        if test_token.startswith("test_") and len(re.findall(r'\b' + re.escape(test_token) + r'\s*\(', test_text)) < 2:
            raise EntryError(f"test function is not invoked for {ident}: {test_token}", "test")
        target = row.get("test_target")
        if not isinstance(target, str) or test_path not in test_targets.get(target, set()):
            raise EntryError(f"test source not registered with CTest target for {ident}: {target}", "test")
        key = (target, test_token)
        if key in evidence_seen and not (row.get("shared_test") is True and
                                         evidence_seen[key].get("shared_test") is True):
            raise EntryError(f"duplicate test evidence for {ident} and {evidence_seen[key]['id']}", "test")
        evidence_seen[key] = row
    elif any(key in row for key in ("test_target", "test_token", "shared_test")):
        raise EntryError(f"test evidence incomplete for {ident}", "test")


def validate_manifest(path: Path, items: list[Item], strict: bool = True,
                      test_targets: dict | None = None) -> tuple[dict[str, dict], list[dict]]:
    """Validate the reviewed manifest.

    Strict mode raises on the first problem. Lenient mode records each problem
    and keeps going: an entry with a broken implementation trace is treated as
    unassessed; an entry with a broken test trace keeps its implementation
    classification but is never displayed as verified.
    """
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise CoverageError(f"cannot load coverage manifest: {exc}") from exc
    if (not isinstance(raw, dict) or type(raw.get("schema")) is not int or
            raw.get("schema") != 1 or not isinstance(raw.get("entries"), list)):
        raise CoverageError("coverage manifest requires schema 1 and an entries array")
    unknown_root = set(raw) - {"schema", "scope", "entries", "exclusions"}
    if unknown_root:
        raise CoverageError(f"unknown coverage manifest fields: {sorted(unknown_root)}")
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
        path_, token = exclusion.get("source"), exclusion.get("token")
        if not isinstance(path_, str) or not path_.startswith(("src/", "include/")) or not isinstance(token, str) or not token or token not in source(path_):
            raise CoverageError(f"invalid exclusion trace for {ident}")
    entries: dict[str, dict] = {}
    problems: list[dict] = []
    test_targets = ctest_targets() if test_targets is None else test_targets
    evidence_seen: dict[tuple[str, str], dict] = {}
    for row in raw["entries"]:
        if not isinstance(row, dict) or not isinstance(row.get("id"), str):
            if strict:
                raise CoverageError("coverage entry has no string identifier")
            problems.append({"severity": "error", "kind": "invalid_manifest_entry", "id": None,
                             "detail": "coverage entry has no string identifier"})
            continue
        try:
            check_entry(row, allowed, entries, test_targets, evidence_seen)
            entries[row["id"]] = row
        except EntryError as exc:
            if strict:
                raise CoverageError(str(exc)) from None
            message = str(exc)
            kind = ("duplicate_identifier" if message.startswith("duplicate coverage identifier")
                    else "unknown_operation" if "missing from source inventory" in message
                    else "broken_test_reference" if exc.scope == "test" else "invalid_manifest_entry")
            problems.append({"severity": "error", "kind": kind, "id": row["id"], "detail": message})
            if exc.scope == "test" and row["id"] not in entries:
                kept = {key: value for key, value in row.items()
                        if key not in {"test", "test_token", "test_target", "shared_test"}}
                if kept.get("state") == "verified":
                    kept["state"] = "implemented_unverified"
                entries[row["id"]] = kept
        except CoverageError as exc:
            if strict:
                raise
            problems.append({"severity": "error", "kind": "invalid_manifest_entry", "id": row["id"],
                             "detail": str(exc)})
    return entries, problems


def load_manifest(path: Path, items: list[Item]) -> dict[str, dict]:
    return validate_manifest(path, items, strict=True)[0]


def counts(items: list[Item], entries: dict[str, dict]) -> Counter:
    return Counter(entries.get(item.id, {}).get("state", "unassessed") for item in items)


def percent(numerator: int, denominator: int) -> str:
    return "unknown" if denominator == 0 else f"{100 * numerator / denominator:.1f}%"


def summary(items: list[Item], entries: dict[str, dict]) -> str:
    c = counts(items, entries)
    known = len(items) - c["unassessed"]
    return f"{len(items)} listed · {known} classified · ≥{percent(c['verified'], len(items))} test verified"


def short_commit(metadata: dict | None) -> str | None:
    commit = (metadata or {}).get("assessed_commit")
    if not commit:
        return None
    return commit[:7] + ("+" if (metadata or {}).get("worktree") == "modified" else "")


def text_width(text: str, size: float) -> float:
    return len(text) * size * 0.6


def render_state_chips(lines: list[str], members: list[Item], entries: dict[str, dict], x: int, y: int) -> None:
    """One row of colour swatches with each state's count."""
    c = counts(members, entries)
    offset = 0.0
    for state in STATE_ORDER:
        label = f"{c[state]} {CHIP_LABELS[state]}"
        lines.append(f'<rect x="{x + offset:.1f}" y="{y - 8}" width="9" height="9" fill="{COLORS[state]}"/>')
        lines.append(f'<text x="{x + offset + 13:.1f}" y="{y}" fill="#c5d1dd" font-size="10" font-family="sans-serif">{escape(label)}</text>')
        offset += 13 + text_width(label, 10) + 10


def render_metadata(lines: list[str], metadata: dict | None, y: int) -> None:
    commit = short_commit(metadata)
    if commit:
        lines.append(f'<text x="1170" y="{y}" text-anchor="end" fill="#7d8b9b" font-size="12" font-family="sans-serif">assessed at {escape(commit)}</text>')


def group_height(members: list[Item], width: int) -> int:
    columns = max(1, min(36, width // 11))
    return 25 + ((len(members) + columns - 1) // columns) * 7


def render_groups(lines: list[str], groups: dict[str, list[Item]], entries: dict[str, dict],
                  x: int, y: int, width: int) -> None:
    for group, members in groups.items():
        band = group_height(members, width)
        c = counts(members, entries)
        short = group.removeprefix("xboxkrnl.exe / ")
        label = f"{short}  ·  {len(members)}  ·  {c['verified']} verified (≥{percent(c['verified'], len(members))})"
        lines.append(f'<rect x="{x-4}" y="{y}" width="{width+8}" height="{band-3}" rx="3" fill="#223044"/>')
        lines.append(f'<text x="{x}" y="{y+13}" fill="#e5edf6" font-size="12" font-family="sans-serif">{escape(label)}</text>')
        cols = max(1, min(len(members), 36, width // 11))
        cw = width / cols
        for j, item in enumerate(members):
            state = entries.get(item.id, {}).get("state", "unassessed")
            cx = x + (j % cols) * cw
            cy = y + 18 + (j // cols) * 7
            title = f"{item.id}: {state}; {item.source}"
            lines.append(f'<rect x="{cx:.2f}" y="{cy:.2f}" width="{cw-1:.2f}" height="6" fill="{COLORS[state]}"><title>{escape(title)}</title></rect>')
        y += band


def render_svg(items: list[Item], entries: dict[str, dict], metadata: dict | None = None) -> str:
    by_category: dict[str, list[Item]] = defaultdict(list)
    for item in items:
        by_category[item.category].append(item)
    total = counts(items, entries)
    overall = f"{total['verified']}/{len(items)} (≥{percent(total['verified'], len(items))}) test verified in the listed source subset"
    grouped: dict[str, list[dict[str, list[Item]]]] = {}
    panel_heights: dict[str, int] = {}
    for category in CATEGORIES:
        groups: dict[str, list[Item]] = defaultdict(list)
        for item in by_category[category]:
            groups[item.group].append(item)
        columns: list[dict[str, list[Item]]] = [{}, {}] if category == "kernel" else [{}]
        if category == "kernel":
            sizes = [0, 0]
            for group, members in sorted(groups.items(), key=lambda pair: (-len(pair[1]), pair[0])):
                side = 0 if sizes[0] <= sizes[1] else 1
                columns[side][group] = members
                sizes[side] += group_height(members, 256)
        else:
            columns[0] = groups
        grouped[category] = [dict(sorted(column.items())) for column in columns]
        content_height = max((sum(group_height(m, 256 if category == "kernel" else 520)
                                  for m in column.values()) for column in columns), default=0)
        panel_heights[category] = max(426, 96 + content_height)
    first_row = max(panel_heights["kernel"], panel_heights["ppc"])
    second_row = max(panel_heights["shader"], panel_heights["pm4"])
    height = 105 + first_row + 20 + second_row + 80
    lines = [f'<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="{height}" viewBox="0 0 1200 {height}" role="img" aria-labelledby="title desc">',
             '<title id="title">Xenon source coverage audit</title>',
             '<desc id="desc">Treemaps of audited source-declared operations. Percentages are verified lower bounds; unassessed entries remain unknown.</desc>',
             f'<rect width="1200" height="{height}" fill="#101721"/>',
             '<text x="30" y="49" fill="#f4f7fb" font-size="30" font-family="sans-serif" font-weight="700">XENON  /  IMPLEMENTATION AUDIT</text>',
             f'<text x="30" y="76" fill="#bdc9d6" font-size="15" font-family="sans-serif">Source-declared scope • {escape(overall)} • overall hardware coverage unknown</text>']
    render_metadata(lines, metadata, 49)
    titles = {"kernel": "Xbox kernel exports", "ppc": "PowerPC instructions", "shader": "Xenos shader instructions", "pm4": "GPU PM4 packets"}
    for index, category in enumerate(CATEGORIES):
        x = 30 + (index % 2) * 585
        y = 105 if index < 2 else 125 + first_row
        panel = by_category[category]
        lines.append(f'<rect x="{x}" y="{y}" width="555" height="{panel_heights[category]}" rx="8" fill="#1b2634" stroke="#405168"/>')
        lines.append(f'<text x="{x+16}" y="{y+30}" fill="#f4f7fb" font-size="21" font-family="sans-serif" font-weight="700">{titles[category]}</text>')
        lines.append(f'<text x="{x+16}" y="{y+52}" fill="#bdc9d6" font-size="12" font-family="sans-serif">{escape(summary(panel, entries))}</text>')
        render_state_chips(lines, panel, entries, x + 16, y + 72)
        if category == "kernel":
            for side, column in enumerate(grouped[category]):
                render_groups(lines, column, entries, x + 16 + side * 264, y + 84, 256)
        else:
            render_groups(lines, grouped[category][0], entries, x + 16, y + 84, 520)
    legend = [("verified", "Verified by test"), ("implemented_unverified", "Implemented, unverified"),
              ("partial", "Partial"), ("stub", "Stub"),
              ("unimplemented", "Unimplemented"), ("unassessed", "Unassessed")]
    for i, (state, label) in enumerate(legend):
        x = 31 + i * 193
        lines.append(f'<rect x="{x}" y="{height-42}" width="18" height="18" fill="{COLORS[state]}"/>')
        lines.append(f'<text x="{x+26}" y="{height-28}" fill="#e5edf6" font-size="13" font-family="sans-serif">{escape(label)}</text>')
    lines.append('</svg>')
    return "\n".join(lines) + "\n"


def render_summary_svg(items: list[Item], entries: dict[str, dict], metadata: dict | None = None) -> str:
    by_category: dict[str, list[Item]] = defaultdict(list)
    for item in items:
        by_category[item.category].append(item)
    total = counts(items, entries)
    titles = {"kernel": "Xbox 360 kernel exports", "ppc": "PowerPC instructions",
              "shader": "Xenos shader instructions", "pm4": "GPU PM4 packets"}
    height = 700
    lines = [f'<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="{height}" viewBox="0 0 1200 {height}" role="img" aria-labelledby="title desc">',
             '<title id="title">Xenon implementation coverage summary</title>',
             '<desc id="desc">Source-listed operations grouped by subsystem. Coloured bars show audited states; unassessed operations remain unknown.</desc>',
             f'<rect width="1200" height="{height}" fill="#101721"/>',
             '<text x="30" y="48" fill="#f4f7fb" font-size="29" font-weight="700" font-family="sans-serif">XENON  /  IMPLEMENTATION COVERAGE</text>',
             f'<text x="30" y="77" fill="#c5d1dd" font-size="16" font-family="sans-serif">{total["verified"]}/{len(items)} verified by tests (≥{percent(total["verified"], len(items))})  ·  {len(items)-total["unassessed"]}/{len(items)} classified  ·  source-listed scope</text>']
    render_metadata(lines, metadata, 48)
    for index, category in enumerate(CATEGORIES):
        x = 30 + (index % 2) * 585
        y = 102 + (index // 2) * 282
        members = by_category[category]
        c = counts(members, entries)
        lines.append(f'<rect x="{x}" y="{y}" width="555" height="266" rx="8" fill="#1b2634" stroke="#405168"/>')
        lines.append(f'<text x="{x+16}" y="{y+30}" fill="#f4f7fb" font-size="21" font-family="sans-serif" font-weight="700">{titles[category]}</text>')
        lines.append(f'<text x="{x+16}" y="{y+53}" fill="#c5d1dd" font-size="13" font-family="sans-serif">{len(members)} listed  ·  {len(members)-c["unassessed"]} classified  ·  {c["verified"]} verified (≥{percent(c["verified"], len(members))})</text>')
        render_state_chips(lines, members, entries, x + 16, y + 75)
        groups: dict[str, list[Item]] = defaultdict(list)
        for item in members:
            groups[item.group].append(item)
        group_rows = sorted(groups.items())
        if len(group_rows) > 14:
            extra = [operation for _, group_members in group_rows[13:]
                     for operation in group_members]
            group_rows = group_rows[:13] + [("Additional groups", extra)]
        for j, (group, operations) in enumerate(group_rows):
            col, row = j // 7, j % 7
            gx = x + 16 + col * 264
            gy = y + 92 + row * 23
            label = group.removeprefix("xboxkrnl.exe / ").replace("FloatingPoint", "Floating point")
            display_label = label if len(label) <= 16 else label[:14] + "…"
            lines.append(f'<text x="{gx}" y="{gy+11}" fill="#dce7f2" font-size="11" font-family="sans-serif">{escape(display_label)}</text>')
            group_counts = counts(operations, entries)
            bx, bar_width = gx + 113, 115
            present_states = [state for state in STATE_ORDER if group_counts[state]]
            available = bar_width - len(present_states)
            assigned = 0
            offset = 0
            for state in present_states:
                count = group_counts[state]
                next_edge = available * (assigned + count) // len(operations)
                start_edge = available * assigned // len(operations)
                cell_width = 1 + next_edge - start_edge
                lines.append(f'<rect x="{bx+offset}" y="{gy}" width="{cell_width}" height="13" fill="{COLORS[state]}"><title>{escape(label)}: {count} {state}</title></rect>')
                offset += cell_width
                assigned += count
            lines.append(f'<text x="{gx+253}" y="{gy+11}" text-anchor="end" fill="#dce7f2" font-size="11" font-family="sans-serif">{len(operations)}</text>')
    legend = (("verified", "Verified"), ("implemented_unverified", "Implemented, unverified"),
              ("partial", "Partial"), ("stub", "Stub"), ("unimplemented", "Unimplemented"),
              ("unassessed", "Unassessed"))
    for i, (state, label) in enumerate(legend):
        x = 30 + i * 193
        lines.append(f'<rect x="{x}" y="{height-43}" width="17" height="17" fill="{COLORS[state]}"/>')
        lines.append(f'<text x="{x+25}" y="{height-29}" fill="#e5edf6" font-size="13" font-family="sans-serif">{escape(label)}</text>')
    lines.append('</svg>')
    return "\n".join(lines) + "\n"


def describe_ci(metadata: dict | None) -> str:
    ci = (metadata or {}).get("ci", {})
    if not ci:
        return "none recorded"
    return ", ".join(f"{platform} `{(info.get('commit') or 'unknown')[:12]}` ({info.get('source', 'unknown')})"
                     for platform, info in sorted(ci.items()))


def render_report(items: list[Item], entries: dict[str, dict], metadata: dict | None = None,
                  approved: dict[str, dict] | None = None) -> str:
    total = counts(items, entries)
    reviewed = counts(items, approved if approved is not None else entries)
    commit = (metadata or {}).get("assessed_commit")
    lines = ["# Xenon coverage audit", "", "Generated by `python3 tools/coverage/generate.py` from source inventories and `coverage.json`.",
             "Percentages below are audited lower bounds against **source-declared** inventories. Unassessed entries are unknown, not unimplemented. They are not game compatibility figures.", "",
             f"Assessed revision: `{commit[:12] if commit else 'unknown'}`" + (" (with uncommitted changes)" if (metadata or {}).get("worktree") == "modified" else "")
             + f". CI evidence: {describe_ci(metadata)}. Machine-readable data: [`inventory.json`](inventory.json); review queue: [audit candidates](AUDIT.md).", "",
             "| Area | Listed | Classified | Verified by tests | Implemented, unverified | Partial | Stub | Unimplemented | Unassessed |", "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for category, name in (("kernel", "Kernel static registrations"), ("ppc", "PPC catalog"), ("shader", "Shader frontend"), ("pm4", "PM4 source enum")):
        group = [item for item in items if item.category == category]
        c = counts(group, entries)
        lines.append(f"| {name} | {len(group)} | {len(group)-c['unassessed']} | {c['verified']} (≥{percent(c['verified'],len(group))}) | {c['implemented_unverified']} | {c['partial']} | {c['stub']} | {c['unimplemented']} | {c['unassessed']} |")
    demoted = reviewed["verified"] - total["verified"]
    lines += ["", f"**Reviewed `verified` entries:** {reviewed['verified']}; {total['verified']} have a passing result for their required CTest target in the recorded CI evidence"
              + (f"; {demoted} are shown as implemented, unverified until that result is available and passing (see [AUDIT.md](AUDIT.md))." if demoted else "."),
              "", f"**Combined source-declared audit lower bound:** {total['verified']}/{len(items)} (≥{percent(total['verified'],len(items))}) have reviewed behavior-test evidence; {total['verified']+total['implemented_unverified']}/{len(items)} (≥{percent(total['verified']+total['implemented_unverified'],len(items))}) have traced implementations without a known partial limitation. **Audit progress:** {len(items)-total['unassessed']}/{len(items)} ({percent(len(items)-total['unassessed'],len(items))}) classified; {total['unassessed']} remain unassessed. **Overall Xbox 360 implementation coverage: unknown.** The kernel inventory extracts statically recoverable ordinal/name registrations and variables from xboxkrnl export, audio, and session sources. It is not a complete console export table: operations absent from Xenon source are outside the denominator, and optional audio exports depend on build configuration. PPC counts the decoder catalog, not every possible PowerPC encoding. Shader counts control-flow enum values and ALU/fetch forms recognized by this frontend; reserved scalar opcode 41 is excluded. PM4 counts declared packet headers and type-3 opcodes, not undocumented hardware values; host extension 0x64 is excluded. A handler, decoder entry, or passthrough packet does not by itself prove implementation.",
              "", "Implementation paths, test source tokens and CTest target names are in the manifest. The generator checks source traceability and target registration, not semantic correctness. `verified` means a behavior-oriented assertion was reviewed and traced to the entry under the [verification policy](VERIFICATION_POLICY.md), and the dashboard shows it as verified only while the required CTest target has a recorded passing CI result. It does not imply retail-title qualification. GitHub CI checks generated assets for drift and requires all selected CTest cases to run without skips or failures.", ""]
    return "\n".join(lines)


@dataclass
class Model:
    items: list[Item]
    approved: dict[str, dict]
    entries: dict[str, dict]
    signals: dict[str, dict]
    discovered: dict[str, list[str]]
    tagged: dict[str, list[str]]
    results: dict[str, dict[str, str]]
    ci: dict[str, dict]
    problems: list[dict]
    candidates: list[dict] = field(default_factory=list)
    records: list[dict] = field(default_factory=list)


def read_json(path: Path) -> dict | None:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    return data if isinstance(data, dict) else None


def build(manifest: Path = MANIFEST, ci_evidence: dict | None = None, prior: dict | None = None,
          strict: bool = True) -> Model:
    """Assemble the inventory, reviewed classifications and evidence."""
    items = inventory()
    targets = ctest_targets()
    approved, problems = validate_manifest(manifest, items, strict=strict, test_targets=targets)
    signals = source_signals(items)
    discovered, tagged, tag_problems = evidence.discover_tests(items, targets, source)
    if strict and tag_problems:
        raise CoverageError(tag_problems[0]["detail"] + f": {tag_problems[0]['id']}")
    problems += tag_problems
    referenced = {row["test_target"] for row in approved.values() if row.get("test_target")}
    results, ci, ci_problems = evidence.resolve_results(referenced, ci_evidence, prior)
    problems += ci_problems
    entries: dict[str, dict] = {}
    for ident, row in approved.items():
        effective = dict(row)
        if row["state"] == "verified":
            eligible, kind, detail = evidence.verification_outcome(row["test_target"], results)
            if not eligible:
                effective["state"] = "implemented_unverified"
                problems.append({"severity": "warning", "kind": kind, "id": ident,
                                 "detail": detail + "; shown as implemented, unverified"})
        entries[ident] = effective
    model = Model(items, approved, entries, signals, discovered, tagged, results, ci, problems)
    model.records = records(model)
    model.problems += audit.conflicts(model.records)
    model.problems.sort(key=lambda row: (["error", "warning", "info"].index(row["severity"]), row["kind"],
                                         row.get("id") or "", row["detail"]))
    model.candidates = audit.candidates(model.records)
    return model


def records(model: Model) -> list[dict]:
    rows = []
    for item in model.items:
        row = {"id": item.id, "category": item.category, "group": item.group, "name": item.name,
               "key": item.key, "source": item.source, "signals": model.signals.get(item.id, {}),
               "manifest_state": model.approved.get(item.id, {}).get("state", "unassessed"),
               "state": model.entries.get(item.id, {}).get("state", "unassessed")}
        entry = model.approved.get(item.id)
        if entry:
            row["evidence"] = {key: entry[key] for key in ("implementation", "implementation_token", "test",
                                                           "test_token", "test_target", "shared_test", "note")
                               if key in entry}
        if model.discovered.get(item.id):
            row["discovered_tests"] = model.discovered[item.id]
        if model.tagged.get(item.id):
            row["tagged_tests"] = model.tagged[item.id]
        rows.append(row)
    return rows


def area_counts(model: Model) -> dict[str, dict[str, int]]:
    result = {}
    for category in CATEGORIES:
        members = [item for item in model.items if item.category == category]
        c = counts(members, model.entries)
        reviewed = counts(members, model.approved)
        row = {"listed": len(members), "classified": len(members) - c["unassessed"]}
        row.update({state: c[state] for state in STATE_ORDER})
        row["reviewed_verified"] = reviewed["verified"]
        result[category] = row
    return result


def snapshot(model: Model) -> dict:
    """Content that determines whether generated files meaningfully changed."""
    return {"counts": area_counts(model), "test_results": model.results, "problems": model.problems,
            "candidates": model.candidates, "operations": model.records}


def digest(content: dict) -> str:
    canonical = json.dumps(content, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return "sha256:" + hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def git(*args: str) -> str | None:
    try:
        return subprocess.run(["git", *args], cwd=ROOT, check=True, capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def current_revision(source_commit: str | None) -> tuple[str | None, str]:
    if source_commit:
        return source_commit, "clean"
    head = git("rev-parse", "HEAD")
    status = git("status", "--porcelain", "--", "src", "include", "tests", "tools/coverage/coverage.json")
    return head, "unknown" if status is None else ("modified" if status else "clean")


def choose_metadata(content_digest: str, prior: dict | None, model: Model, source_commit: str | None,
                    fresh_evidence: bool) -> dict:
    """Reuse the committed metadata when nothing meaningful changed.

    A new revision with identical content keeps the earlier commit SHA so that
    repeat runs produce byte-identical files. Metadata is renewed when the
    content digest changes, or when fresh CI evidence for the current revision
    replaces evidence that was stale for the recorded one.
    """
    previous = (prior or {}).get("metadata") if isinstance(prior, dict) else None
    commit, worktree = current_revision(source_commit)
    renewed = {"assessed_commit": commit, "worktree": worktree, "ci": model.ci}
    if not isinstance(previous, dict) or (prior or {}).get("content_digest") != content_digest:
        return renewed
    stale = any(info.get("commit") != previous.get("assessed_commit") for info in previous.get("ci", {}).values())
    current = model.ci and all(info.get("commit") == commit for info in model.ci.values())
    if fresh_evidence and stale and current and worktree == "clean":
        return renewed
    return previous


def metadata_problems(metadata: dict) -> list[dict]:
    rows = []
    commit = metadata.get("assessed_commit")
    if not metadata.get("ci"):
        rows.append({"severity": "warning", "kind": "ci_missing", "id": None,
                     "detail": "no CI evidence has been recorded; reviewed verified entries cannot be confirmed"})
    for platform, info in sorted(metadata.get("ci", {}).items()):
        if commit and info.get("commit") != commit:
            rows.append({"severity": "warning", "kind": "ci_stale", "id": None,
                         "detail": f"{platform} results come from {(info.get('commit') or 'unknown')[:12]}, "
                                   f"not the assessed revision {commit[:12]}"})
    return rows


def render_inventory(content: dict, content_digest: str, metadata: dict) -> str:
    """inventory.json with one operation per line for readable diffs."""
    head = {"schema": 1,
            "description": "Source-declared Xenon operations with reviewed and effective coverage states. "
                           "Generated by tools/coverage/generate.py; do not edit.",
            "verification_policy": "docs/coverage/VERIFICATION_POLICY.md",
            "content_digest": content_digest, "metadata": metadata,
            "counts": content["counts"], "test_results": content["test_results"], "problems": content["problems"]}
    text = json.dumps(head, indent=2, sort_keys=True, ensure_ascii=False)
    operations = ",\n".join("    " + json.dumps(row, sort_keys=True, separators=(", ", ": "), ensure_ascii=False)
                            for row in content["operations"])
    return text[:-2] + ',\n  "operations": [\n' + operations + "\n  ]\n}\n"


def render_candidates(content: dict, content_digest: str, metadata: dict) -> str:
    data = {"schema": 1,
            "description": "Audit candidates and validation problems. Candidates are suggestions for review; "
                           "they are never applied to tools/coverage/coverage.json automatically.",
            "content_digest": content_digest,
            "metadata": {"assessed_commit": metadata.get("assessed_commit"), "worktree": metadata.get("worktree")},
            "problems": content["problems"] + metadata_problems(metadata)}
    text = json.dumps(data, indent=2, sort_keys=True, ensure_ascii=False)
    rows = ",\n".join("    " + json.dumps(row, sort_keys=True, separators=(", ", ": "), ensure_ascii=False)
                       for row in content["candidates"])
    return text[:-2] + ',\n  "candidates": [\n' + rows + "\n  ]\n}\n"


def products(model: Model, metadata: dict, content: dict | None = None) -> dict[Path, str]:
    content = content or snapshot(model)
    content_digest = digest(content)
    provenance, reference = load_kernel_reference()
    reconciliation = reconcile_kernel_reference(reference, kernel_registration_rows())
    audit_view = dict(content, metadata=metadata)
    return {SVG: render_svg(model.items, model.entries, metadata),
            SUMMARY_SVG: render_summary_svg(model.items, model.entries, metadata),
            REPORT: render_report(model.items, model.entries, metadata, model.approved),
            KERNEL_REFERENCE_REPORT: render_kernel_reference_report(provenance, reference, reconciliation),
            INVENTORY_JSON: render_inventory(content, content_digest, metadata),
            CANDIDATES_JSON: render_candidates(content, content_digest, metadata),
            AUDIT_REPORT: audit.render_audit(audit_view, metadata_problems(metadata))}


def committed_prior() -> dict | None:
    return read_json(INVENTORY_JSON)


def generate(manifest: Path = MANIFEST, ci_evidence: dict | None = None, source_commit: str | None = None,
             strict: bool = True, prior: dict | None = None) -> tuple[Model, dict, dict[Path, str]]:
    prior = committed_prior() if prior is None else prior
    model = build(manifest, ci_evidence, prior, strict)
    content = snapshot(model)
    metadata = choose_metadata(digest(content), prior, model, source_commit, ci_evidence is not None)
    return model, metadata, products(model, metadata, content)


DASHBOARD = (SVG, SUMMARY_SVG, REPORT, KERNEL_REFERENCE_REPORT)
METADATA_LINE = re.compile(r'^(?:<text [^>]*>assessed at [^<]*</text>|Assessed revision: .*)$', re.M)


def stale_products(outputs: dict[Path, str]) -> list[str]:
    return [str(path.relative_to(ROOT)) for path, data in outputs.items()
            if not path.is_file() or path.read_text(encoding="utf-8") != data]


def classify_drift(outputs: dict[Path, str]) -> tuple[list[str], list[str]]:
    """Split stale outputs into (blocking, refreshable).

    Dashboard content drift blocks CI as it always has. Drift confined to the
    assessed-revision line, or to the audit records (inventory.json,
    audit-candidates.json, AUDIT.md), is refreshed by the coverage-refresh
    workflow and is reported without failing.
    """
    blocking, refreshable = [], []
    for name in stale_products(outputs):
        path = ROOT / name
        if path in DASHBOARD and path.is_file() and (
                METADATA_LINE.sub("", path.read_text(encoding="utf-8")) == METADATA_LINE.sub("", outputs[path])):
            refreshable.append(name)
        elif path in DASHBOARD:
            blocking.append(name)
        else:
            refreshable.append(name)
    return blocking, refreshable


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="fail if generated files differ")
    mode.add_argument("--validate", action="store_true", help="validate inventories and the manifest without writing")
    parser.add_argument("--manifest", type=Path, default=MANIFEST)
    parser.add_argument("--ci-evidence", type=Path, help="CI evidence JSON from collect_ci_evidence.py")
    parser.add_argument("--source-commit", help="revision being assessed (default: git HEAD)")
    args = parser.parse_args()
    try:
        ci_evidence = evidence.read_evidence_file(args.ci_evidence)
        model, metadata, outputs = generate(args.manifest, ci_evidence, args.source_commit)
        if args.check:
            blocking, refreshable = classify_drift(outputs)
            errors = [row for row in model.problems if row["severity"] == "error"]
            if blocking:
                raise CoverageError("stale generated assets: " + ", ".join(blocking)
                                    + "; run python3 tools/coverage/generate.py and commit the result")
            if refreshable:
                prefix = "::warning::" if os.environ.get("GITHUB_ACTIONS") == "true" else "warning: "
                print(f"{prefix}audit records or the assessed-revision line are out of date ("
                      + ", ".join(refreshable) + "); the coverage-refresh workflow proposes the update")
            if errors:
                raise CoverageError(errors[0]["detail"])
        elif not args.validate:
            for path, data in outputs.items():
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(data, encoding="utf-8", newline="\n")
        for category in CATEGORIES:
            print(f"{category}: {summary([i for i in model.items if i.category == category], model.entries)}")
        provenance, reference = load_kernel_reference()
        matched = reconcile_kernel_reference(reference, kernel_registration_rows())["matched"]
        print(f"kernel reference: {len(matched)}/{len(reference)} exact registrations")
        problems = model.problems + metadata_problems(metadata)
        print(f"audit: {len(model.candidates)} candidates, {len(problems)} validation problems "
              f"(assessed {short_commit(metadata) or 'unknown'})")
        return 0
    except (CoverageError, ValueError) as exc:
        print(f"coverage error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

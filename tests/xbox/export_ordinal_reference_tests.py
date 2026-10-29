"""Every export the production registry holds has the right ordinal.

Runs `import-scanner --registry` (the registry XenonSession uses, not a
source grep) and checks each registered (library, ordinal, name, kind)
against the pinned research tables in tools/coverage/reference/. A guest
import names only an ordinal, so a wrong ordinal sends a title's call to a
different export's handler: XamVoiceClose, XamVoiceHeadsetPresent and
XamVoiceSubmitPacket were registered one place off (0x30D-0x30F) until this
test was added. It also requires every export registered as stubbed or
partial to carry a note saying what it does not do.

usage: export_ordinal_reference_tests.py <import-scanner> <reference-dir>
"""

import json
import os
import subprocess
import sys
from pathlib import Path

REFERENCES = {
    "xboxkrnl": "xenia_xboxkrnl_997d055.json",
    "xam": "xenia_xam_997d055.json",
}
LIBRARY_ALIASES = {"xboxkrnl.exe": "xboxkrnl", "xam.xex": "xam"}


def load_reference(path):
    exports = json.loads(Path(path).read_text(encoding="utf-8"))["exports"]
    return {row["ordinal"]: (row["name"], row["kind"]) for row in exports}


def parse_registry(text):
    rows = []
    for line in text.splitlines():
        fields = line.split("\t")
        if len(fields) < 6 or fields[0] == "library" or fields[0].startswith("["):
            continue
        library, ordinal, name, kind = fields[0], int(fields[1]), fields[2], fields[3]
        rows.append((library, ordinal, name, kind))
    return rows


def unexplained_gaps(text):
    """Exports registered as stubbed or partial without a note saying why."""
    gaps = []
    for line in text.splitlines():
        fields = line.split("\t")
        if len(fields) < 6 or fields[0] == "library" or fields[0].startswith("["):
            continue
        note = fields[6].strip() if len(fields) > 6 else ""
        if (fields[4] == "stubbed" or fields[5] == "1") and not note:
            gaps.append(f"{fields[0]} 0x{int(fields[1]):X} {fields[2]}")
    return gaps


def check(rows, references):
    """Returns one message per registered export that disagrees with its reference."""
    problems = []
    seen = {}
    for library, ordinal, name, kind in rows:
        canonical = LIBRARY_ALIASES.get(library, library)
        reference = references.get(canonical)
        if reference is None:
            problems.append(f"{library} {name}: no reference table for this library")
            continue
        key = (canonical, ordinal)
        if key in seen and seen[key] != name:
            problems.append(f"{canonical} 0x{ordinal:X}: registered as both {seen[key]} and {name}")
        seen[key] = name
        expected = reference.get(ordinal)
        if expected is None:
            problems.append(f"{canonical} 0x{ordinal:X} {name}: ordinal is not in the reference")
        elif expected != (name, kind):
            problems.append(f"{canonical} 0x{ordinal:X}: registered {name} ({kind}), "
                            f"reference {expected[0]} ({expected[1]})")
    return problems


def test_check_reports_disagreements(references):
    rows = [("xam", 0x30F, "XamVoiceSubmitPacket", "function"),
            ("xboxkrnl.exe", 1, "DbgBreakPoint", "function"),
            ("xboxkrnl", 1, "KeBugCheck", "function"),
            ("xbdm", 1, "DmFoo", "function")]
    problems = check(rows, references)
    assert any("0x30F" in p and "XamVoiceClose" in p for p in problems), problems
    assert any("both DbgBreakPoint and KeBugCheck" in p for p in problems), problems
    assert any("xbdm" in p for p in problems), problems
    assert not any("DbgBreakPoint (function), reference" in p for p in problems), problems


def main():
    scanner, reference_dir = sys.argv[1], Path(sys.argv[2])
    references = {library: load_reference(reference_dir / name)
                  for library, name in REFERENCES.items()}
    test_check_reports_disagreements(references)

    environment = dict(os.environ)
    environment.setdefault("SDL_AUDIODRIVER", "dummy")
    result = subprocess.run([scanner, "--registry"], capture_output=True, text=True,
                            env=environment, timeout=300)
    assert result.returncode == 0, result.stderr
    rows = parse_registry(result.stdout)
    assert len(rows) > 300, f"registry dump looks truncated: {len(rows)} rows\n{result.stderr}"
    for line in result.stderr.splitlines():
        if "registry dumped from" in line:
            print(line)

    problems = check(rows, references)
    for problem in problems:
        print("MISMATCH", problem)
    assert not problems, f"{len(problems)} registered exports disagree with the reference"
    print(f"{len(rows)} registered exports match the reference ordinal tables")

    # A stubbed or partial export must say what it does not do.
    gaps = unexplained_gaps(result.stdout)
    for gap in gaps:
        print("NO NOTE", gap)
    assert not gaps, f"{len(gaps)} stubbed or partial exports have no partial_note"


if __name__ == "__main__":
    main()

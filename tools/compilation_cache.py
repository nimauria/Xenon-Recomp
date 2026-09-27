#!/usr/bin/env python3
"""Gen 10 verified native-object cache and explicit cache maintenance.

Used as CMake CXX_COMPILER_LAUNCHER for generated regions. Preprocessing always
runs, so include changes (including newly selected headers) cannot be hidden by
a stale dependency list. Unsupported compiler modes fail open to compilation,
never to reuse. No third party Python modules are required.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import uuid

SCHEMA = 1

def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

def canonical(value) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()

def file_hash(path: Path) -> str:
    with path.open("rb") as stream:
        h = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
        return h.hexdigest()

def verified(entry: Path, request=None):
    try:
        manifest = json.loads((entry / "manifest.json").read_bytes())
        if manifest.get("schema") != SCHEMA or manifest.get("verified") is not True:
            return None
        if request is not None and manifest["request"] != request:
            return None
        if sha(canonical(manifest["request"])) != entry.name:
            return None
        if file_hash(entry / "payload") != manifest["sha256"]:
            return None
        return manifest
    except (OSError, ValueError, KeyError, TypeError):
        return None

def promote(root: Path, request: dict, payload: Path):
    key = sha(canonical(request))
    entry = root / "objects" / key
    (root / "staging").mkdir(parents=True, exist_ok=True)
    entry.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=key + "-", dir=root / "staging"))
    try:
        shutil.copyfile(payload, stage / "payload")
        manifest = {"schema": SCHEMA, "request": request, "sha256": file_hash(payload), "verified": True}
        if file_hash(stage / "payload") != manifest["sha256"]:
            raise OSError("object changed during promotion")
        (stage / "manifest.json").write_bytes(canonical(manifest))
        # Flush both files before publishing. An interrupted private stage is
        # unreachable. Directory rename is on the same filesystem.
        for path in stage.iterdir():
            with path.open("r+b") as stream:
                os.fsync(stream.fileno())
        try:
            stage.rename(entry)
        except OSError:
            if verified(entry, request):
                return entry
            if entry.exists():
                entry.rename(root / "staging" / (key + "-corrupt-" + uuid.uuid4().hex))
            try:
                stage.rename(entry)
            except OSError:
                if not verified(entry, request):
                    raise
        return entry
    finally:
        if stage.exists():
            shutil.rmtree(stage)

def compiler_identity(compiler: str):
    path = Path(shutil.which(compiler) or compiler).resolve(strict=True)
    files = {str(path): file_hash(path)}
    if path.name.lower() in ("cl.exe", "clang-cl.exe"):
        for sibling in sorted(path.parent.iterdir()):
            if sibling.suffix.lower() in (".exe", ".dll"):
                files[str(sibling)] = file_hash(sibling)
    else:
        for tool in ("cc1plus", "as"):
            found = subprocess.check_output([str(path), "-print-prog-name=" + tool], text=True).strip()
            candidate = Path(shutil.which(found) or found)
            if not candidate.is_file():
                raise OSError("compiler helper unavailable: " + tool)
            files[str(candidate.resolve())] = file_hash(candidate)
    return files

def compile_cached(root: Path, argv: list[str]) -> int:
    if not argv:
        raise ValueError("compiler command required")
    start = time.monotonic()
    msvc = Path(argv[0]).name.lower() in ("cl.exe", "clang-cl.exe")
    args = argv[1:]
    source = [x for x in args if x.lower().endswith((".cpp", ".cc", ".cxx"))]
    # PCH, modules, debug databases, LTO and response files can have additional
    # outputs/hidden inputs. Execute them normally until their contracts exist.
    unsafe = any(x.startswith("@") or x.lower().startswith(("/yu", "/yc", "/zi", "/gl", "/sourcedependencies", "-fplugin", "-fprofile", "-flto", "-g", "-fmodules", "-include-pch")) for x in args)
    if len(source) != 1 or unsafe:
        return subprocess.call(argv)
    output = None
    normalized, pp = [], [argv[0]]
    skip = False
    for i, arg in enumerate(args):
        if skip:
            skip = False
            continue
        lower = arg.lower()
        if (msvc and lower.startswith("/fo")):
            output = Path(arg[3:] if len(arg) > 3 else args[i+1]); skip = len(arg) == 3
            continue
        if not msvc and arg == "-o":
            output = Path(args[i+1]); skip = True
            continue
        if (msvc and lower.startswith("/fd")):
            skip = len(arg) == 3
            continue
        if arg in ("-c", "/c"):
            continue
        pp.append(arg)
        if not msvc and arg in ("-MF", "-MT", "-MQ"):
            pp.append(args[i+1]); skip = True
            continue
        normalized.append("<source>" if arg == source[0] else arg)
    if output is None:
        return subprocess.call(argv)
    pp.append("/EP" if msvc else "-E")
    preprocessed = subprocess.run(pp, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if preprocessed.returncode:
        sys.stdout.buffer.write(preprocessed.stdout); sys.stderr.buffer.write(preprocessed.stderr)
        return preprocessed.returncode
    # /showIncludes is required by Ninja. Replay the current preprocessor's
    # dependency lines on a cache hit, not the previous build's include list.
    lines = preprocessed.stdout.splitlines(keepends=True)
    includes = [line for line in lines if line.lstrip().startswith(b"Note: including file:")]
    text = b"".join(line for line in lines if line not in includes) if msvc else preprocessed.stdout
    try:
        toolchain = compiler_identity(argv[0])
    except (OSError, subprocess.SubprocessError):
        return subprocess.call(argv)
    request = {"schema": SCHEMA, "stage": "object", "producer": "native-launcher-1",
               "preprocessed": sha(text), "source": file_hash(Path(source[0])), "dependencies": [sha(text)], "options": normalized, "toolchain": toolchain,
               "environment": {k: os.environ.get(k, "") for k in ("INCLUDE", "LIB", "LIBPATH", "CL", "_CL_", "CPATH", "CPLUS_INCLUDE_PATH", "SDKROOT", "MACOSX_DEPLOYMENT_TARGET", "SOURCE_DATE_EPOCH", "PATH")}}
    entry = root / "objects" / sha(canonical(request))
    hit = verified(entry, request)
    if hit:
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary = output.with_name(output.name + "." + uuid.uuid4().hex + ".tmp")
        shutil.copyfile(entry / "payload", temporary)
        os.replace(temporary, output)
        if msvc:
            sys.stdout.buffer.write(b"".join(includes))
        sys.stderr.buffer.write(preprocessed.stderr)
    else:
        status = subprocess.call(argv)
        if status:
            return status
        if not output.is_file():
            return 1
        entry = promote(root, request, output)
    # Per-output receipts avoid concurrent append races and permit inspection.
    receipt = {"node": entry.name, "output": str(output.resolve()), "sha256": file_hash(output), "hit": bool(hit), "seconds": time.monotonic()-start}
    receipts = root / "receipts"
    receipts.mkdir(parents=True, exist_ok=True)
    receipt_path = receipts / (sha(str(output.resolve()).encode()) + ".json")
    tmp = receipt_path.with_suffix("." + uuid.uuid4().hex + ".tmp")
    tmp.write_bytes(canonical(receipt)); os.replace(tmp, receipt_path)
    print("[Graph object] " + ("hit " if hit else "built ") + source[0], file=sys.stderr)
    return 0

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("compile", "inspect", "verify", "prune-corrupt"))
    parser.add_argument("root", type=Path)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    opts = parser.parse_args()
    if opts.operation == "compile":
        return compile_cached(opts.root, opts.command)
    # Explicit maintenance only. Staging/live worker directories are untouched.
    valid, invalid = [], []
    for entry in sorted((opts.root / "objects").glob("*")):
        (valid if verified(entry) else invalid).append(entry.name)
    if opts.operation == "prune-corrupt":
        root = (opts.root / "objects").resolve()
        for key in invalid:
            entry = root / key
            if entry.is_symlink() or entry.resolve().parent != root:
                raise ValueError("unsafe cache entry path")
            shutil.rmtree(entry)
    print(json.dumps({"valid": valid, "invalid": invalid}, sort_keys=True))
    return int(bool(invalid) and opts.operation == "verify")

if __name__ == "__main__":
    raise SystemExit(main())


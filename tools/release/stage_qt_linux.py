#!/usr/bin/env python3
"""Stage a relocatable Qt runtime for Xenon's Linux installer.

Qt's CMake QML deployment support intentionally doesn't bundle all shared Qt
runtime libraries on Linux. Official Xenon release builds use this script from
the install graph to copy only the Qt modules/plugins needed by the launcher,
then rewrite RUNPATHs so no matching system Qt installation is required.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from collections import deque
from pathlib import Path


class StageError(RuntimeError):
    pass


def _copy_dereferenced(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src.resolve(), dst)


def _copy_tree(source: Path, destination: Path) -> None:
    if source.is_dir():
        shutil.copytree(source, destination, dirs_exist_ok=True, symlinks=False)


def _copy_selected_plugins(qt_plugins: Path, destination: Path) -> list[Path]:
    copied: list[Path] = []
    # Keep the launcher desktop-focused. In particular, don't copy SQL,
    # multimedia or other optional plugins which would introduce unrelated host
    # dependencies and source-offer obligations.
    plugin_patterns = {
        "platforms": ("libqxcb.so",),
        "platforminputcontexts": ("*.so",),
        "xcbglintegrations": ("*.so",),
        "imageformats": ("libqgif.so", "libqico.so", "libqjpeg.so", "libqsvg.so"),
        "iconengines": ("libqsvgicon.so",),
        "networkinformation": ("*.so",),
        "tls": ("*.so",),
    }
    for family, patterns in plugin_patterns.items():
        src_dir = qt_plugins / family
        if not src_dir.is_dir():
            continue
        for pattern in patterns:
            for src in sorted(src_dir.glob(pattern)):
                if not src.is_file():
                    continue
                dst = destination / family / src.name
                _copy_dereferenced(src, dst)
                copied.append(dst)
    return copied


def _copy_qml_modules(qt_qml: Path, destination: Path) -> list[Path]:
    for module in ("QtQuick", "QtQml", "QtCore"):
        _copy_tree(qt_qml / module, destination / module)
    return [p for p in destination.rglob("*.so*") if p.is_file()]


def _ldd_qt_dependencies(binary: Path, qt_lib: Path) -> dict[str, Path]:
    env = os.environ.copy()
    existing = env.get("LD_LIBRARY_PATH", "")
    env["LD_LIBRARY_PATH"] = str(qt_lib) + ((":" + existing) if existing else "")
    try:
        proc = subprocess.run(
            ["ldd", str(binary)], text=True, capture_output=True, env=env, check=True
        )
    except (FileNotFoundError, subprocess.CalledProcessError) as exc:
        detail = getattr(exc, "stderr", "") or ""
        raise StageError(f"ldd failed for {binary}: {detail.strip()}") from exc

    result: dict[str, Path] = {}
    # Example: libQt6Core.so.6 => /opt/Qt/6.10.3/gcc_64/lib/libQt6Core.so.6 (0x...)
    matcher = re.compile(r"^\s*(libQt6[^\s]+)\s+=>\s+([^\s]+)")
    for line in proc.stdout.splitlines():
        match = matcher.match(line)
        if not match:
            continue
        soname, raw_path = match.groups()
        path = Path(raw_path)
        if path.is_file():
            try:
                path.resolve().relative_to(qt_lib.resolve())
            except ValueError:
                # A system Qt would make the release non-reproducible. Fail
                # rather than silently mixing it with the pinned Qt SDK.
                raise StageError(f"{binary} resolved {soname} outside pinned Qt root: {path}")
            result[soname] = path
    return result


def _stage_qt_library_closure(roots: list[Path], qt_lib: Path, private_root: Path) -> list[Path]:
    queue: deque[Path] = deque(roots)
    scanned: set[Path] = set()
    staged: dict[str, Path] = {}
    while queue:
        binary = queue.popleft()
        resolved = binary.resolve()
        if resolved in scanned:
            continue
        scanned.add(resolved)
        for soname, source in _ldd_qt_dependencies(binary, qt_lib).items():
            if soname in staged:
                continue
            destination = private_root / soname
            _copy_dereferenced(source, destination)
            staged[soname] = destination
            queue.append(destination)
    return list(staged.values())


def _set_runpath(path: Path, private_root: Path, bindir: Path, patchelf: str) -> None:
    if path.parent == bindir:
        runpath = "$ORIGIN:$ORIGIN/../lib/xenon-recomp"
    else:
        rel_private = os.path.relpath(private_root, path.parent).replace(os.sep, "/")
        rel_bin = os.path.relpath(bindir, path.parent).replace(os.sep, "/")
        runpath = f"$ORIGIN/{rel_private}:$ORIGIN/{rel_bin}"
    try:
        subprocess.run([patchelf, "--set-rpath", runpath, str(path)], check=True)
    except subprocess.CalledProcessError as exc:
        raise StageError(f"unable to set RUNPATH on {path}") from exc


def stage(root: Path, qt_root: Path) -> dict[str, object]:
    root = root.resolve()
    qt_root = qt_root.resolve()
    bindir = root / "bin"
    launcher = bindir / "xenon_launcher"
    if not launcher.is_file():
        raise StageError(f"xenon_launcher is not installed under {bindir}")

    qt_lib = qt_root / "lib"
    qt_plugins = qt_root / "plugins"
    qt_qml = qt_root / "qml"
    if not qt_lib.is_dir() or not qt_plugins.is_dir() or not qt_qml.is_dir():
        raise StageError(f"Qt runtime layout is incomplete: {qt_root}")

    private_root = root / "lib" / "xenon-recomp"
    plugins_root = private_root / "plugins"
    qml_root = private_root / "qml"
    private_root.mkdir(parents=True, exist_ok=True)

    plugin_files = _copy_selected_plugins(qt_plugins, plugins_root)
    qml_plugin_files = _copy_qml_modules(qt_qml, qml_root)
    if not (plugins_root / "platforms" / "libqxcb.so").is_file():
        raise StageError("Qt xcb platform plugin was not staged")
    if not (qml_root / "QtQuick").is_dir():
        raise StageError("QtQuick QML modules were not staged")

    # Calculate the shared Qt library closure from the real launcher plus every
    # copied plugin. This avoids shipping unrelated Qt modules while ensuring
    # nested QML plugins have all of their Qt dependencies.
    qt_libraries = _stage_qt_library_closure(
        [launcher, *plugin_files, *qml_plugin_files], qt_lib, private_root
    )
    if not any(path.name.startswith("libQt6Core.so") for path in qt_libraries):
        raise StageError("Qt6Core was not discovered in the launcher dependency closure")

    patchelf = shutil.which("patchelf")
    if not patchelf:
        raise StageError("patchelf is required to create a relocatable Linux package")
    _set_runpath(launcher, private_root, bindir, patchelf)
    for elf in [*qt_libraries, *plugin_files, *qml_plugin_files]:
        _set_runpath(elf, private_root, bindir, patchelf)

    (bindir / "qt.conf").write_text(
        "[Paths]\n"
        "Prefix=..\n"
        "Libraries=lib/xenon-recomp\n"
        "Plugins=lib/xenon-recomp/plugins\n"
        "QmlImports=lib/xenon-recomp/qml\n"
        "Translations=share/qt6/translations\n",
        encoding="utf-8",
    )

    return {
        "root": str(root),
        "qt_root": str(qt_root),
        "qt_libraries": len(qt_libraries),
        "plugin_files": len(plugin_files),
        "qml_plugin_files": len(qml_plugin_files),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=Path, required=True)
    ap.add_argument("--qt-root", type=Path, required=True)
    args = ap.parse_args()
    try:
        result = stage(args.root, args.qt_root)
    except StageError as exc:
        print(f"Xenon Qt staging failed: {exc}", file=sys.stderr)
        return 2
    print(
        "Staged Qt Linux runtime: "
        f"{result['qt_libraries']} libraries, {result['plugin_files']} platform/plugin files, "
        f"{result['qml_plugin_files']} QML plugin files"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

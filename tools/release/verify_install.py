#!/usr/bin/env python3
"""Fail a public release if the installed Xenon tree is missing runtime pieces."""
from __future__ import annotations

import argparse
import json
from pathlib import Path


def any_named(directory: Path, patterns: tuple[str, ...], *, recursive: bool = False) -> bool:
    if not directory.exists():
        return False
    matcher = directory.rglob if recursive else directory.glob
    return any(any(matcher(pattern)) for pattern in patterns)


def verify_install(
    root: Path,
    platform: str,
    *,
    require_audio: bool = False,
    require_dxc: bool = False,
    require_vulkan: bool = False,
) -> list[str]:
    root = root.resolve()
    bindir = root / "bin"
    missing: list[str] = []

    exe = ".exe" if platform == "windows" else ""
    for name in (f"xenon_launcher{exe}", f"xenon_runtime_host{exe}"):
        if not (bindir / name).is_file():
            missing.append(f"bin/{name}")

    if require_audio:
        if platform == "windows":
            if not any_named(bindir, ("avcodec-*.dll", "libavcodec*.dll")):
                missing.append("FFmpeg avcodec runtime")
            if not any_named(bindir, ("avutil-*.dll", "libavutil*.dll")):
                missing.append("FFmpeg avutil runtime")
        else:
            if not any_named(bindir, ("libavcodec.so*",)):
                missing.append("FFmpeg libavcodec runtime")
            if not any_named(bindir, ("libavutil.so*",)):
                missing.append("FFmpeg libavutil runtime")

    if require_dxc:
        dxc_patterns = ("dxcompiler.dll",) if platform == "windows" else ("libdxcompiler.so*",)
        if not any_named(bindir, dxc_patterns):
            missing.append("DXC runtime")

    if require_vulkan:
        vulkan_patterns = ("vulkan-1.dll",) if platform == "windows" else ("libvulkan.so*",)
        if not any_named(bindir, vulkan_patterns):
            missing.append("managed Vulkan loader runtime")

    # Qt deployment is mandatory for the public launcher package. Search the
    # complete tree because Windows Qt deploy and Linux private-runtime staging
    # intentionally use different standard layouts.
    if platform == "windows":
        if not any_named(root, ("Qt6Core.dll", "Qt6Cored.dll"), recursive=True):
            missing.append("Qt6Core runtime")
        if not any_named(root, ("qwindows.dll",), recursive=True):
            missing.append("Qt Windows platform plugin")
    else:
        if not any_named(root, ("libQt6Core.so*",), recursive=True):
            missing.append("Qt6Core runtime")
        if not any_named(root, ("libqxcb.so", "libqwayland*.so"), recursive=True):
            missing.append("Qt Linux platform plugin")
        if not (bindir / "qt.conf").is_file():
            missing.append("bin/qt.conf")

    licenses = root / "share" / "licenses" / "Xenon"
    for notice in ("LICENSE", "THIRD_PARTY_NOTICES.md", "THIRD_PARTY_SOURCE_OFFER.md"):
        if not (licenses / notice).is_file():
            missing.append(f"share/licenses/Xenon/{notice}")

    # These dependency license directories are part of the release contract,
    # not optional documentation. Their absence means the package should not be
    # published even if its binaries happen to run.
    for license_dir in ("xenia-ffmpeg", "dxc", "vulkan-loader", "vulkan-headers", "sdl2", "qt6"):
        path = licenses / license_dir
        if not path.is_dir() or not any(p.is_file() for p in path.rglob("*")):
            missing.append(f"third-party license material: {license_dir}")

    return missing


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=Path, required=True)
    ap.add_argument("--platform", choices=("windows", "linux"), required=True)
    ap.add_argument("--require-audio", action="store_true")
    ap.add_argument("--require-dxc", action="store_true")
    ap.add_argument("--require-vulkan", action="store_true")
    args = ap.parse_args()
    missing = verify_install(
        args.root,
        args.platform,
        require_audio=args.require_audio,
        require_dxc=args.require_dxc,
        require_vulkan=args.require_vulkan,
    )
    report = {"root": str(args.root.resolve()), "ok": not missing, "missing": missing}
    print(json.dumps(report, indent=2))
    return 0 if not missing else 2


if __name__ == "__main__":
    raise SystemExit(main())

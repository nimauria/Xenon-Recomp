#!/usr/bin/env python3
"""Pinned dependency bootstrapper for Xenon Recomp.

This tool owns reproducible build/release dependencies that need exact behaviour
not reliably provided by a distro package. SDL2 is built static; the XMA-capable
xenia-project FFmpeg fork is built shared and staged beside Xenon release binaries
so end users do not need local SDL/FFmpeg installations while LGPL redistribution
remains straightforward. The committed Windows x64 static FFmpeg bundle remains a
developer fallback; managed release presets use this bootstrap instead.

Network access is only required when a pinned source checkout is not already in
.xenon/deps/.cache/src. Configure can invoke this tool automatically, or it can
be run explicitly:

  python tools/deps/bootstrap.py ensure
  python tools/deps/bootstrap.py status
  python tools/deps/bootstrap.py verify
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import tarfile
import urllib.request
import zipfile
from pathlib import Path
from typing import Iterable, Sequence

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
MANIFEST_PATH = SCRIPT_DIR / "manifest.json"
DEFAULT_MANAGED_BASE = REPO_ROOT / ".xenon" / "deps"
STAMP_NAME = ".xenon-dependency.json"


class BootstrapError(RuntimeError):
    pass


def _load_manifest() -> dict:
    data = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    if data.get("schema") != 3 or not isinstance(data.get("dependencies"), dict):
        raise BootstrapError(f"Unsupported dependency manifest: {MANIFEST_PATH}")
    return data


def normalize_arch(value: str) -> str:
    value = value.strip().lower().replace("_", "-")
    if value in {"x86-64", "x86_64", "amd64", "x64"}:
        return "x64"
    if value in {"aarch64", "arm64"}:
        return "arm64"
    if value in {"i386", "i486", "i586", "i686", "x86"}:
        return "x86"
    return value


def host_triplet() -> str:
    if sys.platform.startswith("win"):
        os_name = "windows"
    elif sys.platform.startswith("linux"):
        os_name = "linux"
    elif sys.platform == "darwin":
        os_name = "macos"
    else:
        os_name = sys.platform.replace(" ", "-")
    return f"{os_name}-{normalize_arch(platform.machine())}"


def _run(cmd: Sequence[str], *, cwd: Path | None = None, env: dict | None = None,
         quiet: bool = False) -> None:
    if not quiet:
        where = f" (cwd={cwd})" if cwd else ""
        print("+", " ".join(str(x) for x in cmd) + where, flush=True)
    try:
        subprocess.run(list(map(str, cmd)), cwd=cwd, env=env, check=True)
    except FileNotFoundError as exc:
        raise BootstrapError(f"Required tool not found: {cmd[0]}") from exc
    except subprocess.CalledProcessError as exc:
        raise BootstrapError(f"Command failed with exit code {exc.returncode}: {' '.join(map(str, cmd))}") from exc


def _capture(cmd: Sequence[str], *, cwd: Path | None = None) -> str:
    try:
        return subprocess.check_output(list(map(str, cmd)), cwd=cwd, text=True, stderr=subprocess.STDOUT).strip()
    except (FileNotFoundError, subprocess.CalledProcessError) as exc:
        raise BootstrapError(f"Unable to run {' '.join(map(str, cmd))}") from exc


def _which_any(names: Iterable[str]) -> str | None:
    for name in names:
        value = shutil.which(name)
        if value:
            return value
    return None


def dependency_prefix(root: Path, manifest_entry: dict) -> Path:
    return root / manifest_entry["install_subdir"]


def source_dir(cache_base: Path, key: str, revision: str) -> Path:
    return cache_base / "src" / f"{key}-{revision[:12]}"


def build_dir(cache_base: Path, triplet: str, key: str, revision: str) -> Path:
    return cache_base / "build" / triplet / f"{key}-{revision[:12]}"


def _manifest_sha256() -> str:
    return hashlib.sha256(MANIFEST_PATH.read_bytes()).hexdigest()


def _stamp_payload(key: str, entry: dict, triplet: str) -> dict:
    return {
        "schema": 3,
        "dependency": key,
        "name": entry["name"],
        "repository": entry["repository"],
        "revision": entry["revision"],
        "version": entry["version"],
        "triplet": triplet,
        "license": entry["license"],
        "linkage": entry["linkage"],
        "manifest_sha256": _manifest_sha256(),
    }


def _write_stamp(prefix: Path, payload: dict) -> None:
    prefix.mkdir(parents=True, exist_ok=True)
    (prefix / STAMP_NAME).write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _read_stamp(prefix: Path) -> dict | None:
    path = prefix / STAMP_NAME
    if not path.is_file():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None


def _stamp_matches(prefix: Path, key: str, entry: dict, triplet: str) -> bool:
    actual = _read_stamp(prefix)
    if not actual:
        return False
    expected = _stamp_payload(key, entry, triplet)
    return all(actual.get(k) == v for k, v in expected.items())


def _ensure_checkout(key: str, entry: dict, cache_base: Path, *, offline: bool, quiet: bool) -> Path:
    revision = entry["revision"]
    dst = source_dir(cache_base, key, revision)
    git_dir = dst / ".git"
    if git_dir.is_dir():
        head = _capture(["git", "rev-parse", "HEAD"], cwd=dst)
        if head == revision:
            return dst
        if offline:
            raise BootstrapError(f"Cached {key} checkout is {head}, expected {revision}, and --offline was requested")
        shutil.rmtree(dst)
    elif dst.exists():
        shutil.rmtree(dst)

    if offline:
        raise BootstrapError(f"Pinned source for {key} is not cached: {dst}")

    dst.parent.mkdir(parents=True, exist_ok=True)
    _run(["git", "init", str(dst)], quiet=quiet)
    _run(["git", "remote", "add", "origin", entry["repository"]], cwd=dst, quiet=quiet)
    _run(["git", "fetch", "--depth", "1", "origin", revision], cwd=dst, quiet=quiet)
    _run(["git", "checkout", "--detach", "FETCH_HEAD"], cwd=dst, quiet=quiet)
    head = _capture(["git", "rev-parse", "HEAD"], cwd=dst)
    if head != revision:
        raise BootstrapError(f"Pinned checkout mismatch for {key}: got {head}, expected {revision}")
    return dst


def _download_archive(key: str, entry: dict, cache_base: Path, triplet: str, *, offline: bool, quiet: bool) -> Path:
    assets = entry.get("assets", {})
    asset = assets.get(triplet)
    if not asset:
        raise BootstrapError(f"{key} has no pinned archive for {triplet}")
    url = asset.get("url", "")
    expected = asset.get("sha256", "").lower()
    if not url or len(expected) != 64:
        raise BootstrapError(f"{key} manifest asset for {triplet} is incomplete")
    suffix = ".tar.gz" if url.endswith(".tar.gz") else Path(url.split("?", 1)[0]).suffix
    dst = cache_base / "downloads" / f"{key}-{entry['version']}-{triplet}{suffix}"
    dst.parent.mkdir(parents=True, exist_ok=True)

    def digest(path: Path) -> str:
        h = hashlib.sha256()
        with path.open("rb") as f:
            for chunk in iter(lambda: f.read(1024 * 1024), b""):
                h.update(chunk)
        return h.hexdigest()

    if dst.is_file() and digest(dst) == expected:
        return dst
    if dst.exists():
        dst.unlink()
    if offline:
        raise BootstrapError(f"Pinned archive for {key} is not cached: {dst}")
    if not quiet:
        print(f"Downloading {key}: {url}", flush=True)
    req = urllib.request.Request(url, headers={"User-Agent": "Xenon-Recomp dependency bootstrap"})
    try:
        with urllib.request.urlopen(req, timeout=120) as src, dst.open("wb") as out:
            shutil.copyfileobj(src, out, length=1024 * 1024)
    except Exception as exc:
        dst.unlink(missing_ok=True)
        raise BootstrapError(f"Unable to download {key} from {url}: {exc}") from exc
    actual = digest(dst)
    if actual != expected:
        dst.unlink(missing_ok=True)
        raise BootstrapError(f"SHA-256 mismatch for {key}: got {actual}, expected {expected}")
    return dst


def _safe_extract_archive(archive: Path, destination: Path) -> None:
    shutil.rmtree(destination, ignore_errors=True)
    destination.mkdir(parents=True, exist_ok=True)
    root = destination.resolve()

    def safe_target(name: str) -> Path:
        target = (destination / name).resolve()
        try:
            target.relative_to(root)
        except ValueError as exc:
            raise BootstrapError(f"Unsafe archive member: {name}") from exc
        return target

    if archive.name.endswith(".tar.gz") or archive.suffix in {".tgz", ".tar"}:
        with tarfile.open(archive, "r:*") as tf:
            members = tf.getmembers()
            for member in members:
                member_target = safe_target(member.name)
                if member.issym() or member.islnk():
                    link_name = member.linkname
                    if not link_name or Path(link_name).is_absolute():
                        raise BootstrapError(
                            f"Unsafe dependency archive link: {member.name} -> {link_name}"
                        )
                    # Symbolic links are relative to the link's parent; hard links
                    # are archive-root relative. In both cases, require the final
                    # target to stay inside the extraction root.
                    if member.issym():
                        link_target = (member_target.parent / link_name).resolve()
                    else:
                        link_target = (destination / link_name).resolve()
                    try:
                        link_target.relative_to(root)
                    except ValueError as exc:
                        raise BootstrapError(
                            f"Unsafe dependency archive link: {member.name} -> {link_name}"
                        ) from exc
            # Python 3.9 is still supported. We already validate every archive
            # path/link above; on newer Python versions explicitly select the
            # legacy fully-trusted extraction mode to avoid changing behavior
            # underneath that validation as tarfile defaults evolve.
            if sys.version_info >= (3, 12):
                tf.extractall(destination, filter="fully_trusted")
            else:
                tf.extractall(destination)
    else:
        with zipfile.ZipFile(archive) as zf:
            for info in zf.infolist():
                safe_target(info.filename)
            zf.extractall(destination)


def _copy_file(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def _find_preferred(root: Path, name: str, triplet: str) -> Path | None:
    candidates = [p for p in root.rglob(name) if p.is_file()]
    if not candidates:
        return None
    def score(p: Path) -> tuple[int, int, str]:
        text = p.as_posix().lower()
        arch_score = 0
        if triplet.endswith("-x64") and any(x in text for x in ("/x64/", "/x86_64/", "/linux/")):
            arch_score = 3
        if triplet.endswith("-arm64") and any(x in text for x in ("/arm64/", "/aarch64/")):
            arch_score = 3
        if "clang" in text:
            arch_score -= 1
        return (arch_score, -len(p.parts), text)
    return sorted(candidates, key=score, reverse=True)[0]


def _install_dxc_archive(extracted: Path, prefix: Path, triplet: str) -> None:
    include = _find_preferred(extracted, "dxcapi.h", triplet)
    if not include:
        raise BootstrapError("DXC archive does not contain dxcapi.h")
    # Preserve Microsoft's normal include spelling used by Xenon: <dxc/dxcapi.h>.
    include_parent = include.parent
    for header in include_parent.glob("*.h"):
        _copy_file(header, prefix / "include" / "dxc" / header.name)

    runtime = prefix / "runtime"
    libdir = prefix / "lib"
    runtime.mkdir(parents=True, exist_ok=True)
    libdir.mkdir(parents=True, exist_ok=True)
    if triplet.startswith("windows-"):
        compiler = _find_preferred(extracted, "dxcompiler.dll", triplet)
        import_lib = _find_preferred(extracted, "dxcompiler.lib", triplet)
        if not compiler or not import_lib:
            raise BootstrapError("DXC Windows archive is missing dxcompiler.dll or dxcompiler.lib")
        _copy_file(compiler, runtime / "dxcompiler.dll")
        _copy_file(import_lib, libdir / "dxcompiler.lib")
        dxil = _find_preferred(extracted, "dxil.dll", triplet)
        if dxil:
            _copy_file(dxil, runtime / "dxil.dll")
    elif triplet.startswith("linux-"):
        compiler = _find_preferred(extracted, "libdxcompiler.so", triplet)
        if not compiler:
            # Some release archives version the SO name.
            candidates = sorted(extracted.rglob("libdxcompiler.so*"))
            compiler = candidates[0] if candidates else None
        if not compiler:
            raise BootstrapError("DXC Linux archive is missing libdxcompiler.so")
        _copy_file(compiler, runtime / compiler.name)
        _copy_file(compiler, libdir / "libdxcompiler.so")
        dxil_candidates = sorted(extracted.rglob("libdxil.so*"))
        if dxil_candidates:
            _copy_file(dxil_candidates[0], runtime / dxil_candidates[0].name)
    else:
        raise BootstrapError(f"Managed DXC is not available for {triplet}")

    license_dir = prefix / "licenses" / "dxc"
    for license_name in ("LICENSE-LLVM.txt", "LICENSE-MIT.txt", "LICENSE.txt", "LICENSE.TXT"):
        license_file = _find_preferred(extracted, license_name, triplet)
        if license_file:
            _copy_file(license_file, license_dir / license_file.name)


def _build_vulkan_headers(source: Path, build: Path, prefix: Path, jobs: int, quiet: bool) -> None:
    shutil.rmtree(build, ignore_errors=True)
    cmd = [
        _cmake_executable(), "-S", str(source), "-B", str(build), *_cmake_generator_args(),
        "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={prefix}",
        "-DVULKAN_HEADERS_ENABLE_TESTS=OFF",
    ]
    _run(cmd, quiet=quiet)
    _run([_cmake_executable(), "--build", str(build), "--config", "Release", "--target", "install", "--parallel", str(jobs)], quiet=quiet)


def _build_vulkan_loader(source: Path, build: Path, prefix: Path, headers_prefix: Path,
                         jobs: int, quiet: bool) -> None:
    shutil.rmtree(build, ignore_errors=True)
    cmd = [
        _cmake_executable(), "-S", str(source), "-B", str(build), *_cmake_generator_args(),
        "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={prefix}",
        f"-DVULKAN_HEADERS_INSTALL_DIR={headers_prefix}",
        "-DBUILD_TESTS=OFF", "-DLOADER_CODEGEN=OFF",
    ]
    # Keep the Linux loader feature-complete. These may require the normal X11/
    # Wayland development packages on the RELEASE BUILD MACHINE only; they are
    # not end-user installer prerequisites.
    _run(cmd, quiet=quiet)
    _run([_cmake_executable(), "--build", str(build), "--config", "Release", "--target", "install", "--parallel", str(jobs)], quiet=quiet)


def _stage_vulkan_runtime(prefix: Path, triplet: str) -> list[str]:
    runtime = prefix / "runtime"
    shutil.rmtree(runtime, ignore_errors=True)
    runtime.mkdir(parents=True, exist_ok=True)
    copied: list[str] = []
    if triplet.startswith("windows-"):
        patterns = ["vulkan-1.dll"]
    elif triplet.startswith("macos-"):
        patterns = ["libvulkan.dylib", "libvulkan.*.dylib"]
    else:
        patterns = ["libvulkan.so", "libvulkan.so.*"]
    for base in (prefix / "bin", prefix / "lib", prefix / "lib64"):
        for pattern in patterns:
            for src in sorted(base.glob(pattern)) if base.is_dir() else []:
                dst = runtime / src.name
                shutil.copy2(src.resolve(), dst)
                copied.append(dst.name)
    return sorted(set(copied))


def _cmake_executable() -> str:
    # When the bootstrap is launched by Xenon's parent CMake configure, use the
    # exact CMake executable that configured the parent project. This avoids a
    # second CMake installation on PATH selecting different generators.
    return os.environ.get("XENON_CMAKE_COMMAND") or "cmake"


def _visual_studio_generators(cmake_help: str) -> list[tuple[int, int, str]]:
    generators: list[tuple[int, int, str]] = []
    pattern = re.compile(r"Visual Studio (\d+) (\d{4})")
    for match in pattern.finditer(cmake_help):
        name = match.group(0)
        candidate = (int(match.group(1)), int(match.group(2)), name)
        if candidate not in generators:
            generators.append(candidate)
    return sorted(generators, reverse=True)


def _native_vs_platform() -> str:
    arch = normalize_arch(platform.machine())
    if arch == "arm64":
        return "ARM64"
    if arch == "x86":
        return "Win32"
    return "x64"


def _cmake_generator_args() -> list[str]:
    # Auto-bootstrap is normally launched from an already configured Xenon
    # build. Preserve that generator so a Visual Studio parent does not spawn a
    # Ninja child that suddenly requires cl.exe/INCLUDE/LIB to be on the shell
    # PATH. This is especially important from normal PowerShell, where CMake's
    # VS generator can locate MSVC but a standalone Ninja configure cannot.
    inherited = os.environ.get("XENON_CMAKE_GENERATOR", "").strip()
    if inherited:
        args = ["-G", inherited]
        generator_platform = os.environ.get("XENON_CMAKE_GENERATOR_PLATFORM", "").strip()
        generator_toolset = os.environ.get("XENON_CMAKE_GENERATOR_TOOLSET", "").strip()
        generator_instance = os.environ.get("XENON_CMAKE_GENERATOR_INSTANCE", "").strip()
        make_program = os.environ.get("XENON_CMAKE_MAKE_PROGRAM", "").strip()
        if generator_platform:
            args.extend(["-A", generator_platform])
        if generator_toolset:
            args.extend(["-T", generator_toolset])
        if generator_instance:
            args.append(f"-DCMAKE_GENERATOR_INSTANCE={generator_instance}")
        if make_program and "Ninja" in inherited:
            args.append(f"-DCMAKE_MAKE_PROGRAM={make_program}")
        return args

    # Standalone bootstrap on Windows should prefer a Visual Studio generator.
    # Merely having ninja.exe on PATH is not enough: Ninja still needs a fully
    # initialized MSVC developer environment, while the VS generator can locate
    # the installed Build Tools itself.
    if sys.platform.startswith("win"):
        try:
            help_text = _capture([_cmake_executable(), "--help"])
            generators = _visual_studio_generators(help_text)
        except BootstrapError:
            generators = []
        if generators:
            return ["-G", generators[0][2], "-A", _native_vs_platform()]

    if shutil.which("ninja"):
        return ["-G", "Ninja"]
    return []


def _build_sdl2(source: Path, build: Path, prefix: Path, jobs: int, quiet: bool) -> None:
    shutil.rmtree(build, ignore_errors=True)
    build.mkdir(parents=True, exist_ok=True)
    cmd = [
        _cmake_executable(), "-S", str(source), "-B", str(build),
        *_cmake_generator_args(),
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_INSTALL_PREFIX={prefix}",
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
        "-DSDL_SHARED=OFF",
        "-DSDL_STATIC=ON",
        "-DSDL_TEST=OFF",
        "-DSDL_TESTS=OFF",
        "-DSDL_TEST_LIBRARY=OFF",
        "-DSDL_INSTALL=ON",
    ]
    _run(cmd, quiet=quiet)
    _run([_cmake_executable(), "--build", str(build), "--config", "Release", "--target", "install", "--parallel", str(jobs)], quiet=quiet)


def _find_msys_bash() -> str | None:
    explicit = os.environ.get("XENON_MSYS2_BASH")
    candidates = [
        explicit,
        r"C:\msys64\usr\bin\bash.exe",
        r"C:\msys64\mingw64\bin\bash.exe",
    ]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return candidate
    return None


def _ffmpeg_configure_args(prefix: Path, *, windows: bool, have_nasm: bool) -> list[str]:
    args = [
        f"--prefix={prefix.as_posix()}",
        "--enable-shared",
        "--disable-static",
        "--disable-programs",
        "--disable-doc",
        "--disable-network",
        "--disable-avdevice",
        "--disable-avfilter",
        "--disable-avformat",
        "--disable-postproc",
        "--disable-swresample",
        "--disable-swscale",
        "--disable-debug",
        "--disable-everything",
        "--enable-decoder=xmaframes",
    ]
    if not windows:
        args.append("--enable-pic")
    if not have_nasm:
        args.append("--disable-x86asm")
    if windows:
        args.extend(["--toolchain=msvc", "--target-os=win64", "--arch=x86_64"])
    return args


def _build_ffmpeg_posix(source: Path, build: Path, prefix: Path, jobs: int, quiet: bool) -> None:
    shutil.rmtree(build, ignore_errors=True)
    build.mkdir(parents=True, exist_ok=True)
    configure = source / "configure"
    if not configure.is_file():
        raise BootstrapError(f"FFmpeg configure script missing: {configure}")
    have_nasm = bool(shutil.which("nasm") or shutil.which("yasm"))
    _run([str(configure), *_ffmpeg_configure_args(prefix, windows=False, have_nasm=have_nasm)], cwd=build, quiet=quiet)
    make = _which_any(["gmake", "make"])
    if not make:
        raise BootstrapError("Building xenia-project FFmpeg requires make or gmake")
    _run([make, f"-j{jobs}"], cwd=build, quiet=quiet)
    _run([make, "install"], cwd=build, quiet=quiet)


def _build_ffmpeg_windows(source: Path, build: Path, prefix: Path, jobs: int, quiet: bool) -> None:
    # Xenia's FFmpeg fork uses the normal FFmpeg configure/make build. Native
    # MSVC is supported, but configure/make need an MSYS2 shell. The compiler
    # environment (cl/link) must be visible to this process; official CI should
    # run from a VS Developer Command Prompt or setup-msbuild equivalent.
    if not shutil.which("cl.exe") and not shutil.which("cl"):
        raise BootstrapError(
            "Windows FFmpeg bootstrap requires an MSVC developer environment (cl.exe not found). "
            "Run from a Visual Studio Developer Command Prompt or initialize VsDevCmd first."
        )
    bash = _find_msys_bash()
    if not bash:
        raise BootstrapError(
            "Windows FFmpeg bootstrap requires MSYS2. Set XENON_MSYS2_BASH to msys2 bash.exe "
            "or install MSYS2 at C:\\msys64."
        )
    shutil.rmtree(build, ignore_errors=True)
    build.mkdir(parents=True, exist_ok=True)

    # Convert Windows paths to MSYS-style paths without requiring cygpath.
    def msys_path(p: Path) -> str:
        s = str(p.resolve()).replace("\\", "/")
        if len(s) >= 3 and s[1] == ":":
            s = f"/{s[0].lower()}{s[2:]}"
        return s

    src_msys = msys_path(source)
    build_msys = msys_path(build)
    prefix_msys = msys_path(prefix)
    have_nasm = bool(shutil.which("nasm.exe") or shutil.which("nasm"))
    args = _ffmpeg_configure_args(Path(prefix_msys), windows=True, have_nasm=have_nasm)
    quoted = " ".join(_shell_quote(x) for x in args)
    script = (
        f"set -euo pipefail\n"
        f"cd {_shell_quote(build_msys)}\n"
        f"{_shell_quote(src_msys + '/configure')} {quoted}\n"
        f"make -j{jobs}\n"
        f"make install\n"
    )
    script_path = build / "xenon-build-ffmpeg.sh"
    script_path.write_text(script, encoding="utf-8", newline="\n")
    _run([bash, msys_path(script_path)], quiet=quiet)


def _shell_quote(value: str) -> str:
    return "'" + value.replace("'", "'\"'\"'") + "'"


def _candidate_lib(prefix: Path, names: Sequence[str]) -> Path | None:
    for directory in (prefix / "lib", prefix / "lib64", prefix / "bin"):
        for name in names:
            path = directory / name
            if path.is_file():
                return path
    return None


def _stage_ffmpeg_runtime(prefix: Path) -> list[str]:
    runtime = prefix / "runtime"
    shutil.rmtree(runtime, ignore_errors=True)
    runtime.mkdir(parents=True, exist_ok=True)
    candidates: list[Path] = []
    if sys.platform.startswith("win"):
        candidates.extend((prefix / "bin").glob("avcodec-*.dll"))
        candidates.extend((prefix / "bin").glob("avutil-*.dll"))
    elif sys.platform == "darwin":
        candidates.extend((prefix / "lib").glob("libavcodec*.dylib"))
        candidates.extend((prefix / "lib").glob("libavutil*.dylib"))
    else:
        candidates.extend((prefix / "lib").glob("libavcodec.so*"))
        candidates.extend((prefix / "lib").glob("libavutil.so*"))

    copied: list[str] = []
    seen: set[str] = set()
    for path in sorted(candidates, key=lambda p: p.name):
        if path.name in seen or not path.exists():
            continue
        seen.add(path.name)
        # Dereference symlinks deliberately so every SONAME/loader-visible name
        # in runtime/ is a self-contained file when archived for release.
        shutil.copy2(path.resolve(), runtime / path.name)
        copied.append(path.name)
    return copied


def _copy_licenses(key: str, source: Path, prefix: Path) -> None:
    license_dir = prefix / "licenses" / key
    license_dir.mkdir(parents=True, exist_ok=True)
    names = ["LICENSE.txt", "LICENSE", "LICENSE.md", "COPYING.LGPLv2.1", "COPYING.LGPLv3"]
    for name in names:
        src = source / name
        if src.is_file():
            shutil.copy2(src, license_dir / name)


def verify_dependency(key: str, entry: dict, root: Path, triplet: str) -> tuple[bool, str]:
    prefix = dependency_prefix(root, entry)
    if not _stamp_matches(prefix, key, entry, triplet):
        return False, "missing or stale dependency stamp"

    if key == "sdl2":
        header_candidates = [prefix / "include" / "SDL2" / "SDL.h", prefix / "include" / "SDL.h"]
        if not any(p.is_file() for p in header_candidates):
            return False, "SDL.h not found"
        if not _candidate_lib(prefix, ["libSDL2.a", "SDL2-static.lib", "SDL2.lib", "libSDL2.lib"]):
            return False, "SDL2 static/import library not found"
        return True, "ok"

    if key == "xenia-ffmpeg":
        codec_id = prefix / "include" / "libavcodec" / "codec_id.h"
        avcodec_h = prefix / "include" / "libavcodec" / "avcodec.h"
        if not codec_id.is_file() or not avcodec_h.is_file():
            return False, "FFmpeg development headers not found"
        if "AV_CODEC_ID_XMAFRAMES" not in codec_id.read_text(encoding="utf-8", errors="ignore"):
            return False, "FFmpeg headers do not expose AV_CODEC_ID_XMAFRAMES"
        if not _candidate_lib(prefix, ["libavcodec.so", "libavcodec.dylib", "avcodec.lib", "libavcodec.lib", "libavcodec.a"]):
            return False, "libavcodec link library not found"
        if not _candidate_lib(prefix, ["libavutil.so", "libavutil.dylib", "avutil.lib", "libavutil.lib", "libavutil.a"]):
            return False, "libavutil link library not found"
        runtime = prefix / "runtime"
        if not runtime.is_dir():
            return False, "FFmpeg runtime directory has not been staged"
        runtime_names = [p.name.lower() for p in runtime.iterdir() if p.is_file()]
        if not any("avcodec" in name for name in runtime_names):
            return False, "FFmpeg avcodec runtime library has not been staged"
        if not any("avutil" in name for name in runtime_names):
            return False, "FFmpeg avutil runtime library has not been staged"
        return True, "ok"

    if key == "vulkan-headers":
        if not (prefix / "include" / "vulkan" / "vulkan.h").is_file():
            return False, "Vulkan headers not installed"
        return True, "ok"

    if key == "vulkan-loader":
        runtime = prefix / "runtime"
        if not runtime.is_dir():
            return False, "Vulkan loader runtime has not been staged"
        if triplet.startswith("windows-"):
            if not (runtime / "vulkan-1.dll").is_file():
                return False, "vulkan-1.dll not staged"
            if not _candidate_lib(prefix, ["vulkan-1.lib", "vulkan.lib"]):
                return False, "Vulkan loader import library not found"
        elif triplet.startswith("macos-"):
            if not any(runtime.glob("libvulkan*.dylib")):
                return False, "libvulkan dylib runtime not staged"
            if not _candidate_lib(prefix, ["libvulkan.dylib", "libvulkan.1.dylib", "libvulkan.a"]):
                return False, "Vulkan loader link library not found"
        else:
            if not any(runtime.glob("libvulkan.so*")):
                return False, "libvulkan.so runtime not staged"
            if not _candidate_lib(prefix, ["libvulkan.so", "libvulkan.so.1", "libvulkan.a"]):
                return False, "Vulkan loader link library not found"
        return True, "ok"

    if key == "dxc":
        if not (prefix / "include" / "dxc" / "dxcapi.h").is_file():
            return False, "dxcapi.h not installed"
        runtime = prefix / "runtime"
        if triplet.startswith("windows-"):
            if not (prefix / "lib" / "dxcompiler.lib").is_file():
                return False, "dxcompiler.lib not installed"
            if not (runtime / "dxcompiler.dll").is_file():
                return False, "dxcompiler.dll not staged"
        elif triplet.startswith("linux-"):
            if not (prefix / "lib" / "libdxcompiler.so").is_file():
                return False, "libdxcompiler.so link library not installed"
            if not any(runtime.glob("libdxcompiler.so*")):
                return False, "libdxcompiler.so runtime not staged"
        else:
            return False, f"DXC verifier has no layout for {triplet}"
        return True, "ok"

    return False, f"no verifier implemented for {key}"


def ensure_dependency(key: str, entry: dict, *, root: Path, cache_base: Path, triplet: str,
                      jobs: int, force: bool, offline: bool, quiet: bool) -> None:
    prefix = dependency_prefix(root, entry)
    ok, _ = verify_dependency(key, entry, root, triplet)
    if ok and not force:
        if not quiet:
            print(f"{key}: already provisioned at {prefix}")
        return

    native_triplet = host_triplet()
    if triplet != native_triplet:
        raise BootstrapError(
            f"Native bootstrap can only build for {native_triplet} on this host; requested {triplet}. "
            "Cross-build dependencies explicitly or point CMake at a prepared dependency root."
        )
    if key == "xenia-ffmpeg" and triplet.startswith("windows-") and not triplet.endswith("-x64"):
        raise BootstrapError("Managed Windows xenia-ffmpeg bootstrap currently supports x64 only")

    # Provision prerequisites first so a release build is deterministic even
    # when a user selects only a top-level dependency such as vulkan-loader.
    for dependency in entry.get("depends", []):
        manifest = _load_manifest()
        ensure_dependency(dependency, manifest["dependencies"][dependency], root=root,
                          cache_base=cache_base, triplet=triplet, jobs=jobs,
                          force=force, offline=offline, quiet=quiet)

    kind = entry.get("kind", "git")
    build = build_dir(cache_base, triplet, key, entry["revision"])
    shutil.rmtree(prefix, ignore_errors=True)
    prefix.parent.mkdir(parents=True, exist_ok=True)
    source: Path | None = None

    if kind == "archive":
        archive = _download_archive(key, entry, cache_base, triplet, offline=offline, quiet=quiet)
        extracted = cache_base / "extract" / triplet / f"{key}-{entry['revision'].replace('/', '_')}"
        _safe_extract_archive(archive, extracted)
        if key == "dxc":
            _install_dxc_archive(extracted, prefix, triplet)
        else:
            raise BootstrapError(f"No archive installer implemented for dependency {key}")
    else:
        source = _ensure_checkout(key, entry, cache_base, offline=offline, quiet=quiet)
        if key == "sdl2":
            _build_sdl2(source, build, prefix, jobs, quiet)
        elif key == "xenia-ffmpeg":
            if sys.platform.startswith("win"):
                _build_ffmpeg_windows(source, build, prefix, jobs, quiet)
            else:
                _build_ffmpeg_posix(source, build, prefix, jobs, quiet)
        elif key == "vulkan-headers":
            _build_vulkan_headers(source, build, prefix, jobs, quiet)
        elif key == "vulkan-loader":
            headers_entry = _load_manifest()["dependencies"]["vulkan-headers"]
            headers_prefix = dependency_prefix(root, headers_entry)
            _build_vulkan_loader(source, build, prefix, headers_prefix, jobs, quiet)
        else:
            raise BootstrapError(f"No builder implemented for dependency {key}")

    if source is not None:
        _copy_licenses(key, source, prefix)
    payload = _stamp_payload(key, entry, triplet)
    if key == "xenia-ffmpeg":
        runtime_files = _stage_ffmpeg_runtime(prefix)
        if not runtime_files:
            raise BootstrapError("xenia-ffmpeg built, but no avcodec/avutil runtime libraries were produced")
        payload["runtime_files"] = runtime_files
    elif key == "vulkan-loader":
        runtime_files = _stage_vulkan_runtime(prefix, triplet)
        if not runtime_files:
            raise BootstrapError("Vulkan loader built, but no runtime library was produced")
        payload["runtime_files"] = runtime_files
    elif key == "dxc":
        payload["runtime_files"] = sorted(p.name for p in (prefix / "runtime").iterdir() if p.is_file())
    _write_stamp(prefix, payload)
    ok, reason = verify_dependency(key, entry, root, triplet)
    if not ok:
        raise BootstrapError(f"{key} build completed but verification failed: {reason}")
    if not quiet:
        print(f"{key}: provisioned at {prefix}")


def _selected_keys(manifest: dict, only: list[str] | None) -> list[str]:
    keys = list(manifest["dependencies"].keys())
    if not only:
        return keys
    unknown = [k for k in only if k not in manifest["dependencies"]]
    if unknown:
        raise BootstrapError(f"Unknown dependency name(s): {', '.join(unknown)}")
    return only


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Provision Xenon's pinned native dependencies")
    parser.add_argument("command", choices=("ensure", "verify", "status", "clean"))
    parser.add_argument("--root", type=Path, default=None, help="Managed triplet root (default: .xenon/deps/<triplet>)")
    parser.add_argument("--triplet", default=host_triplet())
    parser.add_argument("--only", action="append", dest="only", help="Operate on one dependency (repeatable)")
    parser.add_argument("--jobs", type=int, default=max(1, os.cpu_count() or 1))
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("--json", action="store_true", dest="as_json")
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    manifest = _load_manifest()
    keys = _selected_keys(manifest, args.only)
    managed_base = DEFAULT_MANAGED_BASE
    root = (args.root or (managed_base / args.triplet)).resolve()
    cache_base = managed_base / ".cache"

    if args.command == "clean":
        for key in keys:
            prefix = dependency_prefix(root, manifest["dependencies"][key])
            shutil.rmtree(prefix, ignore_errors=True)
            build_parent = cache_base / "build" / args.triplet
            if build_parent.is_dir():
                for path in build_parent.glob(f"{key}-*"):
                    shutil.rmtree(path, ignore_errors=True)
        return 0

    if args.command == "ensure":
        for key in keys:
            ensure_dependency(
                key,
                manifest["dependencies"][key],
                root=root,
                cache_base=cache_base,
                triplet=args.triplet,
                jobs=args.jobs,
                force=args.force,
                offline=args.offline,
                quiet=args.quiet,
            )

    rows = []
    all_ok = True
    for key in keys:
        entry = manifest["dependencies"][key]
        prefix = dependency_prefix(root, entry)
        ok, reason = verify_dependency(key, entry, root, args.triplet)
        all_ok &= ok
        rows.append({
            "dependency": key,
            "name": entry["name"],
            "version": entry["version"],
            "revision": entry["revision"],
            "linkage": entry["linkage"],
            "prefix": str(prefix),
            "ready": ok,
            "detail": reason,
        })

    if args.as_json:
        print(json.dumps({"triplet": args.triplet, "root": str(root), "dependencies": rows}, indent=2))
    elif not args.quiet or args.command != "ensure":
        print(f"Xenon dependency root: {root}")
        print(f"Host triplet: {args.triplet}")
        for row in rows:
            marker = "READY" if row["ready"] else "MISSING"
            print(f"[{marker:7}] {row['dependency']:<14} {row['version']:<18} {row['detail']}")

    return 0 if all_ok else 2


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BootstrapError as exc:
        print(f"dependency bootstrap error: {exc}", file=sys.stderr)
        raise SystemExit(1)

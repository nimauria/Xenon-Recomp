#!/usr/bin/env python3
"""Audit C++ source ownership against CMake's configured target graph.

Run ``prepare`` before CMake configure, then ``check`` after configure. The
File API works with Ninja, Makefiles, and Visual Studio, including targets
whose source lists contain generator expressions.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path


SOURCE_ROOT = Path(__file__).resolve().parents[2]
MAIN_PATTERN = re.compile(r"\b(?:int|auto)\s+(?:main|wmain)\s*\(")


def cache_values(build: Path) -> dict[str, str]:
    values = {}
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.split(":", 1)[0]] = value
    return values


def load_targets(build: Path, source: Path) -> tuple[dict[str, dict], dict[Path, set[str]]]:
    reply_dir = build / ".cmake" / "api" / "v1" / "reply"
    indexes = sorted(reply_dir.glob("index-*.json"), key=lambda p: p.stat().st_mtime_ns)
    if not indexes:
        raise RuntimeError("No CMake File API reply. Run prepare before configuring CMake.")
    index = json.loads(indexes[-1].read_text(encoding="utf-8"))
    codemodel_reply = index.get("reply", {}).get("codemodel-v2")
    if not codemodel_reply:
        raise RuntimeError("No codemodel-v2 reply. Run prepare and reconfigure CMake.")
    codemodel = json.loads((reply_dir / codemodel_reply["jsonFile"]).read_text(encoding="utf-8"))
    targets: dict[str, dict] = {}
    owners: dict[Path, set[str]] = defaultdict(set)
    for configuration in codemodel["configurations"]:
        for target_ref in configuration["targets"]:
            target = json.loads((reply_dir / target_ref["jsonFile"]).read_text(encoding="utf-8"))
            name = target["name"]
            targets[name] = target
            for item in target.get("sources", []):
                if "compileGroupIndex" not in item or not item["path"].endswith(".cpp"):
                    continue
                path = Path(item["path"])
                owners[(path if path.is_absolute() else source / path).resolve()].add(name)
    return targets, owners


def ctest_names(build: Path) -> set[str]:
    result = subprocess.run(
        ["ctest", "--test-dir", str(build), "--show-only=json-v1"],
        capture_output=True, check=True, text=True,
    )
    return {test["name"] for test in json.loads(result.stdout)["tests"]}


def inactive_reason(path: Path, targets: dict[str, dict], cache: dict[str, str]) -> str | None:
    text = path.as_posix()
    windows = sys.platform == "win32"
    if text.startswith("launcher/") and "xenon_launcher" not in targets:
        return "launcher disabled or Qt unavailable"
    if text.startswith("runtime_host/") and "xenon_runtime_host" not in targets:
        return "runtime host disabled"
    if text.startswith(("src/audio/", "tests/audio/")) and "xenon_audio" not in targets:
        return "audio disabled"
    if text in ("tests/core/guest_export_abi_tests.cpp", "tests/core/audio_guest_callback_tests.cpp") and "xenon_audio" not in targets:
        return "audio integration test requires audio"
    if text.startswith("src/graphics/d3d12/") and (not windows or "xenon_graphics_d3d12" not in targets):
        return "D3D12 unavailable on this configuration"
    if text.startswith("src/graphics/vulkan/") and "xenon_graphics_vulkan" not in targets:
        return "Vulkan disabled or SDK unavailable"
    if text.startswith("src/graphics/dxc/") and "xenon_graphics_dxc" not in targets:
        return "DXC disabled or SDK unavailable"
    if text == "tests/graphics/xenos/shader_translation.cpp" and "xenon_graphics_dxc" not in targets:
        return "shader translation test requires DXC"
    if text == "src/memory/mapping/host_vm_fallback.cpp" and sys.platform.startswith(("linux", "darwin", "win")):
        return "fallback host VM implementation for other platforms"
    if text == "src/memory/mapping/host_vm_windows.cpp" and not windows:
        return "Windows host VM implementation"
    if text == "src/memory/mapping/host_vm_posix.cpp" and windows:
        return "POSIX host VM implementation"
    if text == "src/input/xinput/xinput_host_win32.cpp" and (not windows or cache.get("XENON_INPUT_ENABLE_XINPUT") == "OFF"):
        return "Windows XInput implementation"
    if text == "src/input/sdl/sdl2_host.cpp" and cache.get("XENON_INPUT_ENABLE_SDL2") == "OFF":
        return "SDL2 input disabled"
    if text == "src/input/sdl/sdl3_host.cpp" and (
        cache.get("XENON_INPUT_ENABLE_SDL3") == "OFF"
        or cache.get("SDL3_DIR", "").endswith("-NOTFOUND")
    ):
        return "SDL3 input disabled or dependency unavailable"
    if text.startswith("tests/input/") and "xenon_input" not in targets:
        return "input disabled"
    if text.startswith("tests/graphics/") and "xenon_graphics" not in targets:
        return "graphics disabled"
    if text.startswith("tests/kernel/") and "xenon_kernel" not in targets:
        return "kernel disabled"
    if text.startswith("tests/memory/") and "xenon_memory" not in targets:
        return "memory disabled"
    if text.startswith("tests/network/") and "xenon_network" not in targets:
        return "network disabled"
    if text.startswith("tests/runtime_host/") and "xenon_runtime_host" not in targets:
        return "runtime host disabled"
    if text.startswith("tests/cpu/dynamic_fallback") and "xenon_memory" not in targets:
        return "dynamic fallback test requires memory"
    if text == "tests/xam/xam_net_socket_tests.cpp" and not windows:
        return "NetDll host socket implementation is Windows-only"
    return None


def non_ctest_executable(target: str) -> bool:
    return (
        target.startswith("xenon_cpu_codegen_")
        or target in {
            "xenon_memory_codegen_integration",
            "xenon_memory_v2_benchmarks",
            "xenon_recomp_analysis_benchmark",
            "xenon_runtime_host_fixture",
        }
    )


def check(build: Path, source: Path) -> int:
    cache = cache_values(build)
    configured_source = Path(cache.get("CMAKE_HOME_DIRECTORY", "")).resolve()
    if configured_source != source:
        raise RuntimeError(f"Build directory belongs to {configured_source}, expected {source}")
    targets, owners = load_targets(build, source)
    registered = ctest_names(build)
    production = [*sorted((source / "src").rglob("*.cpp")),
                  *sorted((source / "launcher" / "src").rglob("*.cpp")),
                  *sorted((source / "runtime_host" / "src").rglob("*.cpp"))]
    test_sources = [*sorted((source / "tests").rglob("*.cpp")),
                    *sorted((source / "launcher" / "tests").rglob("*.cpp"))]
    failures = []
    skipped = []
    production_count = 0
    test_count = 0
    for file in production:
        relative = file.relative_to(source)
        target_names = owners.get(file.resolve(), set())
        production_owners = target_names - registered
        if production_owners:
            production_count += 1
        elif reason := inactive_reason(relative, targets, cache):
            skipped.append((relative, reason))
        else:
            failures.append(f"production source has no production target: {relative}")
    for file in test_sources:
        if not MAIN_PATTERN.search(file.read_text(encoding="utf-8-sig", errors="replace")):
            continue
        test_count += 1
        relative = file.relative_to(source)
        target_names = owners.get(file.resolve(), set())
        if target_names & registered or any(non_ctest_executable(name) for name in target_names):
            continue
        if reason := inactive_reason(relative, targets, cache):
            skipped.append((relative, reason))
        elif target_names:
            failures.append(f"standalone test has no CTest registration: {relative} ({', '.join(sorted(target_names))})")
        else:
            failures.append(f"standalone test is not built: {relative}")
    print(f"Production: {production_count}/{len(production)} compiled in configured targets")
    print(f"Standalone test programs: {test_count}; CTest entries: {len(registered)}")
    for reason, count in sorted(Counter(reason for _, reason in skipped).items()):
        print(f"SKIP {count}: {reason}")
    for failure in failures:
        print(f"ERROR {failure}", file=sys.stderr)
    print(f"Excluded by platform/dependency: {len(skipped)}; errors: {len(failures)}")
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("prepare", "check"))
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--source-dir", type=Path, default=SOURCE_ROOT)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    source = args.source_dir.resolve()
    if args.action == "prepare":
        query = build / ".cmake" / "api" / "v1" / "query" / "codemodel-v2"
        query.parent.mkdir(parents=True, exist_ok=True)
        query.touch()
        print(f"Prepared CMake File API query: {query}")
        return 0
    return check(build, source)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, KeyError, ValueError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"build accountability error: {exc}", file=sys.stderr)
        raise SystemExit(2)

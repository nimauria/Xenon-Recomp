import importlib.util
import io
import json
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "tools" / "deps" / "bootstrap.py"
spec = importlib.util.spec_from_file_location("xenon_deps_bootstrap", MODULE_PATH)
bootstrap = importlib.util.module_from_spec(spec)
assert spec.loader
spec.loader.exec_module(bootstrap)


class DependencyBootstrapTests(unittest.TestCase):
    def setUp(self):
        self.manifest = bootstrap._load_manifest()
        self.triplet = "linux-x64"

    def _fake_ready_dependency(self, root: Path, key: str) -> Path:
        entry = self.manifest["dependencies"][key]
        prefix = bootstrap.dependency_prefix(root, entry)
        (prefix / "lib").mkdir(parents=True)

        if key == "sdl2":
            (prefix / "include" / "SDL2").mkdir(parents=True)
            (prefix / "include" / "SDL2" / "SDL.h").write_text("/* SDL */\n")
            (prefix / "lib" / "libSDL2.a").write_bytes(b"")
        elif key == "xenia-ffmpeg":
            (prefix / "include" / "libavcodec").mkdir(parents=True)
            (prefix / "include" / "libavcodec" / "avcodec.h").write_text("/* avcodec */\n")
            (prefix / "include" / "libavcodec" / "codec_id.h").write_text(
                "enum { AV_CODEC_ID_XMAFRAMES = 1 };\n"
            )
            (prefix / "lib" / "libavcodec.so").write_bytes(b"")
            (prefix / "lib" / "libavutil.so").write_bytes(b"")
            (prefix / "runtime").mkdir(parents=True)
            (prefix / "runtime" / "libavcodec.so").write_bytes(b"")
            (prefix / "runtime" / "libavutil.so").write_bytes(b"")
        elif key == "vulkan-headers":
            (prefix / "include" / "vulkan").mkdir(parents=True)
            (prefix / "include" / "vulkan" / "vulkan.h").write_text("/* Vulkan */\n")
        elif key == "vulkan-loader":
            (prefix / "lib" / "libvulkan.so").write_bytes(b"")
            (prefix / "runtime").mkdir(parents=True)
            (prefix / "runtime" / "libvulkan.so.1").write_bytes(b"")
        elif key == "dxc":
            (prefix / "include" / "dxc").mkdir(parents=True)
            (prefix / "include" / "dxc" / "dxcapi.h").write_text("/* DXC */\n")
            (prefix / "lib" / "libdxcompiler.so").write_bytes(b"")
            (prefix / "runtime").mkdir(parents=True)
            (prefix / "runtime" / "libdxcompiler.so").write_bytes(b"")
        else:
            self.fail(f"unknown fake dependency: {key}")

        bootstrap._write_stamp(prefix, bootstrap._stamp_payload(key, entry, self.triplet))
        return prefix

    def test_manifest_matches_release_pins(self):
        self.assertEqual(self.manifest["schema"], 3)
        deps = self.manifest["dependencies"]
        self.assertEqual(deps["sdl2"]["revision"], "5d249570393f7a37e037abf22cd6012a4cc56a71")
        self.assertEqual(deps["xenia-ffmpeg"]["revision"], "15ece0882e8d5875051ff5b73c5a8326f7cee9f5")
        self.assertEqual(deps["vulkan-headers"]["version"], "1.4.357.0")
        self.assertEqual(deps["vulkan-loader"]["version"], "1.4.357.0")
        self.assertEqual(deps["dxc"]["revision"], "v1.9.2607")
        self.assertEqual(deps["xenia-ffmpeg"]["linkage"], "shared")
        self.assertIn("AV_CODEC_ID_XMAFRAMES", deps["xenia-ffmpeg"]["required_capabilities"])
        self.assertEqual(
            deps["dxc"]["assets"]["windows-x64"]["sha256"],
            "a1dfb116ba3eeae6a1582291b53a8e7bf65ad760676bd3194685c8f7367cd241",
        )
        self.assertEqual(
            deps["dxc"]["assets"]["linux-x64"]["sha256"],
            "55665c87824051ed4774ff3280a79ccbbb7d39243b9736ca5e98222134112d54",
        )

    def test_arch_normalization(self):
        self.assertEqual(bootstrap.normalize_arch("x86_64"), "x64")
        self.assertEqual(bootstrap.normalize_arch("AMD64"), "x64")
        self.assertEqual(bootstrap.normalize_arch("aarch64"), "arm64")

    def test_verify_rejects_mainline_style_ffmpeg_without_xmaframes(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            prefix = self._fake_ready_dependency(root, "xenia-ffmpeg")
            (prefix / "include" / "libavcodec" / "codec_id.h").write_text("AV_CODEC_ID_XMA2\n")
            entry = self.manifest["dependencies"]["xenia-ffmpeg"]
            ok, detail = bootstrap.verify_dependency("xenia-ffmpeg", entry, root, self.triplet)
            self.assertFalse(ok)
            self.assertIn("XMAFRAMES", detail)

    def test_verify_requires_both_ffmpeg_runtime_libraries(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            prefix = self._fake_ready_dependency(root, "xenia-ffmpeg")
            (prefix / "runtime" / "libavutil.so").unlink()
            entry = self.manifest["dependencies"]["xenia-ffmpeg"]
            ok, detail = bootstrap.verify_dependency("xenia-ffmpeg", entry, root, self.triplet)
            self.assertFalse(ok)
            self.assertIn("avutil", detail)

    def test_stale_manifest_stamp_is_rejected(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            prefix = self._fake_ready_dependency(root, "sdl2")
            stamp_path = prefix / bootstrap.STAMP_NAME
            stamp = json.loads(stamp_path.read_text())
            stamp["manifest_sha256"] = "0" * 64
            stamp_path.write_text(json.dumps(stamp))
            entry = self.manifest["dependencies"]["sdl2"]
            ok, detail = bootstrap.verify_dependency("sdl2", entry, root, self.triplet)
            self.assertFalse(ok)
            self.assertIn("stale", detail)

    def test_verify_accepts_every_pinned_linux_layout(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            for key in self.manifest["dependencies"]:
                self._fake_ready_dependency(root, key)
                entry = self.manifest["dependencies"][key]
                ok, detail = bootstrap.verify_dependency(key, entry, root, self.triplet)
                self.assertTrue(ok, f"{key}: {detail}")


    @unittest.skipUnless(sys.platform.startswith("linux") and shutil.which("cmake"),
                         "managed CMake resolver integration test requires Linux + CMake")
    def test_cmake_resolver_finds_managed_dxc_and_vulkan(self):
        with tempfile.TemporaryDirectory() as td:
            base = Path(td)
            deps_root = base / "deps"
            for key in ("dxc", "vulkan-headers", "vulkan-loader"):
                self._fake_ready_dependency(deps_root, key)

            # A zero-length file is enough for find_library at configure time;
            # this test verifies resolver discovery, not linker execution.
            project = base / "project"
            (project / "tools" / "deps").mkdir(parents=True)
            shutil.copy2(ROOT / "tools" / "deps" / "manifest.json",
                         project / "tools" / "deps" / "manifest.json")
            cmake_file = project / "CMakeLists.txt"
            cmake_file.write_text(
                "cmake_minimum_required(VERSION 3.25)\n"
                "project(xenon_dependency_resolver_probe LANGUAGES CXX)\n"
                "set(XENON_DEPENDENCY_MODE MANAGED CACHE STRING \"\" FORCE)\n"
                "set(XENON_AUTO_BOOTSTRAP_DEPS OFF CACHE BOOL \"\" FORCE)\n"
                "set(XENON_FETCH_MISSING_DEPS OFF CACHE BOOL \"\" FORCE)\n"
                f"set(XENON_MANAGED_DEPS_ROOT \"{deps_root.as_posix()}\" CACHE PATH \"\" FORCE)\n"
                f"include(\"{(ROOT / 'cmake' / 'Dependencies.cmake').as_posix()}\")\n"
                "xenon_resolve_dxc(TEST_DXC TEST_DXC_INCLUDE)\n"
                "if(NOT TEST_DXC)\n  message(FATAL_ERROR \"managed DXC was not resolved\")\nendif()\n"
                "xenon_resolve_vulkan(TEST_VULKAN)\n"
                "if(NOT TEST_VULKAN)\n  message(FATAL_ERROR \"managed Vulkan was not resolved\")\nendif()\n",
                encoding="utf-8",
            )
            subprocess.run(
                ["cmake", "-S", str(project), "-B", str(base / "build")],
                check=True, capture_output=True, text=True,
            )

    def test_tar_extraction_accepts_safe_relative_symlink(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            archive = root / "safe.tar.gz"
            with tarfile.open(archive, "w:gz") as tf:
                payload = b"runtime"
                info = tarfile.TarInfo("lib/libdxcompiler.so.1")
                info.size = len(payload)
                tf.addfile(info, io.BytesIO(payload))
                link = tarfile.TarInfo("lib/libdxcompiler.so")
                link.type = tarfile.SYMTYPE
                link.linkname = "libdxcompiler.so.1"
                tf.addfile(link)
            out = root / "out"
            bootstrap._safe_extract_archive(archive, out)
            self.assertTrue((out / "lib" / "libdxcompiler.so").exists())

    def test_tar_extraction_rejects_link_escape(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            archive = root / "unsafe.tar.gz"
            with tarfile.open(archive, "w:gz") as tf:
                link = tarfile.TarInfo("lib/evil.so")
                link.type = tarfile.SYMTYPE
                link.linkname = "../../outside"
                tf.addfile(link)
            with self.assertRaises(bootstrap.BootstrapError):
                bootstrap._safe_extract_archive(archive, root / "out")


if __name__ == "__main__":
    unittest.main()

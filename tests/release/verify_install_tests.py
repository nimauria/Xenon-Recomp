import importlib.util
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "tools" / "release" / "verify_install.py"
spec = importlib.util.spec_from_file_location("xenon_verify_install", MODULE_PATH)
verify = importlib.util.module_from_spec(spec)
assert spec.loader
spec.loader.exec_module(verify)


class VerifyInstallTests(unittest.TestCase):
    def _populate_common(self, root: Path, platform: str) -> None:
        bindir = root / "bin"
        bindir.mkdir(parents=True)
        suffix = ".exe" if platform == "windows" else ""
        (bindir / f"xenon_launcher{suffix}").write_bytes(b"")
        (bindir / f"xenon_runtime_host{suffix}").write_bytes(b"")
        licenses = root / "share" / "licenses" / "Xenon"
        licenses.mkdir(parents=True)
        (licenses / "LICENSE").write_text("MIT")
        (licenses / "THIRD_PARTY_NOTICES.md").write_text("notices")
        (licenses / "THIRD_PARTY_SOURCE_OFFER.md").write_text("source offer")
        for name in ("xenia-ffmpeg", "dxc", "vulkan-loader", "vulkan-headers", "sdl2", "qt6"):
            directory = licenses / name
            directory.mkdir(parents=True)
            (directory / "LICENSE.txt").write_text(name)

    def test_complete_windows_tree_passes(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self._populate_common(root, "windows")
            bindir = root / "bin"
            for name in ("avcodec-1.dll", "avutil-1.dll", "dxcompiler.dll", "vulkan-1.dll", "Qt6Core.dll"):
                (bindir / name).write_bytes(b"")
            (root / "plugins" / "platforms").mkdir(parents=True)
            (root / "plugins" / "platforms" / "qwindows.dll").write_bytes(b"")
            missing = verify.verify_install(root, "windows", require_audio=True, require_dxc=True, require_vulkan=True)
            self.assertEqual(missing, [])

    def test_complete_linux_tree_passes_with_private_qt_layout(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self._populate_common(root, "linux")
            bindir = root / "bin"
            for name in ("libavcodec.so.1", "libavutil.so.1", "libdxcompiler.so", "libvulkan.so.1"):
                (bindir / name).write_bytes(b"")
            (bindir / "qt.conf").write_text("[Paths]\n")
            qtlib = root / "lib" / "xenon-recomp"
            qtlib.mkdir(parents=True)
            (qtlib / "libQt6Core.so.6").write_bytes(b"")
            (qtlib / "plugins" / "platforms").mkdir(parents=True)
            (qtlib / "plugins" / "platforms" / "libqxcb.so").write_bytes(b"")
            missing = verify.verify_install(root, "linux", require_audio=True, require_dxc=True, require_vulkan=True)
            self.assertEqual(missing, [])

    def test_missing_license_material_blocks_release(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self._populate_common(root, "windows")
            (root / "share" / "licenses" / "Xenon" / "dxc" / "LICENSE.txt").unlink()
            missing = verify.verify_install(root, "windows")
            self.assertIn("third-party license material: dxc", missing)


if __name__ == "__main__":
    unittest.main()

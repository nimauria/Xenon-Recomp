import concurrent.futures
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("compilation_cache", ROOT / "tools/compilation_cache.py")
cache = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cache)

class CacheTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="xenon-native-graph-")
        self.root = Path(self.tmp.name)
    def tearDown(self):
        self.tmp.cleanup()
    def test_atomic_parallel_and_corruption(self):
        payload = self.root / "input"
        payload.write_bytes(b"native bytes")
        request = {"stage": "object", "compiler": "A", "target": "x64", "flags": ["O2"]}
        with concurrent.futures.ThreadPoolExecutor(8) as pool:
            entries = list(pool.map(lambda _: cache.promote(self.root, request, payload), range(24)))
        self.assertEqual(len(set(entries)), 1)
        self.assertTrue(cache.verified(entries[0], request))
        for field in ("compiler", "target", "flags"):
            changed = dict(request, **{field: "different"})
            self.assertIsNone(cache.verified(entries[0], changed))
        (entries[0] / "payload").write_bytes(b"corrupt")
        self.assertIsNone(cache.verified(entries[0], request))
        self.assertTrue(cache.verified(cache.promote(self.root, request, payload), request))
        (entries[0] / "manifest.json").unlink()
        self.assertIsNone(cache.verified(entries[0], request))
    def test_real_compiler(self):
        compiler = os.environ.get("XENON_TEST_CXX") or shutil.which("cl.exe") or shutil.which("c++")
        self.assertIsNotNone(compiler, "real compiler is required")
        src, header, obj = self.root / "region.cpp", self.root / "value.h", self.root / "region.obj"
        src.write_text('#include "value.h"\nextern "C" int region() { return VALUE; }\n')
        header.write_text('#define VALUE 7\n')
        msvc = Path(compiler).name.lower() == "cl.exe"
        command = [compiler, "/nologo", "/c", "/O2", "/Brepro", "/showIncludes", str(src), "/Fo"+str(obj)] if msvc else [compiler,"-c","-O2",str(src),"-o",str(obj)]
        launcher = [sys.executable, str(ROOT / "tools/compilation_cache.py"), "compile", str(self.root / "cache")]
        def run():
            result=subprocess.run(launcher+command,capture_output=True)
            self.assertEqual(result.returncode,0,result.stdout.decode(errors="replace")+result.stderr.decode(errors="replace"))
            return result
        run(); first=obj.read_bytes();obj.unlink()
        self.assertIn(b"[Graph object] hit",run().stderr)
        self.assertEqual(first,obj.read_bytes())
        header.write_text('#define VALUE 8\n')
        self.assertIn(b"[Graph object] built",run().stderr)
        self.assertNotEqual(first,obj.read_bytes())
        command.insert(1,"/DNEW_SEMANTICS=1" if msvc else "-DNEW_SEMANTICS=1")
        self.assertIn(b"[Graph object] built",run().stderr)
        # A corrupt object never authorizes reuse.
        for node in (self.root / "cache/objects").iterdir():
            (node / "payload").write_bytes(b"bad")
        self.assertIn(b"[Graph object] built",run().stderr)

    def test_reuse_across_build_directories(self):
        # XEN-003: two modules' workspaces compile the shared runtime from one
        # source tree with one set of flags into different object paths; the
        # second reuses the first's verified object.
        compiler = os.environ.get("XENON_TEST_CXX") or shutil.which("cl.exe") or shutil.which("c++")
        self.assertIsNotNone(compiler, "real compiler is required")
        src = self.root / "runtime.cpp"
        src.write_text('extern "C" int runtime_value() { return 42; }\n')
        msvc = Path(compiler).name.lower() == "cl.exe"
        launcher = [sys.executable, str(ROOT / "tools/compilation_cache.py"), "compile", str(self.root / "cache")]
        objects = []
        outputs = []
        for workspace in ("module-a", "module-b"):
            obj = self.root / workspace / "build" / "runtime.obj"
            obj.parent.mkdir(parents=True)
            command = ([compiler, "/nologo", "/c", "/O2", "/Brepro", str(src), "/Fo" + str(obj)] if msvc
                       else [compiler, "-c", "-O2", str(src), "-o", str(obj)])
            result = subprocess.run(launcher + command, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
            outputs.append(result.stderr)
            objects.append(obj.read_bytes())
        self.assertIn(b"[Graph object] built", outputs[0])
        self.assertIn(b"[Graph object] hit", outputs[1])
        self.assertEqual(objects[0], objects[1])

    def test_clang_cl_with_option_terminator(self):
        # CMake ends clang-cl's options with "--" before the source; the
        # preprocessing run must still see /EP as an option, not a file name.
        compiler = os.environ.get("XENON_TEST_CLANG_CL") or shutil.which("clang-cl.exe")
        if not compiler and Path("C:/Program Files/LLVM/bin/clang-cl.exe").is_file():
            compiler = "C:/Program Files/LLVM/bin/clang-cl.exe"
        if not compiler:
            self.skipTest("clang-cl is not installed")
        src, obj = self.root / "clang_region.cpp", self.root / "clang_region.obj"
        src.write_text('extern "C" int clang_region() { return 3; }\n')
        command = [compiler, "/nologo", "/O2", "/Brepro", "/showIncludes", "/Fo" + str(obj), "-c", "--", str(src)]
        launcher = [sys.executable, str(ROOT / "tools/compilation_cache.py"), "compile", str(self.root / "cache")]
        first = subprocess.run(launcher + command, capture_output=True)
        self.assertEqual(first.returncode, 0, first.stderr.decode(errors="replace"))
        self.assertIn(b"[Graph object] built", first.stderr)
        obj.unlink()
        second = subprocess.run(launcher + command, capture_output=True)
        self.assertEqual(second.returncode, 0, second.stderr.decode(errors="replace"))
        self.assertIn(b"[Graph object] hit", second.stderr)
        # clang-cl is identified by its own executable, not every LLVM tool
        # beside it.
        identity = cache.compiler_identity(compiler, self.root / "cache")
        self.assertEqual(list(identity), [str(Path(compiler).resolve())])

    def test_toolchain_hash_memo_follows_file_changes(self):
        tool = self.root / "tool.exe"
        tool.write_bytes(b"one")
        memo_root = self.root / "cache"
        self.assertEqual(cache.memoized_file_hash(memo_root, tool), cache.file_hash(tool))
        self.assertEqual(cache.memoized_file_hash(memo_root, tool), cache.file_hash(tool))
        tool.write_bytes(b"two-two")
        self.assertEqual(cache.memoized_file_hash(memo_root, tool), cache.file_hash(tool))

if __name__ == "__main__":
    unittest.main()

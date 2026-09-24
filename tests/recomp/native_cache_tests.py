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

if __name__ == "__main__":
    unittest.main()

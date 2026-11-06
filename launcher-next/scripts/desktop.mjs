import { execFileSync } from "node:child_process";
import { readFile } from "node:fs/promises";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
const root = fileURLToPath(new URL("../", import.meta.url));
process.chdir(root);
const buildOnly = process.argv.includes("--build-only");
const runtimeOption = process.argv.indexOf("--runtime-host");
const runtimeValue =
  process.argv.find((arg) => arg.startsWith("--runtime-host="))?.slice(15) ||
  (runtimeOption >= 0
    ? process.argv[runtimeOption + 1]
    : process.env.XENON_RUNTIME_HOST);
if (runtimeOption >= 0 && (!runtimeValue || runtimeValue.startsWith("--")))
  throw Error("Supply a runtime host executable");
const runtimeHost = runtimeValue ? resolve(runtimeValue) : undefined;
const cefOption = process.argv.indexOf("--cef-root");
const cef =
  cefOption >= 0
    ? resolve(process.argv[cefOption + 1])
    : (await readFile(".tools/cef-root.txt", "utf8")).trim();
if (process.platform === "win32") {
  execFileSync(
    "powershell.exe",
    [
      "-NoProfile",
      "-ExecutionPolicy",
      "Bypass",
      "-File",
      "scripts/desktop.ps1",
      ...(buildOnly ? ["-BuildOnly"] : []),
      "-CefRoot",
      cef,
      ...(runtimeHost ? ["-RuntimeHost", runtimeHost] : []),
    ],
    { stdio: "inherit", windowsHide: true },
  );
} else {
  execFileSync("npm", ["run", "build"], { stdio: "inherit" });
  execFileSync(
    "cmake",
    [
      "-S",
      ".",
      "-B",
      "build/native",
      "-G",
      "Ninja",
      "-DCMAKE_BUILD_TYPE=Release",
      "-DXENON_NEXT_BUILD_DESKTOP=ON",
      "-DCEF_ROOT=" + cef,
      ...(process.env.XENON_NEXT_VERSION
        ? ["-DXENON_NEXT_VERSION=" + process.env.XENON_NEXT_VERSION]
        : []),
    ],
    { stdio: "inherit" },
  );
  execFileSync("cmake", ["--build", "build/native", "--parallel", "4"], {
    stdio: "inherit",
  });
  if (!buildOnly) {
    const exe =
      process.platform === "darwin"
        ? "build/native/desktop/XenonLauncher.app/Contents/MacOS/XenonLauncher"
        : "build/native/desktop/xenon_launcher_next";
    execFileSync(
      resolve(exe),
      runtimeHost ? ["--runtime-host=" + runtimeHost] : [],
      { stdio: "inherit" },
    );
  }
}

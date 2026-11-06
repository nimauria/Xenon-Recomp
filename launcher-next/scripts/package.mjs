import { execFileSync } from "node:child_process";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
const root = fileURLToPath(new URL("../", import.meta.url));
process.chdir(root);
if (!process.argv.includes("--skip-build"))
  execFileSync(process.execPath, ["scripts/desktop.mjs", "--build-only"], {
    stdio: "inherit",
    windowsHide: true,
  });
if (process.platform === "win32") {
  execFileSync(
    "powershell.exe",
    [
      "-NoProfile",
      "-ExecutionPolicy",
      "Bypass",
      "-File",
      "scripts/package.ps1",
      "-SkipBuild",
    ],
    { stdio: "inherit", windowsHide: true },
  );
} else {
  const build = resolve(process.env.XENON_PACKAGE_BUILD || "build/native");
  execFileSync(
    "cmake",
    ["-DBUILD_ROOT=" + build, "-P", "scripts/package-posix.cmake"],
    { stdio: "inherit" },
  );
}

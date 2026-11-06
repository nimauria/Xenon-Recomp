// Each check gets a fresh data folder and an owned CEF process. Never attaches
// to a user's launcher or enables Discord publishing.
import { spawn } from "node:child_process";
import { createWriteStream } from "node:fs";
import { mkdir, writeFile } from "node:fs/promises";
import { resolve, join } from "node:path";
import { fileURLToPath } from "node:url";
import { createServer } from "node:net";
import { chromium } from "playwright-core";
import { createNetworkFixtures } from "./create-network-fixtures.mjs";
import { createDemo } from "./demo.mjs";
process.chdir(fileURLToPath(new URL("../", import.meta.url)));
const build = resolve(process.env.XENON_TEST_BUILD || "build/native");
const desktop = resolve(
  process.env.XENON_TEST_DESKTOP || join(build, "desktop"),
);
const executable =
  process.platform === "darwin"
    ? join(desktop, "XenonLauncher.app/Contents/MacOS/XenonLauncher")
    : join(
        desktop,
        "xenon_launcher_next" + (process.platform === "win32" ? ".exe" : ""),
      );
const fixtureHost = join(
  build,
  "xenon_launcher_fixture_host" + (process.platform === "win32" ? ".exe" : ""),
);
const webRoot =
  process.platform === "darwin"
    ? join(desktop, "XenonLauncher.app/Contents/Resources/web")
    : join(desktop, "web");
const output = resolve(
  "build/desktop-validation-" +
    new Date().toISOString().replaceAll(/[:.]/g, "-"),
);
await mkdir(output, { recursive: true });
const plans = [
  ["refinements", "smoke-refinements.mjs", true, "demo"],
  ["demo", "smoke-demo.mjs", true, "demo"],
  ["game-settings", "smoke-game-settings.mjs"],
  ["desktop", "smoke-desktop.mjs"],
  ["controller", "smoke-controller-menus.mjs"],
  ["localization", "smoke-localization.mjs", false, false, [webRoot]],
  ["crash", "smoke-crash.mjs"],
  ["recovery-fr", "smoke-crash.mjs", false, false, ["fr"]],
  ["profiles", "smoke-profiles.mjs", true],
  ["media", "smoke-media-activity.mjs", true],
  ["modules", "smoke-module-lifecycle.mjs", true],
  ["captures", "smoke-captures.mjs"],
  ["migration", "smoke-migration.mjs"],
  ["shell", "smoke-shell.mjs"],
  ["services", "smoke-services.mjs", true],
  ["library", "smoke-library.mjs"],
  ["jobs", "smoke-jobs.mjs"],
  ["network", "smoke-network.mjs", false, true],
  ["presentation", "smoke-presentation-paths.mjs", true, true],
  ["profile-cache", "smoke-profile-cache.mjs", false, true],
  ["dlc", "smoke-dlc-catalog.mjs", false, true],
  ["actions", "smoke-parity-actions.mjs", true],
];
const requested = new Set(process.argv.slice(2));
for (const name of requested)
  if (!plans.some((p) => p[0] === name))
    throw Error("Unknown desktop check: " + name);
const report = [];
const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
async function freePort() {
  const server = createServer();
  await new Promise((resolve, reject) =>
    server.once("error", reject).listen(0, "127.0.0.1", resolve),
  );
  const port = server.address().port;
  await new Promise((resolve) => server.close(resolve));
  return port;
}
for (const [name, script, host, network, extra = []] of plans) {
  if (requested.size && !requested.has(name)) continue;
  const directory = join(output, name);
  await mkdir(directory, { recursive: true });
  const data = join(directory, "data"),
    port = await freePort(),
    endpoint = "http://127.0.0.1:" + port;
  const args = ["--data-dir=" + data, "--remote-debugging-port=" + port];
  const scriptArgs = [...extra];
  if (host) args.push("--fixture-host=" + fixtureHost);
  if (network === "demo") {
    const demo = await createDemo(directory);
    args.push("--fixture-network=" + demo.network);
  } else if (network) {
    const folder = join(directory, "network");
    await createNetworkFixtures(folder);
    args.push("--fixture-network=" + folder);
    if (["network", "presentation", "profile-cache"].includes(name))
      scriptArgs.push(folder);
  } else args.push("--offline");
  const log = createWriteStream(join(directory, "desktop.log"));
  const app = spawn(executable, args, {
    windowsHide: true,
    stdio: ["ignore", "pipe", "pipe"],
  });
  app.stdout.pipe(log, { end: false });
  app.stderr.pipe(log, { end: false });
  let exit, launchError;
  app.once("exit", (code, signal) => {
    exit = { code, signal };
  });
  app.once("error", (error) => {
    launchError = error;
  });
  const started = Date.now();
  console.log("Checking " + name + "...");
  let error;
  try {
    let ready = false;
    for (let i = 0; i < 200; i++) {
      if (launchError) throw launchError;
      if (exit)
        throw Error(
          "Launcher exited before readiness: " + JSON.stringify(exit),
        );
      try {
        const response = await fetch(endpoint + "/json/list");
        const tabs = await response.json();
        if (tabs.some((t) => t.url === "xenon://app/")) {
          ready = true;
          break;
        }
      } catch {}
      await delay(100);
    }
    if (!ready) throw Error("Launcher did not expose its test endpoint");
    const checkLog = createWriteStream(join(directory, "check.log"));
    const child = spawn(
      process.execPath,
      ["scripts/" + script, ...scriptArgs],
      {
        windowsHide: true,
        env: { ...process.env, CEF_DEBUG_URL: endpoint, CEF_KEEP_OPEN: "" },
        stdio: ["ignore", "pipe", "pipe"],
      },
    );
    child.stdout.pipe(checkLog, { end: false });
    child.stderr.pipe(checkLog, { end: false });
    const code = await new Promise((resolve, reject) => {
      const timeout = setTimeout(() => {
        child.kill();
        reject(Error("Desktop check exceeded 180 seconds"));
      }, 180000);
      child.once("error", reject);
      child.once("close", (code) => {
        clearTimeout(timeout);
        resolve(code);
      });
    });
    await new Promise((resolve) => checkLog.end(resolve));
    if (code !== 0)
      throw Error("Check failed; see " + join(directory, "check.log"));
  } catch (failure) {
    error = failure.message;
  } finally {
    try {
      const browser = await chromium.connectOverCDP(endpoint, {
        timeout: 2000,
      });
      for (const page of browser.contexts().flatMap((c) => c.pages()))
        if (page.url() === "xenon://app/") await page.close();
      await browser.close();
    } catch {}
    for (let i = 0; i < 50 && !exit && !launchError; i++) await delay(100);
    if (!exit && !launchError) {
      app.kill();
      for (let i = 0; i < 20 && !exit; i++) await delay(100);
    }
    log.end();
  }
  report.push({
    name,
    passed: !error,
    milliseconds: Date.now() - started,
    ...(error ? { error } : {}),
  });
  await writeFile(
    join(output, "results.json"),
    JSON.stringify({ desktop, checks: report }, null, 2) + "\n",
  );
  console.log(name + ": " + (error ? "FAILED - " + error : "passed"));
  if (error) process.exitCode = 1;
}
console.log("Desktop evidence: " + output);

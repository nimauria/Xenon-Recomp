import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { readFile, rename } from "node:fs/promises";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
process.chdir(fileURLToPath(new URL("../", import.meta.url)));
const storage = resolve(process.argv[2] || "");
assert.ok(
  storage.startsWith(resolve("build/startup-e2e-").slice(0, -1)),
  "Use an isolated startup-e2e fixture folder",
);
const original = await readFile(resolve(storage, "state.json"), "utf8");
const endpoint = process.env.CEF_DEBUG_URL || "http://127.0.0.1:9223";
async function connect() {
  for (let i = 0; i < 150; i++) {
    try {
      return await chromium.connectOverCDP(endpoint);
    } catch (error) {
      if (i === 149) throw error;
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
  }
}
let browser = await connect(),
  page = browser
    .contexts()
    .flatMap((c) => c.pages())
    .find((p) => p.url().startsWith("data:text/html"));
assert.ok(page);
await page
  .getByRole("heading", { name: "The launcher could not start", exact: true })
  .waitFor();
assert.equal(
  await readFile(resolve(storage, "state.json"), "utf8"),
  original,
  "Startup failure must not overwrite corrupt saved data",
);
const bridge = await page.evaluate(
  () =>
    new Promise((resolve) => {
      if (typeof window.cefQuery !== "function") {
        resolve(false);
        return;
      }
      window.cefQuery({
        request: JSON.stringify({
          version: 1,
          id: "fallback-check",
          command: "application.snapshot",
          payload: {},
        }),
        onSuccess: (raw) => resolve(JSON.parse(raw).ok === true),
        onFailure: () => resolve(false),
      });
      setTimeout(() => resolve(false), 1000);
    }),
);
assert.equal(
  bridge,
  false,
  "Startup recovery must not receive privileged application IPC",
);
await page.screenshot({ path: "build/startup-failure-verified.png" });
// Repair only the test fixture, retaining the original bytes as a separate backup.
await rename(
  resolve(storage, "state.json"),
  resolve(storage, "state.corrupt.json"),
);
const disconnected = new Promise((resolve) =>
  browser.once("disconnected", resolve),
);
await page
  .getByRole("link", { name: "Restart in safe mode", exact: true })
  .click();
await disconnected;
browser = await connect();
page = browser
  .contexts()
  .flatMap((c) => c.pages())
  .find((p) => p.url() === "xenon://app/");
assert.ok(page);
await page.getByRole("heading", { name: "Settings", exact: true }).waitFor();
const snapshot = await page.evaluate(
  () =>
    new Promise((resolve) =>
      window.cefQuery({
        request: JSON.stringify({
          version: 1,
          id: "ready-check",
          command: "application.snapshot",
          payload: {},
        }),
        onSuccess: (raw) => resolve(JSON.parse(raw).data),
      }),
    ),
);
assert.equal(snapshot.capabilities.runtime, false);
assert.equal(snapshot.capabilities.online, false);
assert.equal(
  await readFile(resolve(storage, "state.corrupt.json"), "utf8"),
  original,
);
await page.close();
await browser.close();
console.log(
  "Native startup-failure window, original-data preservation, blocked recovery IPC, safe-mode restart and retained corrupt fixture backup passed.",
);

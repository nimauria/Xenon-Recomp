import { chromium } from "playwright-core";
import { mkdir, writeFile, readFile } from "node:fs/promises";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
import assert from "node:assert/strict";
import { writeFixtureZip } from "./zip-fixture.mjs";
process.chdir(fileURLToPath(new URL("../", import.meta.url)));
const browser = await chromium.connectOverCDP(
  process.env.CEF_DEBUG_URL || "http://127.0.0.1:9223",
  { timeout: 10000 },
);
const page = browser
  .contexts()
  .flatMap((context) => context.pages())
  .find((page) => page.url() === "xenon://app/");
assert.ok(page);
page.setDefaultTimeout(15000);
const errors = [];
page.on("pageerror", (error) => errors.push(error.message));
async function rpc(action, data = {}) {
  const response = await page.evaluate(
    ({ action, data }) =>
      new Promise((resolve, reject) =>
        window.cefQuery({
          request: JSON.stringify({
            version: 1,
            id: crypto.randomUUID(),
            command:
              action === "snapshot"
                ? "application.snapshot"
                : "launcher.action",
            payload: action === "snapshot" ? {} : { action, data },
          }),
          onSuccess: (value) => resolve(JSON.parse(value)),
          onFailure: (_code, message) => reject(new Error(message)),
        }),
      ),
    { action, data },
  );
  assert.equal(response.ok, true, JSON.stringify(response.error));
  return response.data;
}
const navigate = (name) =>
  page
    .getByRole("navigation", { name: "Main navigation" })
    .getByRole("button", { name, exact: true })
    .click();
const operation = (id) => page.locator(`[data-job-id="${id}"]`);
async function terminal(id, state) {
  const deadline = Date.now() + 60000;
  while (Date.now() < deadline) {
    const actual = await operation(id).locator(".operation-state").innerText();
    if (actual === state) return;
    if (["failed", "cancelled", "interrupted", "completed"].includes(actual))
      throw new Error(
        `Expected ${state}, saw ${await operation(id).innerText()}`,
      );
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error("Operation did not finish within 60 seconds");
}
async function retried(id) {
  for (let i = 0; i < 100; i++) {
    const row = (await rpc("snapshot")).jobs.find((row) => row.retryOf === id);
    if (row) return row;
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  throw new Error("Retry was not queued");
}
const folder = resolve("build/jobs-smoke-fixtures");
await mkdir(folder, { recursive: true });
const stamp = Date.now();
const moduleId = `smoke.jobs.${stamp}`;
const zip = resolve(folder, "repairable.zip");
await writeFile(zip, "intentionally invalid ZIP");
let snapshot = await rpc("modules.installPackage", { path: zip });
const failedId = snapshot.jobs.at(-1).id;
await page.getByRole("button", { name: "Refresh", exact: true }).click();
await navigate("Downloads");
await terminal(failedId, "failed");
const manifest = (version) =>
  JSON.stringify({
    id: moduleId,
    name: "Background smoke module",
    version,
    settings: [],
  });
await writeFixtureZip(zip, {
  "module/module.json": manifest("1"),
  "module/example.txt": "fixture",
});
await operation(failedId)
  .getByRole("button", { name: "Retry", exact: true })
  .click();
const repaired = await retried(failedId);
await terminal(repaired.id, "completed");
assert.equal(
  (await rpc("snapshot")).modules.find((module) => module.id === moduleId)
    .version,
  "1",
);
const largeZip = resolve(folder, "background.zip");
const files = { "module/module.json": manifest("2") };
for (let i = 0; i < 15000; i++)
  files[`module/content/f${String(i).padStart(5, "0")}.bin`] =
    "test-only package data ".repeat(12);
await writeFixtureZip(largeZip, files);
const original = await readFile(largeZip);
snapshot = await rpc("modules.installPackage", { path: largeZip });
const cancelledId = snapshot.jobs.at(-1).id;
await page.getByRole("button", { name: "Refresh", exact: true }).click();
await operation(cancelledId)
  .getByRole("button", { name: "Cancel", exact: true })
  .click();
await terminal(cancelledId, "cancelled");
assert.equal(
  (await rpc("snapshot")).modules.find((module) => module.id === moduleId)
    .version,
  "1",
  "Cancellation must preserve the previous module",
);
assert.deepEqual(
  await readFile(largeZip),
  original,
  "The user package must remain unchanged",
);
await operation(cancelledId)
  .getByRole("button", { name: "Retry", exact: true })
  .click();
const running = await retried(cancelledId);
await navigate("Home");
await page
  .getByRole("heading", { name: "Home", level: 1, exact: true })
  .waitFor();
assert.ok(
  ["queued", "running", "committing"].includes(
    (await rpc("snapshot")).jobs.find((job) => job.id === running.id).state,
  ),
  "Retry must still be active when the renderer reloads",
);
await page.reload();
await page
  .getByRole("heading", { name: "Library", level: 1, exact: true })
  .waitFor();
await navigate("Downloads");
await page.screenshot({ path: "build/jobs-progress.png", fullPage: true });
await terminal(running.id, "completed");
snapshot = await rpc("snapshot");
assert.equal(
  snapshot.modules.find((module) => module.id === moduleId).version,
  "2",
  "A renderer reload must not interrupt native installation",
);
await page.screenshot({ path: "build/jobs-completed.png", fullPage: true });
assert.deepEqual(errors, []);
page.once("dialog", (dialog) => dialog.accept());
await page.getByRole("button", { name: "Clear finished", exact: true }).click();
await page
  .getByRole("heading", { name: "No operations to show", exact: true })
  .waitFor();
assert.equal(
  (await rpc("snapshot")).modules.find((module) => module.id === moduleId)
    .version,
  "2",
  "Clearing history must preserve installation",
);
console.log(
  "Packaged background jobs: failed ZIP/repair/retry, UI cancellation, preserved prior module, renderer reload during retry, completion and history clearing passed.",
);
await page.close();
await browser.close();

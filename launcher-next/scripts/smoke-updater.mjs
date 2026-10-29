import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { readFile, readdir } from "node:fs/promises";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
process.chdir(fileURLToPath(new URL("../", import.meta.url)));
const fixture = JSON.parse(
  await readFile("build/updater-fixture.json", "utf8"),
);
let browser;
for (let attempt = 0; attempt < 100; attempt++) {
  try {
    browser = await chromium.connectOverCDP(
      process.env.CEF_DEBUG_URL || "http://127.0.0.1:9223",
    );
    break;
  } catch (error) {
    if (attempt === 99) throw error;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
}
const page = browser
  .contexts()
  .flatMap((c) => c.pages())
  .find((p) => p.url() === "xenon://app/");
assert.ok(page);
page.setDefaultTimeout(20000);
page.on("dialog", (dialog) => dialog.accept());
async function rpc(command, payload = {}) {
  const reply = await page.evaluate(
    ({ command, payload }) =>
      new Promise((resolve) =>
        window.cefQuery({
          request: JSON.stringify({
            version: 1,
            id: crypto.randomUUID(),
            command,
            payload,
          }),
          onSuccess: (raw) => resolve(JSON.parse(raw)),
          onFailure: (_code, message) =>
            resolve({ ok: false, error: { message } }),
        }),
      ),
    { command, payload },
  );
  assert.ok(reply.ok, reply.error?.message);
  return reply.data;
}
const action = (action, data = {}) => rpc("launcher.action", { action, data });
async function until(predicate) {
  for (let i = 0; i < 2400; i++) {
    const state = await rpc("application.snapshot");
    if (predicate(state)) return state;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw Error("Update fixture timed out");
}
assert.equal(
  (await rpc("application.snapshot")).launcherUpdate.installedVersion,
  fixture.oldVersion,
);
await action("profiles.create", {
  name: "Update survival fixture",
  description: "This profile must survive replacement.",
});
await action("settings.set", { key: "ui/theme", value: "carbon" });
await action("updates.check");
await until((s) => s.launcherUpdate.state === "update-available");
await action("updates.download");
await until(
  (s) =>
    s.launcherUpdate.state === "ready-to-install" && !s.launcherUpdate.busy,
);
await page
  .getByRole("navigation", { name: "Main navigation" })
  .getByRole("button", { name: "Settings", exact: true })
  .click();
await page
  .getByRole("navigation", { name: "Settings categories" })
  .getByRole("button", { name: "Updates", exact: true })
  .click();
const updates = page.getByRole("region", { name: "Xenon Launcher updates" });
await updates
  .getByRole("button", { name: "Install verified package", exact: true })
  .waitFor();
const disconnected = new Promise((resolve) =>
  browser.once("disconnected", resolve),
);
await updates
  .getByRole("button", { name: "Install verified package", exact: true })
  .click();
let timeout;
try {
  await Promise.race([
    disconnected,
    new Promise((_, reject) => {
      timeout = setTimeout(
        () => reject(Error("Original launcher did not close")),
        180000,
      );
    }),
  ]);
} finally {
  clearTimeout(timeout);
}
let work, status;
for (let i = 0; i < 1200; i++) {
  const folders = await readdir(resolve(fixture.root, ".xenon-updates"));
  assert.equal(folders.length, 1);
  work = resolve(fixture.root, ".xenon-updates", folders[0]);
  try {
    status = JSON.parse(await readFile(resolve(work, "status.json"), "utf8"));
  } catch {}
  if (
    ["completed", "rolled-back", "failed", "recovery-required"].includes(
      status?.state,
    )
  )
    break;
  await new Promise((resolve) => setTimeout(resolve, 100));
}
assert.equal(status?.state, "completed", JSON.stringify(status));
const receipt = JSON.parse(await readFile(resolve(work, "ready.json"), "utf8"));
assert.equal(receipt.version, fixture.newVersion);
assert.equal(receipt.phase, "interface-ready");
const installed = JSON.parse(
  (
    await readFile(
      resolve(fixture.installation, "launcher-package.json"),
      "utf8",
    )
  ).replace(/^\uFEFF/, ""),
);
assert.equal(installed.version, fixture.newVersion);
const backup = JSON.parse(
  (
    await readFile(resolve(work, "backup/launcher-package.json"), "utf8")
  ).replace(/^\uFEFF/, ""),
);
assert.equal(backup.version, fixture.oldVersion);
const state = JSON.parse(
  await readFile(resolve(fixture.storage, "state.json"), "utf8"),
);
assert.ok(state.profiles.some((p) => p.name === "Update survival fixture"));
assert.equal(state.settings["ui/theme"], "carbon");
console.log(
  JSON.stringify(
    {
      result:
        "Real packaged update download/verification/install/restart/interface-ready handshake passed; previous install and user data retained.",
      work,
      receipt,
    },
    null,
    2,
  ),
);
await browser.close().catch(() => {});

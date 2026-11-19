import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { mkdir, writeFile, readFile, access } from "node:fs/promises";
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { fixtureModule, publishFixture } from "./create-network-fixtures.mjs";
process.chdir(fileURLToPath(new URL("../", import.meta.url)));
const network = resolve(process.argv[2] ?? "build/profile-cache-network");
let browser;
for (let i = 0; i < 100; i++) {
  try {
    browser = await chromium.connectOverCDP(
      process.env.CEF_DEBUG_URL || "http://127.0.0.1:9223",
    );
    break;
  } catch (error) {
    if (i === 99) throw error;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
}
const page = browser
  .contexts()
  .flatMap((c) => c.pages())
  .find((p) => p.url() === "xenon://app/");
assert.ok(page);
page.setDefaultTimeout(15000);
page.on("dialog", (d) => d.accept());
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
const nav = (name) =>
  page
    .getByRole("navigation", { name: "Main navigation" })
    .getByRole("button", { name, exact: true })
    .click();
async function until(predicate) {
  for (let i = 0; i < 400; i++) {
    const state = await rpc("application.snapshot");
    if (predicate(state)) return state;
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  throw Error("Profile/cache desktop fixture timed out");
}
async function run(name, data = {}) {
  const queued = await action(name, data),
    id = queued.jobs.at(-1).id;
  const state = await until((s) =>
    s.jobs.some(
      (j) =>
        j.id === id &&
        ["completed", "failed", "cancelled", "interrupted"].includes(j.state) &&
        !j.cleaningUp,
    ),
  );
  assert.equal(
    state.jobs.find((j) => j.id === id).state,
    "completed",
    state.jobs.find((j) => j.id === id).error,
  );
  return state;
}
const root = resolve(`build/profile-cache-ui-${Date.now()}`),
  destination = resolve(root, "Managed cache");
await mkdir(destination, { recursive: true });
await writeFile(resolve(destination, "Unrelated.txt"), "preserved");
await nav("Profiles");
await page.getByRole("button", { name: "Create profile", exact: true }).click();
let editor = page.getByRole("dialog", { name: "Create profile", exact: true });
await editor
  .getByRole("textbox", { name: "Name", exact: true })
  .fill("\u00c9lite");
await editor.getByRole("button", { name: "Save profile", exact: true }).click();
await page.getByRole("heading", { name: "\u00c9lite", exact: true }).waitFor();
await page.getByRole("button", { name: "Create profile", exact: true }).click();
await editor
  .getByRole("textbox", { name: "Name", exact: true })
  .fill("\u00e9LITE");
await editor.getByRole("button", { name: "Save profile", exact: true }).click();
await editor
  .getByRole("alert")
  .getByText("That profile name is already in use.", { exact: true })
  .waitFor();
assert.equal(await editor.isVisible(), true);
await editor
  .getByRole("textbox", { name: "Name", exact: true })
  .fill("\u041f\u0438\u043b\u043e\u0442");
await editor.getByRole("button", { name: "Save profile", exact: true }).click();
await page
  .getByRole("heading", { name: "\u041f\u0438\u043b\u043e\u0442", exact: true })
  .waitFor();
let state = await rpc("application.snapshot");
const unicode = state.profiles.find((p) => p.name === "\u00c9lite");
await page
  .locator(`[id="profile-${unicode.id}"]`)
  .getByRole("button", { name: "Duplicate", exact: true })
  .click();
await page
  .getByRole("heading", { name: "\u00c9lite copy", exact: true })
  .waitFor();
await writeFile(
  resolve(root, "profile.json"),
  JSON.stringify({
    schema: "xenon.launcher.profile",
    version: 1,
    profile: { profileName: "\u00e9LITE" },
  }),
);
await action("profiles.import", { path: resolve(root, "profile.json") });
await page
  .getByRole("heading", { name: "\u00e9LITE (2)", exact: true })
  .waitFor();
await run("catalog.refresh");
await run("modules.checkUpdate", { id: fixtureModule });
state = await run("modules.downloadUpdate", { id: fixtureModule });
const savedBefore = JSON.parse(
    await readFile(resolve(state.storagePath, "state.json"), "utf8"),
  ),
  oldPackage = savedBefore.moduleUpdates[fixtureModule]._packagePath;
await nav("Settings");
const categories = page.getByRole("navigation", {
  name: "Settings categories",
});
await categories.getByRole("button", { name: "Network", exact: true }).click();
await page
  .getByText(
    "Runtime network configuration is unavailable. Network status remains available on the Network page.",
    { exact: true },
  )
  .waitFor();
assert.equal(
  await page.getByRole("checkbox", { name: /^Enabled/ }).isDisabled(),
  true,
);
await page
  .getByRole("button", { name: "Open network status", exact: true })
  .click();
await page
  .getByRole("heading", { name: "Network service", exact: true })
  .waitFor();
await nav("Settings");
await categories
  .getByRole("button", { name: "Filesystem", exact: true })
  .click();
await page
  .getByRole("button", { name: "Open filesystem tools", exact: true })
  .click();
await page
  .getByRole("heading", { name: "Runtime filesystem", exact: true })
  .waitFor();
assert.equal(
  await page
    .getByRole("textbox", { name: "Guest mount point", exact: true })
    .isDisabled(),
  true,
);
await nav("Settings");
await categories.getByRole("button", { name: "Paths", exact: true }).click();
const cache = page.getByRole("article", {
  name: "Cache destination",
  exact: true,
});
await cache
  .getByRole("button", { name: "Choose cache folder", exact: true })
  .waitFor();
state = await run("settings.set", { key: "paths/cache", value: destination });
const customRoot = state.cache.effectivePath;
assert.equal(dirname(customRoot), destination);
await cache.getByText(customRoot, { exact: true }).waitFor();
await cache.getByText(/Previous cache copies/).waitFor();
assert.equal(
  await readFile(resolve(destination, "Unrelated.txt"), "utf8"),
  "preserved",
);
await access(oldPackage);
const copied = JSON.parse(
  await readFile(resolve(state.storagePath, "state.json"), "utf8"),
).moduleUpdates[fixtureModule]._packagePath;
assert.equal(dirname(dirname(copied)), resolve(customRoot, "downloads"));
await run("modules.installDownloaded", { id: fixtureModule });
await page.evaluate(
  ({ id }) =>
    new Promise((resolve, reject) => {
      const image = new Image();
      image.src = `xenon://app/catalog-tile/${id}?probe=${Date.now()}`;
      image.decode().then(resolve).catch(reject);
    }),
  { id: fixtureModule },
);
const dismissToast = page.getByRole("button", {
  name: "Dismiss toast",
  exact: true,
});
if (await dismissToast.count()) await dismissToast.click();
await cache.evaluate((node) => node.scrollIntoView({ block: "start" }));
await page.screenshot({ path: "build/profile-cache-verified.png" });
await cache
  .getByRole("button", { name: "Use default cache folder", exact: true })
  .click();
state = await until((s) => !s.cache.pending && s.cache.configuredPath === "");
const defaultRoot = state.cache.effectivePath;
assert.equal(dirname(defaultRoot), state.storagePath);
await access(copied);
assert.ok(state.cache.previousLocations.includes(customRoot));
await publishFixture(network, "2.0.0");
await run("modules.checkUpdate", { id: fixtureModule });
state = await run("modules.downloadUpdate", { id: fixtureModule });
const nextPackage = JSON.parse(
  await readFile(resolve(state.storagePath, "state.json"), "utf8"),
).moduleUpdates[fixtureModule]._packagePath;
assert.equal(dirname(dirname(nextPackage)), resolve(defaultRoot, "downloads"));
await page.reload();
state = await rpc("application.snapshot");
assert.equal(state.cache.effectivePath, defaultRoot);
assert.equal(
  state.profiles.filter((p) =>
    ["\u00c9lite", "\u00e9LITE (2)", "\u041f\u0438\u043b\u043e\u0442"].includes(
      p.name,
    ),
  ).length,
  3,
);
await page.evaluate(
  (id) =>
    new Promise((resolve, reject) => {
      const image = new Image();
      image.src = `xenon://app/catalog-tile/${id}?after=${Date.now()}`;
      image.decode().then(resolve).catch(reject);
    }),
  fixtureModule,
);
console.log(
  "Desktop Unicode create/duplicate/import, inline duplicate validation, network configuration gating, managed cache copy/reset/reload, decoded artwork, install from copied package, future downloads and retained original/unrelated files passed. Cache destination/source selection used native RPC.",
);
if (!process.env.CEF_KEEP_OPEN) await page.close();
await browser.close();

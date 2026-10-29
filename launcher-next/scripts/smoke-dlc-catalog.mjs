import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { mkdir, writeFile, access } from "node:fs/promises";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
process.chdir(fileURLToPath(new URL("../", import.meta.url)));
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
  for (let i = 0; i < 300; i++) {
    const state = await rpc("application.snapshot");
    if (predicate(state)) return state;
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  throw Error("Catalogue add-on fixture timed out");
}
async function exists(path) {
  try {
    await access(path);
    return true;
  } catch {
    return false;
  }
}
const root = resolve(`build/dlc-catalog-ui-${Date.now()}`),
  modulePath = resolve(root, "Module"),
  gamePath = resolve(root, "Game"),
  sources = resolve(root, "Originals"),
  replacement = resolve(root, "Replacement");
for (const path of [modulePath, gamePath, sources, replacement])
  await mkdir(path, { recursive: true });
await writeFile(
  resolve(modulePath, "module.json"),
  JSON.stringify({
    id: "catalogue.dlc.fixture",
    name: "Catalogue DLC fixture",
    version: "1.0.0",
    addOns: Array.from({ length: 65 }, (_, i) => ({
      dlcId: `mission:${i}`,
      name: `Mission ${i}`,
      description: "User-provided catalogue test content",
      content_ids: [`expected-${i}`],
      group: "Fixture missions",
    })),
  }),
);
await writeFile(resolve(sources, "A.bin"), "A fixture");
await writeFile(resolve(sources, "B.bin"), "B fixture");
await writeFile(resolve(replacement, "new.bin"), "replacement fixture");
await action("settings.set", {
  key: "paths/games",
  value: resolve(root, "Managed"),
});
await action("modules.import", { path: modulePath });
await action("library.import", { paths: [gamePath] });
let state = await rpc("application.snapshot");
const game = state.games.find((g) => g.contentPath === gamePath);
assert.ok(game);
await action("library.update", {
  id: game.id,
  moduleId: "catalogue.dlc.fixture",
});
await page
  .getByRole("navigation", { name: "Main navigation" })
  .getByRole("button", { name: "Library", exact: true })
  .click();
await page.getByRole("button", { name: "Game", exact: true }).click();
const addons = page.getByRole("region", { name: "Game add-ons", exact: true });
await addons.locator(".addon-row").first().waitFor();
assert.equal(await addons.locator(".addon-row").count(), 32);
await addons
  .getByRole("button", { name: "Show more add-ons", exact: true })
  .click();
assert.equal(await addons.locator(".addon-row").count(), 64);
await addons
  .getByRole("searchbox", { name: "Search add-ons", exact: true })
  .fill("Mission 64");
assert.equal(await addons.locator(".addon-row").count(), 1);
const row = addons.locator('[data-addon-id="mission:64"]');
await row.getByRole("button", { name: "Import files", exact: true }).waitFor();
await action("dlc.import", {
  id: game.id,
  catalogId: "mission:64",
  paths: [resolve(sources, "A.bin"), resolve(sources, "B.bin")],
});
state = await until(
  (s) =>
    s.games.find((g) => g.id === game.id)?.dlc.length === 1 &&
    s.jobs.at(-1)?.state === "completed",
);
const first = state.games.find((g) => g.id === game.id).dlc[0];
assert.equal(first.catalogId, "mission:64");
assert.equal(await exists(resolve(first.path, "A.bin")), true);
assert.equal(await exists(resolve(first.path, "B.bin")), true);
await row.getByRole("button", { name: "Check copy", exact: true }).click();
await until(
  (s) => s.games.find((g) => g.id === game.id).dlc[0]?.receiptVerified === true,
);
await action("dlc.import", {
  id: game.id,
  catalogId: "mission:64",
  paths: [replacement],
});
state = await until(
  (s) =>
    s.games.find((g) => g.id === game.id)?.dlc[0]?.path !== first.path &&
    s.jobs.at(-1)?.state === "completed",
);
const current = state.games.find((g) => g.id === game.id).dlc[0];
assert.equal(current.id, first.id);
assert.equal(await exists(resolve(first.path, "A.bin")), true);
await writeFile(resolve(first.path, "A.bin"), "changed previous copy");
await row.getByRole("button", { name: "Check copy", exact: true }).click();
await until(
  (s) => s.games.find((g) => g.id === game.id).dlc[0]?.receiptVerified === true,
);
await row.scrollIntoViewIfNeeded();
await page.screenshot({ path: "build/dlc-catalog-verified.png" });
await action("dlc.remove", { id: game.id, dlcId: current.id });
await until(
  (s) =>
    !s.games.find((g) => g.id === game.id).dlc.length &&
    s.jobs.at(-1)?.state === "completed",
);
assert.equal(await exists(first.path), false);
assert.equal(await exists(current.path), false);
assert.equal(await exists(resolve(sources, "A.bin")), true);
assert.equal(await exists(resolve(replacement, "new.bin")), true);
await row.getByText("Not imported", { exact: true }).waitFor();
await page.reload();
state = await rpc("application.snapshot");
assert.equal(state.dlcCatalog["catalogue.dlc.fixture"].length, 65);
assert.equal(state.games.find((g) => g.id === game.id).dlc.length, 0);
console.log(
  "Packaged declared add-on catalogue, bounded rows/search/pagination, multi-file native import, receipt check, replacement with retained previous copy, stable identity, owned deletion and original preservation passed. Source selection/deletion used native RPC.",
);
if (!process.env.CEF_KEEP_OPEN) await page.close();
await browser.close();

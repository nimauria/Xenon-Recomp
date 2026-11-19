import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { mkdir, readFile, writeFile, access } from "node:fs/promises";
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { writeFixtureZip } from "./zip-fixture.mjs";
process.chdir(fileURLToPath(new URL("../", import.meta.url)));
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
  throw Error("Module lifecycle fixture timed out");
}
async function exists(path) {
  try {
    await access(path);
    return true;
  } catch {
    return false;
  }
}
const root = resolve(`build/module-lifecycle-ui-${Date.now()}`),
  external = resolve(root, "External"),
  zip = resolve(root, "module.zip"),
  game = resolve(root, "Game.iso");
await mkdir(external, { recursive: true });
await writeFile(game, "original game fixture");
await action("settings.set", {
  key: "paths/modules",
  value: resolve(root, "Installed"),
});
for (const version of ["1", "2"]) {
  await writeFixtureZip(zip, {
    "module/module.json": JSON.stringify({
      id: "desktop.lifecycle",
      name: "Desktop lifecycle",
      version,
      runtime_apis: { input: { version: 1 } },
    }),
    "module/payload.txt": "package data",
  });
  await action("modules.installPackage", { path: zip });
  await until((s) => s.jobs.at(-1)?.state === "completed");
}
const definition = (version) =>
  JSON.stringify({
    id: "external.lifecycle",
    name: "External lifecycle",
    runtime: { apis: { input: { version } } },
  });
await writeFile(resolve(external, "module.json"), definition(2));
await action("modules.import", { path: external });
await page
  .getByRole("navigation", { name: "Main navigation" })
  .getByRole("button", { name: "Modules", exact: true })
  .click();
const ownedCard = page.locator('[id="module-desktop.lifecycle"]'),
  externalCard = page.locator('[id="module-external.lifecycle"]');
await ownedCard
  .getByRole("button", { name: "Uninstall owned files", exact: true })
  .waitFor();
assert.equal(
  await externalCard
    .getByRole("button", { name: "Uninstall owned files", exact: true })
    .count(),
  0,
);
await externalCard
  .getByRole("list", { name: "External lifecycle compatibility problems" })
  .getByText("Input API v2 exceeds the supported v1 contract", { exact: true })
  .waitFor();
await action("library.import", { paths: [game] });
let state = await rpc("application.snapshot");
const gameId = state.games.find((g) => g.contentPath === game).id;
await action("library.update", { id: gameId, moduleId: "external.lifecycle" });
const nav = (name) =>
  page
    .getByRole("navigation", { name: "Main navigation" })
    .getByRole("button", { name, exact: true })
    .click();
await nav("Library");
await page
  .getByRole("alert")
  .getByText("Input API v2 exceeds the supported v1 contract", { exact: true })
  .waitFor();
assert.equal(
  await page
    .getByRole("button", { name: /^(Play|Start test session)$/ })
    .isDisabled(),
  true,
);
await nav("Modules");
await writeFile(resolve(external, "module.json"), definition(1));
await externalCard
  .getByRole("button", { name: "Check compatibility", exact: true })
  .click();
await until(
  (s) =>
    s.modules.find((m) => m.id === "external.lifecycle").compatibility.issues
      .length === 0,
);
await nav("Library");
assert.equal(
  await page
    .getByRole("button", { name: /^(Play|Start test session)$/ })
    .isDisabled(),
  false,
);
await nav("Modules");
await action("library.update", { id: gameId, moduleId: "desktop.lifecycle" });
await action("session.launch", { gameId });
state = await until((s) => s.session.state === "running");
const config = JSON.parse(
  await readFile(
    resolve(
      state.storagePath,
      "sessions",
      state.session.sessionId,
      "launch-config.json",
    ),
    "utf8",
  ),
);
assert.deepEqual(config.runtimeApiRequirements, {
  input: { version: 1, required: true },
});
assert.equal(config.moduleCompatibility.binaryAbiVerified, false);
await action("session.stop");
await until((s) => s.session.state === "stopped");
await page.screenshot({ path: "build/module-lifecycle-verified.png" });
state = await rpc("application.snapshot");
const owned = state.modules.find((m) => m.id === "desktop.lifecycle"),
  ownedRoots = [owned.path, ...owned.previousVersions.map((p) => p.path)].map(
    (path) => dirname(dirname(path)),
  );
await action("modules.uninstall", { id: owned.id });
await until((s) => s.jobs.at(-1)?.state === "completed");
await ownedCard.waitFor({ state: "detached" });
for (const path of ownedRoots) assert.equal(await exists(path), false);
assert.equal(await exists(game), true);
assert.equal(await exists(zip), true);
assert.equal(await exists(resolve(external, "module.json")), true);
state = await rpc("application.snapshot");
assert.equal(state.games.find((g) => g.id === gameId).moduleId, "");
await action("modules.remove", { id: "external.lifecycle" });
await page.reload();
state = await rpc("application.snapshot");
assert.equal(state.modules.length, 0);
assert.equal(await exists(resolve(external, "module.json")), true);
console.log(
  "Desktop compatibility display/recheck, external-folder controls, normalized API handoff, owned current/retained-version uninstall, unlink, preserved original game/package/external folder and reload passed. Fixture paths and deletion used native RPC.",
);
if (!process.env.CEF_KEEP_OPEN) await page.close();
await browser.close();

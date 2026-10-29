import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
process.chdir(fileURLToPath(new URL("../", import.meta.url)));
const browser = await chromium.connectOverCDP(
  process.env.CEF_DEBUG_URL || "http://127.0.0.1:9223",
);
const page = browser
  .contexts()
  .flatMap((c) => c.pages())
  .find((p) => p.url() === "xenon://app/");
assert.ok(page);
page.setDefaultTimeout(15000);
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
const nav = (name) =>
  page
    .getByRole("navigation", { name: "Main navigation" })
    .getByRole("button", { name, exact: true })
    .click();
async function until(predicate) {
  for (let i = 0; i < 300; i++) {
    const state = await rpc("application.snapshot");
    if (predicate(state)) return state;
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  throw Error("Parity action fixture timed out");
}
await action("settings.set", { key: "audio/masterVolume", value: 0.2 });
await action("settings.set", { key: "audio/muteUnfocused", value: true });
await nav("Settings");
await page
  .getByRole("navigation", { name: "Settings categories" })
  .getByRole("button", { name: "Audio", exact: true })
  .click();
const definitions = JSON.parse(
    await readFile("contracts/settings.json", "utf8"),
  ),
  volume = definitions.find((d) => d.key === "audio/masterVolume");
await page
  .getByRole("button", { name: `Reset ${volume.label}`, exact: true })
  .click();
let state = await until((s) => s.settings["audio/masterVolume"] === undefined);
assert.equal(state.settings["audio/muteUnfocused"], true);
const root = resolve(`build/module-rescan-ui-${Date.now()}`),
  folder = resolve(root, "Example"),
  game = resolve(root, "Game");
await mkdir(folder, { recursive: true });
await mkdir(game, { recursive: true });
const definition = {
  id: "migration.scan.fixture",
  name: "Module scan fixture",
  version: "1.0.0",
  settings: [{ id: "flag", type: "bool", default: true }],
};
await writeFile(resolve(folder, "module.json"), JSON.stringify(definition));
await action("settings.set", { key: "paths/modules", value: root });
await nav("Modules");
await page
  .getByRole("button", { name: "Rescan installed modules", exact: true })
  .click();
await until((s) => s.modules.some((m) => m.id === definition.id));
const card = page.locator(`[id="module-${definition.id}"]`);
await card.getByRole("button", { name: "Disable", exact: true }).click();
await until(
  (s) => s.modules.find((m) => m.id === definition.id).enabled === false,
);
await action("modules.setting", {
  id: definition.id,
  key: "flag",
  value: false,
});
await action("library.import", { paths: [game] });
state = await rpc("application.snapshot");
const registered = state.games.find((g) => g.contentPath === game);
assert.ok(registered);
await action("library.update", { id: registered.id, moduleId: definition.id });
await card.getByRole("button", { name: "Unlink games", exact: true }).click();
state = await until(
  (s) => s.games.find((g) => g.id === registered.id).moduleId === "",
);
assert.ok(state.modules.some((m) => m.id === definition.id));
definition.version = "2.0.0";
await writeFile(resolve(folder, "module.json"), JSON.stringify(definition));
await page
  .getByRole("button", { name: "Rescan installed modules", exact: true })
  .click();
state = await until(
  (s) => s.modules.find((m) => m.id === definition.id).version === "2.0.0",
);
assert.equal(state.modules.find((m) => m.id === definition.id).enabled, false);
assert.equal(
  state.modules.find((m) => m.id === definition.id).settings.flag,
  false,
);
await writeFile(resolve(folder, "module.json"), "{invalid");
await page
  .getByRole("button", { name: "Rescan installed modules", exact: true })
  .click();
state = await until(
  (s) =>
    s.jobs.at(-1)?.state === "completed" && Boolean(s.jobs.at(-1)?.warning),
);
assert.equal(
  state.modules.find((m) => m.id === definition.id).version,
  "2.0.0",
);
await nav("Downloads");
await page
  .getByText(
    "Some module locations could not be read. Their registered data was preserved.",
    { exact: true },
  )
  .waitFor();
await page.screenshot({ path: "build/parity-actions-verified.png" });
console.log(
  "Packaged individual-setting reset, preserved neighbouring preferences, background module discovery/rescan, settings/disabled-state preservation, explicit game unlink and unreadable-manifest warnings passed.",
);
if (!process.env.CEF_KEEP_OPEN) await page.close();
await browser.close();

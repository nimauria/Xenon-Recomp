import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { mkdir, writeFile, access } from "node:fs/promises";
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
          onSuccess: (value) => resolve(JSON.parse(value)),
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
  throw Error("Library state did not reach the expected result");
}
async function exists(path) {
  try {
    await access(path);
    return true;
  } catch {
    return false;
  }
}
const fixture = resolve(`build/library-ui-content-${Date.now()}`),
  managed = resolve(`build/library-ui-managed-${Date.now()}`);
await mkdir(resolve(fixture, "Game", "Empty"), { recursive: true });
await writeFile(
  resolve(fixture, "Game", "default.xex"),
  "Managed game fixture; no game code executed.",
);
await writeFile(resolve(fixture, "addon.bin"), "Managed DLC fixture.");
await mkdir(resolve(managed, "Foreign"), { recursive: true });
await writeFile(resolve(managed, "Foreign", "keep.txt"), "Unowned content.");
await action("settings.set", { key: "paths/games", value: managed });
await page
  .getByRole("navigation", { name: "Main navigation" })
  .getByRole("button", { name: "Library", exact: true })
  .click();
await action("library.import", {
  paths: [resolve(fixture, "Game")],
  mode: "keep",
});
await page.getByRole("button", { name: "Properties", exact: true }).click();
await page
  .getByRole("dialog", { name: "Game properties" })
  .getByRole("button", { name: "Move into Xenon Library", exact: true })
  .click();
let state = await until(
  (s) =>
    s.games.some((g) => g.title === "Game" && g.managedCopies?.length) &&
    s.jobs.at(-1)?.state === "completed",
);
const game = state.games.find((g) => g.title === "Game");
assert.equal(await exists(resolve(fixture, "Game")), false);
assert.equal(await exists(resolve(game.contentPath, "default.xex")), true);
await page.getByRole("button", { name: "Game", exact: true }).click();
await action("dlc.import", {
  id: game.id,
  paths: [resolve(fixture, "addon.bin")],
});
state = await until(
  (s) =>
    s.games.find((g) => g.id === game.id)?.dlc.length === 1 &&
    s.jobs.at(-1)?.state === "completed",
);
const addon = state.games.find((g) => g.id === game.id).dlc[0];
await page.getByRole("button", { name: "Check copy", exact: true }).click();
await until(
  (s) => s.games.find((g) => g.id === game.id).dlc[0]?.receiptVerified === true,
);
assert.equal(await exists(resolve(fixture, "addon.bin")), true);
assert.equal(await exists(addon.path), true);
await action("dlc.remove", { id: game.id, dlcId: addon.id });
await until(
  (s) =>
    !s.games.find((g) => g.id === game.id).dlc.length &&
    s.jobs.at(-1)?.state === "completed",
);
assert.equal(await exists(addon.path), false);
assert.equal(await exists(resolve(fixture, "addon.bin")), true);
await page.reload();
await page.getByRole("button", { name: "Properties", exact: true }).click();
const properties = page.getByRole("dialog", { name: "Game properties" });
await properties
  .getByLabel("Title", { exact: true })
  .fill("Managed fixture game");
await properties.getByRole("button", { name: "Save", exact: true }).click();
await page.getByRole("button", { name: "Properties", exact: true }).click();
assert.equal(
  await properties
    .getByRole("button", { name: "Remove reference", exact: true })
    .isDisabled(),
  true,
);
await page.screenshot({ path: "build/library-managed-verified.png" });
await properties
  .getByRole("button", { name: "Delete managed files", exact: true })
  .waitFor();
await properties
  .getByRole("button", { name: "Close dialog", exact: true })
  .click();
await action("library.deleteManaged", { id: game.id });
state = await until(
  (s) =>
    !s.games.find((g) => g.id === game.id).managedCopies.length &&
    s.jobs.at(-1)?.state === "completed",
);
assert.equal(await exists(resolve(game.contentPath, "default.xex")), false);
assert.equal(await exists(resolve(managed, "Foreign", "keep.txt")), true);
assert.equal(
  state.games.find((g) => g.id === game.id).title,
  "Managed fixture game",
);
await page.reload();
state = await rpc("application.snapshot");
assert.equal(state.games.find((g) => g.id === game.id).managedCopies.length, 0);
console.log(
  "Packaged source registration, properties move confirmation, verified native transfer, DLC copy/delete, managed deletion boundaries and reload persistence passed. File selection and deletion used native RPC; Windows picker interaction remains unverified.",
);
if (!process.env.CEF_KEEP_OPEN) await page.close();
await browser.close();

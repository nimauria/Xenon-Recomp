import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { mkdir, writeFile, access, readFile } from "node:fs/promises";
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
let keepUnsaved = false;
page.on("dialog", (dialog) =>
  keepUnsaved && dialog.message().startsWith("Discard")
    ? dialog.dismiss()
    : dialog.accept(),
);
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
  throw Error("Media/activity fixture timed out");
}
async function exists(path) {
  try {
    await access(path);
    return true;
  } catch {
    return false;
  }
}
const root = resolve(`build/media-ui-${Date.now()}`);
await mkdir(root, { recursive: true });
const first = resolve(root, "Disc1.iso"),
  second = resolve(root, "Disc2.iso");
await writeFile(first, "first disc fixture");
await writeFile(second, "second disc fixture");
await action("settings.set", {
  key: "paths/games",
  value: resolve(root, "Managed"),
});
await action("library.import", { paths: [first] });
let state = await rpc("application.snapshot");
const game = state.games.find((g) => g.contentPath === first);
assert.ok(game);
await action("library.media.add", { id: game.id, paths: [second] });
state = await rpc("application.snapshot");
const media = state.games.find((g) => g.id === game.id).media,
  firstId = media[0].id,
  secondId = media[1].id;
const nav = (name) =>
  page
    .getByRole("navigation", { name: "Main navigation" })
    .getByRole("button", { name, exact: true })
    .click();
await nav("Library");
await page.getByRole("button", { name: "Disc1", exact: true }).click();
await page.getByRole("button", { name: "Properties", exact: true }).click();
const properties = page.getByRole("dialog", {
    name: "Game properties",
    exact: true,
  }),
  discs = properties.getByRole("region", {
    name: "Game discs and media",
    exact: true,
  });
await discs
  .getByRole("combobox", { name: "Edit media details", exact: true })
  .selectOption(secondId);
await discs
  .getByRole("textbox", { name: "Media name", exact: true })
  .fill("PAL Disc Two");
await discs
  .getByRole("spinbutton", { name: "Disc count", exact: true })
  .fill("2");
await discs
  .getByRole("spinbutton", { name: "Disc number", exact: true })
  .fill("2");
await discs
  .getByRole("textbox", { name: "Media identifier", exact: true })
  .fill("ABCD1234");
await discs
  .getByRole("textbox", { name: "Media region", exact: true })
  .fill("PAL");
await page.waitForFunction(
  () =>
    Array.from(document.querySelectorAll("dialog button")).find(
      (b) => b.textContent.trim() === "Save",
    )?.disabled === true,
);
keepUnsaved = true;
await properties
  .getByRole("button", { name: "Close dialog", exact: true })
  .click();
assert.equal(await properties.isVisible(), true);
keepUnsaved = false;
await discs
  .getByRole("button", { name: "Save disc details", exact: true })
  .click();
await until(
  (s) => s.games.find((g) => g.id === game.id).media[1].mediaId === "ABCD1234",
);
const secondRow = discs.locator(`[data-media-id="${secondId}"]`),
  firstRow = discs.locator(`[data-media-id="${firstId}"]`);
await secondRow
  .getByRole("button", { name: "Use as primary", exact: true })
  .click();
await until(
  (s) => s.games.find((g) => g.id === game.id).contentPath === second,
);
await action("session.launch", { gameId: game.id });
state = await until((s) => s.session.state === "running");
assert.equal(
  await firstRow
    .getByRole("button", { name: "Use as primary", exact: true })
    .isDisabled(),
  true,
);
assert.ok(state.storagePath);
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
assert.equal(config.contentPath, second);
assert.equal(config.primaryMediaId, secondId);
assert.equal(config.media.length, 2);
await action("session.stop");
await until((s) => s.session.state === "stopped" && s.history.length === 1);
await properties
  .getByRole("button", { name: "Close dialog", exact: true })
  .click();
await nav("Home");
await page.getByText(/Test activity: 1 sessions/).waitFor();
state = await rpc("application.snapshot");
assert.equal(state.activitySummary.playCount, 0);
assert.equal(state.activitySummary.totalPlayTimeMs, 0);
assert.ok(state.history[0].durationMs > 0);
await action("history.clear");
await until((s) => s.history.length === 0);
await page.reload();
await nav("Home");
state = await rpc("application.snapshot");
assert.equal(state.history.length, 0);
assert.equal(state.activitySummary.fixturePlayCount, 1);
await nav("Library");
await page.getByRole("button", { name: "Properties", exact: true }).click();
await discs
  .getByRole("combobox", { name: "Edit media details", exact: true })
  .selectOption(secondId);
assert.equal(
  await discs
    .getByRole("textbox", { name: "Media region", exact: true })
    .inputValue(),
  "PAL",
);
await firstRow
  .getByRole("button", { name: "Use as primary", exact: true })
  .click();
await action("library.transfer", {
  id: game.id,
  mediaId: secondId,
  path: second,
  mode: "move",
});
state = await until((s) => s.jobs.at(-1)?.state === "completed");
assert.equal(state.games.find((g) => g.id === game.id).contentPath, first);
assert.equal(await exists(second), false);
await discs.scrollIntoViewIfNeeded();
await page.screenshot({ path: "build/media-activity-verified.png" });
await action("library.deleteManaged", { id: game.id, mediaId: secondId });
await until((s) => s.jobs.at(-1)?.state === "completed");
await secondRow
  .getByText("Source missing; reference kept.", { exact: true })
  .waitFor();
assert.equal(await exists(first), true);
await action("library.media.remove", { id: game.id, mediaId: secondId });
state = await rpc("application.snapshot");
assert.equal(state.games.find((g) => g.id === game.id).media.length, 1);
console.log(
  "Packaged media grouping, disc details/unsaved guard, primary selection, running-session lock, full launch linkage, isolated fixture playtime, cleared-history persistence, secondary move/delete and metadata reload passed. Source selection and deletion used native RPC.",
);
if (!process.env.CEF_KEEP_OPEN) await page.close();
await browser.close();

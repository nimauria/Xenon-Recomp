import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { mkdir, writeFile } from "node:fs/promises";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
process.chdir(fileURLToPath(new URL("../", import.meta.url)));
let browser;
for (let attempt = 0; attempt < 100; ++attempt) {
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
let discard = false;
page.on("dialog", (d) => (discard ? d.accept() : d.dismiss()));
async function rpc(command, payload = {}) {
  const reply = await page.evaluate(
    ({ command, payload }) =>
      new Promise((resolve, reject) =>
        window.cefQuery({
          request: JSON.stringify({
            version: 1,
            id: crypto.randomUUID(),
            command,
            payload,
          }),
          onSuccess: (raw) => resolve(JSON.parse(raw)),
          onFailure: (_c, m) => reject(Error(m)),
        }),
      ),
    { command, payload },
  );
  assert.ok(reply.ok, reply.error?.message);
  return reply.data;
}
const action = (action, data = {}) => rpc("launcher.action", { action, data });
const folder = resolve("build/game-settings-fixture-" + Date.now());
await mkdir(resolve(folder, "module"), { recursive: true });
await writeFile(resolve(folder, "One.xex"), "fixture");
await writeFile(resolve(folder, "Two.xex"), "fixture");
await writeFile(
  resolve(folder, "module/module.json"),
  JSON.stringify({
    id: "test.settings.desktop",
    name: "Settings fixture",
    settings: [
      {
        id: "difficulty",
        type: "choice",
        label: "Difficulty",
        description: "Fixture game-specific option.",
        default: "normal",
        options: ["normal", "hard"],
      },
    ],
  }),
);
try {
  await page.getByRole("navigation", { name: "Main navigation" }).waitFor();
  await action("modules.import", { path: resolve(folder, "module") });
  let state = await action("library.import", {
    paths: [resolve(folder, "One.xex"), resolve(folder, "Two.xex")],
    mode: "keep",
  });
  const first = state.games.find(
      (g) => g.contentPath === resolve(folder, "One.xex"),
    ),
    second = state.games.find(
      (g) => g.contentPath === resolve(folder, "Two.xex"),
    );
  assert.ok(first && second);
  for (const game of [first, second])
    await action("library.update", {
      id: game.id,
      moduleId: "test.settings.desktop",
    });
  await action("settings.set", { key: "audio/masterVolume", value: 0.7 });
  await action("settings.set", { key: "library/viewMode", value: "Focused" });
  await action("settings.set", {
    key: "library/lastSelectedGameId",
    value: first.id,
  });
  await page.reload();
  await page
    .getByRole("navigation", { name: "Main navigation" })
    .getByRole("button", { name: "Library", exact: true })
    .click();
  await page
    .getByRole("button", { name: "Game settings", exact: true })
    .click();
  let dialog = page.getByRole("dialog", { name: "Settings · One" });
  await dialog.waitFor();
  assert.equal(await dialog.getByRole("tab").count(), 4);
  await dialog.getByRole("tab", { name: "Audio", exact: true }).click();
  assert.equal(
    await dialog
      .getByRole("spinbutton", { name: "Master volume", exact: true })
      .inputValue(),
    "0.7",
  );
  await dialog
    .getByRole("checkbox", { name: "Customize Master volume", exact: true })
    .check();
  await dialog
    .getByRole("spinbutton", { name: "Master volume", exact: true })
    .fill("0.25");
  await dialog.getByRole("tab", { name: "Game options", exact: true }).click();
  await dialog
    .getByRole("checkbox", { name: "Customize Difficulty", exact: true })
    .check();
  await dialog
    .getByRole("combobox", { name: "Difficulty", exact: true })
    .selectOption({ label: "hard" });
  await dialog
    .getByRole("button", { name: "Close dialog", exact: true })
    .click();
  await dialog.waitFor(); // Rejected dirty-close confirmation keeps the draft.
  await dialog
    .getByRole("button", { name: "Save game settings", exact: true })
    .click();
  await dialog.waitFor({ state: "hidden" });
  state = await rpc("application.snapshot");
  assert.equal(
    state.games.find((g) => g.id === first.id).runtimeOverrides[
      "audio/masterVolume"
    ],
    0.25,
  );
  assert.equal(
    state.games.find((g) => g.id === first.id).moduleOverrides[
      "test.settings.desktop"
    ].difficulty,
    "hard",
  );
  assert.equal(
    state.games.find((g) => g.id === second.id).runtimeOverrides,
    undefined,
  );
  assert.equal(state.modules[0].settings.difficulty, "normal");
  await page.reload();
  await page
    .getByRole("button", { name: "Game settings", exact: true })
    .click();
  dialog = page.getByRole("dialog", { name: "Settings · One" });
  await dialog.getByRole("tab", { name: "Audio", exact: true }).click();
  assert.equal(
    await dialog
      .getByRole("spinbutton", { name: "Master volume", exact: true })
      .inputValue(),
    "0.25",
  );
  await dialog
    .getByRole("button", { name: "Use defaults for this game", exact: true })
    .click();
  assert.equal(
    await dialog
      .getByRole("spinbutton", { name: "Master volume", exact: true })
      .inputValue(),
    "0.7",
  );
  await dialog
    .getByRole("button", { name: "Save game settings", exact: true })
    .click();
  await dialog.waitFor({ state: "hidden" });
  await page.getByRole("button", { name: "Game actions", exact: true }).click();
  await page
    .getByRole("menuitem", { name: "Game settings", exact: true })
    .click();
  dialog = page.getByRole("dialog", { name: "Settings · One" });
  await page.setViewportSize({ width: 640, height: 850 });
  await dialog.getByRole("tab", { name: "Controls", exact: true }).click();
  assert.ok(
    await dialog.evaluate((e) => e.scrollWidth <= e.clientWidth + 1),
    "640px dialog must fit",
  );
  await mkdir("build/game-settings-evidence", { recursive: true });
  await page.screenshot({
    path: "build/game-settings-evidence/settings-640.png",
  });
  discard = true;
  await dialog
    .getByRole("button", { name: "Close dialog", exact: true })
    .click();
  console.log(
    "CEF per-game tabs, inherited values, sparse overrides, module isolation, dirty guard, save/reload/reset, action-menu access and 640px layout passed.",
  );
} finally {
  if (!process.env.CEF_KEEP_OPEN) await page.close();
  await browser.close();
}

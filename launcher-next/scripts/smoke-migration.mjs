import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { mkdir, writeFile, readFile, copyFile } from "node:fs/promises";
import { resolve } from "node:path";
import { pathToFileURL, fileURLToPath } from "node:url";
process.chdir(fileURLToPath(new URL("../", import.meta.url)));
const browser = await chromium.connectOverCDP(
  process.env.CEF_DEBUG_URL || "http://127.0.0.1:9223",
);
const page = browser
  .contexts()
  .flatMap((c) => c.pages())
  .find((page) => page.url() === "xenon://app/");
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
        }),
      ),
    { command, payload },
  );
  assert.ok(reply.ok, reply.error?.message);
  return reply.data;
}
const action = (action, data = {}) => rpc("launcher.action", { action, data });
const folder = resolve(`build/legacy-ui-${Date.now()}`);
await mkdir(resolve(folder, "Profiles"), { recursive: true });
await mkdir(resolve(folder, "Games/fixture"), { recursive: true });
const avatar = resolve(folder, "Profiles/avatar.png");
await copyFile(
  resolve("public/theme-art/backgrounds/theme-carbon-nebula.png"),
  avatar,
);
const profiles = [
  {
    profileId: "old-primary",
    profileName: "Migrated fixture pilot",
    description: "Copied legacy data, for testing only",
    active: true,
    avatarPath: pathToFileURL(avatar).href,
    avatarZoom: 1.5,
    isolatedSettings: true,
    runtimeOverrides: { "audio/masterVolume": 0.3 },
    startupPage: "Profiles",
    region: "PAL",
  },
];
const settings = { "ui/theme": "carbon", "accessibility/reduceMotion": "true" };
const games = [
  {
    gameId: "legacy-game",
    title: "Legacy fixture game",
    contentPath: resolve(folder, "Games/fixture"),
    favorite: true,
    totalPlayTimeMs: 123456,
  },
];
await writeFile(
  resolve(folder, "Profiles/profiles.json"),
  JSON.stringify(profiles),
);
await writeFile(resolve(folder, "settings.json"), JSON.stringify(settings));
await writeFile(resolve(folder, "library.json"), JSON.stringify(games));
await action("migration.inspect", { path: folder });
for (let attempt = 0; attempt < 200; attempt++) {
  const state = await rpc("application.snapshot");
  if (state.migration?.state === "review") break;
  await new Promise((resolve) => setTimeout(resolve, 50));
}
await page
  .getByRole("navigation", { name: "Main navigation" })
  .getByRole("button", { name: "Settings", exact: true })
  .click();
const panel = page.getByRole("region", {
  name: "Previous launcher data",
  exact: true,
});
await panel
  .getByRole("heading", { name: "Import review", exact: true })
  .waitFor();
assert.ok((await panel.innerText()).includes("1 profiles"));
await panel.getByText("Profiles (1)", { exact: true }).click();
await panel.getByText("Migrated fixture pilot", { exact: true }).waitFor();
await page.screenshot({ path: "build/migration-review.png" });
await panel
  .getByRole("button", { name: "Import reviewed data", exact: true })
  .click();
await panel
  .getByRole("heading", { name: "Previous data imported", exact: true })
  .waitFor();
let state = await rpc("application.snapshot");
assert.ok(
  state.games.find((game) => game.title === "Legacy fixture game")?.favorite,
);
assert.equal(
  state.profiles.find((p) => p.id === state.activeProfileId)?.name,
  "Migrated fixture pilot",
);
assert.equal(
  JSON.parse(await readFile(state.migration.backupPath, "utf8")).profiles
    .length,
  1,
);
assert.deepEqual(
  JSON.parse(await readFile(resolve(folder, "Profiles/profiles.json"), "utf8")),
  profiles,
);
await page
  .getByRole("navigation", { name: "Main navigation" })
  .getByRole("button", { name: "Profiles", exact: true })
  .click();
await page
  .getByRole("heading", { name: "Migrated fixture pilot", exact: true })
  .waitFor();
await page.waitForFunction(() =>
  [...document.querySelectorAll(".avatar img")].some(
    (image) => image.complete && image.naturalWidth > 0,
  ),
);
await page.reload();
await page
  .getByRole("heading", { name: "Migrated fixture pilot", exact: true })
  .waitFor();
state = await rpc("application.snapshot");
assert.equal(state.settings["ui/theme"], "carbon");
assert.equal(state.migration.state, "completed");
console.log(
  "Packaged legacy-data review/apply, original preservation, backup, active profile, copied avatar, appearance and reload persistence passed.",
);
if (!process.env.CEF_KEEP_OPEN) await page.close();
await browser.close();

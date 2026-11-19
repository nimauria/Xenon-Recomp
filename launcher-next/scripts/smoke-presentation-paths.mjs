import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { mkdir, readFile, writeFile, access, copyFile } from "node:fs/promises";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { fixtureModule } from "./create-network-fixtures.mjs";
process.chdir(fileURLToPath(new URL("../", import.meta.url)));
const folder = resolve(process.argv[2] ?? "build/presentation-paths-network");
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
  throw Error("Presentation/path fixture timed out");
}
const root = resolve(`build/presentation-ui-${Date.now()}`),
  moduleRoot = resolve(root, "Module"),
  game = resolve(root, "MyGame.iso");
await mkdir(moduleRoot, { recursive: true });
await writeFile(game, "game fixture");
await writeFile(
  resolve(moduleRoot, "module.json"),
  JSON.stringify({
    id: fixtureModule,
    name: "Presentation fixture",
    launcher: {
      description: "Installed description",
      gameTitle: "Installed title",
    },
  }),
);
let entry = JSON.parse(await readFile(resolve(folder, "entry.json"), "utf8"));
entry.launcher = {
  ...entry.launcher,
  gameTitle: "Catalogue title one",
  description: "Catalogue description one",
  heroArt: entry.launcher.tileArt,
};
entry.dlc = [{ id: "catalogue-one", name: "Catalogue add-on one" }];
await writeFile(resolve(folder, "entry.json"), JSON.stringify(entry));
await action("modules.import", { path: moduleRoot });
await action("library.import", { paths: [game] });
let state = await rpc("application.snapshot");
const gameId = state.games.find((g) => g.contentPath === game).id;
await action("library.update", {
  id: gameId,
  moduleId: fixtureModule,
  title: "My custom title",
});
await action("catalog.refresh");
state = await until((s) => s.jobs.at(-1)?.state === "completed");
assert.equal(state.games.find((g) => g.id === gameId).title, "My custom title");
await nav("Library");
await page
  .getByRole("heading", { name: "My custom title", exact: true })
  .waitFor();
await page.getByText("Catalogue description one", { exact: true }).waitFor();
await page.waitForFunction(
  () =>
    document.querySelectorAll('img[src*="catalog-tile/"]').length > 0 &&
    Array.from(document.querySelectorAll('img[src*="catalog-tile/"]')).every(
      (i) => i.complete && i.naturalWidth > 0,
    ),
);
await page.getByText("Catalogue add-on one", { exact: true }).waitFor();
entry.launcher.gameTitle = "Catalogue title two";
entry.launcher.description = "Catalogue description two";
entry.dlc = [{ id: "catalogue-two", name: "Catalogue add-on two" }];
await writeFile(resolve(folder, "entry.json"), JSON.stringify(entry));
await page.getByRole("button", { name: "Properties", exact: true }).click();
const properties = page.getByRole("dialog", {
  name: "Game properties",
  exact: true,
});
await properties
  .getByRole("textbox", { name: "Title", exact: true })
  .fill("Unsaved custom title");
assert.equal(
  await properties
    .getByRole("button", { name: "Refresh module metadata", exact: true })
    .isDisabled(),
  true,
);
await properties
  .getByRole("button", { name: "Close dialog", exact: true })
  .click();
await page.getByRole("button", { name: "Properties", exact: true }).click();
await properties
  .getByRole("button", { name: "Refresh module metadata", exact: true })
  .click();
state = await until(
  (s) =>
    s.jobs.at(-1)?.state === "completed" &&
    s.games.find((g) => g.id === gameId).title === "Catalogue title two",
);
await page
  .getByRole("heading", { name: "Catalogue title two", exact: true })
  .waitFor();
await page.getByText("Catalogue description two", { exact: true }).waitFor();
await page.getByText("Catalogue add-on two", { exact: true }).waitFor();
await nav("Settings");
await page
  .getByRole("navigation", { name: "Settings categories" })
  .getByRole("button", { name: "Paths", exact: true })
  .click();
const paths = page.getByRole("region", {
  name: "Storage locations",
  exact: true,
});
await paths
  .getByRole("article", { name: "Games destination", exact: true })
  .waitFor();
const customGames = resolve(root, "Future games");
await mkdir(customGames, { recursive: true });
await action("settings.set", { key: "paths/games", value: customGames });
const games = paths.getByRole("article", {
  name: "Games destination",
  exact: true,
});
await games.getByText(customGames, { exact: true }).waitFor();
await games
  .getByRole("button", { name: "Use default games folder", exact: true })
  .click();
state = await until((s) => !s.settings["paths/games"]);
assert.equal(
  state.locations.destinations.find((p) => p.id === "games").effectivePath,
  resolve(state.storagePath, "games"),
);
await paths
  .getByRole("article", { name: "Profiles and settings location", exact: true })
  .getByText(resolve(state.storagePath, "state.json"), { exact: true })
  .waitFor();
assert.equal(
  await paths
    .getByRole("button", { name: "Choose cache folder", exact: true })
    .count(),
  1,
);
await page.screenshot({ path: "build/presentation-paths-storage.png" });
const defaultCaptures = resolve(state.storagePath, "captures");
await mkdir(defaultCaptures, { recursive: true });
await copyFile(
  resolve(folder, "cover.png"),
  resolve(defaultCaptures, "Default.png"),
);
await action("settings.set", { key: "paths/screenshots", value: "" });
await action("captures.refresh");
await until(
  (s) =>
    s.captureScan.state === "ready" &&
    s.captures.some((c) => c.name === "Default.png"),
);
await nav("Captures");
await page.waitForFunction(() =>
  Array.from(document.querySelectorAll('img[src*="captures/"]')).some(
    (i) => i.complete && i.naturalWidth > 0,
  ),
);
const customCaptures = resolve(root, "Custom captures");
await mkdir(customCaptures, { recursive: true });
await copyFile(
  resolve(folder, "cover.png"),
  resolve(customCaptures, "Custom.png"),
);
await action("settings.set", {
  key: "paths/screenshots",
  value: customCaptures,
});
await until(
  (s) => s.captures.length === 1 && s.captures[0].name === "Custom.png",
);
await nav("Settings");
await page
  .getByRole("navigation", { name: "Settings categories" })
  .getByRole("button", { name: "Paths", exact: true })
  .click();
await paths
  .getByRole("button", { name: "Use default screenshots folder", exact: true })
  .click();
await until(
  (s) => s.captures.length === 1 && s.captures[0].name === "Default.png",
);
await page
  .getByRole("navigation", { name: "Settings categories" })
  .getByRole("button", { name: "About", exact: true })
  .click();
state = await rpc("application.snapshot");
await page
  .getByText(
    `Version ${state.application.version} / ${state.application.platform} (${state.application.architecture})`,
    { exact: true },
  )
  .waitFor();
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
assert.equal(config.screenshotsPath, defaultCaptures);
assert.equal(config.gameRoot, resolve(state.storagePath, "games"));
await action("session.stop");
await until((s) => s.session.state === "stopped");
await page.reload();
state = await rpc("application.snapshot");
assert.equal(
  state.games.find((g) => g.id === gameId).title,
  "Catalogue title two",
);
assert.equal(state.captureFolderPath, defaultCaptures);
await access(game);
console.log(
  "Desktop catalogue title/description/artwork/DLC refresh, unsaved-edit guard, path defaults/reset and preserved source, real profile store, capture scan/image reset, native build information, launch linkage and reload passed. Fixture source selection used native RPC.",
);
if (!process.env.CEF_KEEP_OPEN) await page.close();
await browser.close();

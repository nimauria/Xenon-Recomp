import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { mkdir, readFile } from "node:fs/promises";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
process.chdir(fileURLToPath(new URL("../", import.meta.url)));
const browser = await chromium.connectOverCDP(
  process.env.CEF_DEBUG_URL || "http://127.0.0.1:9223",
);
const page = browser
  .contexts()
  .flatMap((context) => context.pages())
  .find((page) => page.url() === "xenon://app/");
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
const navigate = (name) =>
  page
    .getByRole("navigation", { name: "Main navigation" })
    .getByRole("button", { name, exact: true })
    .click();
page.on("dialog", (dialog) => dialog.accept());
await page.reload();
await navigate("Profiles");
await page.getByRole("button", { name: "Create profile", exact: true }).click();
await page.getByLabel("Name", { exact: true }).fill("Isolated fixture pilot");
await page
  .getByLabel("Description", { exact: true })
  .fill("Profile parity acceptance");
await page.getByRole("combobox", { name: /^Region/ }).selectOption("PAL");
await page.getByLabel("Isolated runtime settings", { exact: true }).check();
await page
  .getByRole("combobox", { name: /^Startup page/ })
  .selectOption("Support");
await page.getByLabel("Renderer", { exact: true }).selectOption("Vulkan");
await page.getByLabel("Master volume", { exact: true }).fill("0.35");
await page.getByLabel("Controller deadzone", { exact: true }).fill("0.2");
await page.getByRole("button", { name: "Save profile", exact: true }).click();
await page
  .getByRole("heading", { name: "Isolated fixture pilot", exact: true })
  .waitFor();
let state = await rpc("application.snapshot");
const profile = state.profiles.find(
  (row) => row.name === "Isolated fixture pilot",
);
assert.ok(profile);
assert.equal(Object.keys(profile.runtimeOverrides).length, 9);
assert.equal(profile.runtimeOverrides["audio/masterVolume"], 0.35);
await rpc("profiles.activate", { id: profile.id });
await action("settings.set", { key: "audio/masterVolume", value: 0.9 });
await page.reload();
await page.getByRole("heading", { name: "Support", exact: true }).waitFor();
state = await rpc("application.snapshot");
assert.equal(
  state.profiles.find((row) => row.id === profile.id).effectiveRuntimeSettings[
    "audio/masterVolume"
  ],
  0.35,
);
await navigate("Profiles");
const card = page.getByRole("article").filter({
  has: page.getByRole("heading", {
    name: "Isolated fixture pilot",
    exact: true,
  }),
});
await card.getByRole("button", { name: "Duplicate", exact: true }).click();
await page
  .getByRole("heading", { name: "Isolated fixture pilot copy", exact: true })
  .waitFor();
const source = resolve("build/profile-smoke-content");
await mkdir(source, { recursive: true });
await action("library.import", { paths: [source] });
state = await rpc("application.snapshot");
assert.ok(
  state.capabilities.runtime,
  "Start with --fixture-host to verify runtime configuration linkage",
);
const game = state.games.find((game) => game.contentPath === source);
await action("session.launch", { gameId: game.id });
for (let attempt = 0; attempt < 100; attempt++) {
  state = await rpc("application.snapshot");
  if (state.session?.state === "running") break;
  await new Promise((resolve) => setTimeout(resolve, 50));
}
assert.equal(state.session.state, "running");
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
assert.equal(config.renderer, "Vulkan");
assert.equal(config.audioMasterVolume, 0.35);
assert.equal(config.inputDeadzone, 0.2);
assert.equal(config.region, "PAL");
assert.ok(
  config.savePath && config.screenshotsPath && config.gameRoot,
  "Blank overrides inherit launcher paths",
);
await action("session.stop");
for (let attempt = 0; attempt < 100; attempt++) {
  state = await rpc("application.snapshot");
  if (state.session?.state === "stopped") break;
  await new Promise((resolve) => setTimeout(resolve, 50));
}
assert.equal(state.session.state, "stopped");
console.log(
  "Packaged profile editor, nine overrides, startup/reload, duplication and fixture runtime configuration passed.",
);
await page.close();
await browser.close();

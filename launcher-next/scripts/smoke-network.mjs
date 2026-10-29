import { chromium } from "playwright-core";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { readFile, writeFile } from "node:fs/promises";
import assert from "node:assert/strict";
import {
  fixtureModule,
  publishFixture,
  registry,
} from "./create-network-fixtures.mjs";
process.chdir(fileURLToPath(new URL("../", import.meta.url)));
const folder = resolve(process.argv[2] ?? "build/network-fixtures");
const browser = await chromium.connectOverCDP(
  process.env.CEF_DEBUG_URL || "http://127.0.0.1:9223",
  { timeout: 10000 },
);
const page = browser
  .contexts()
  .flatMap((context) => context.pages())
  .find((page) => page.url() === "xenon://app/");
assert.ok(page);
page.setDefaultTimeout(15000);
const errors = [];
page.on("pageerror", (error) => errors.push(error.message));
async function snapshot() {
  return page.evaluate(
    () =>
      new Promise((resolve) =>
        window.cefQuery({
          request: JSON.stringify({
            version: 1,
            id: crypto.randomUUID(),
            command: "application.snapshot",
            payload: {},
          }),
          onSuccess: (result) => resolve(JSON.parse(result).data),
          onFailure: () => resolve(null),
        }),
      ),
  );
}
async function until(predicate) {
  for (let attempt = 0; attempt < 300; attempt++) {
    const state = await snapshot();
    if (predicate(state)) return state;
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  throw new Error("Native online state did not reach the expected condition");
}
const navigate = (name) =>
  page
    .getByRole("navigation", { name: "Main navigation" })
    .getByRole("button", { name, exact: true })
    .click();
await until((state) =>
  state?.catalog.entries.some((entry) => entry.id === fixtureModule),
);
assert.equal((await snapshot()).capabilities.networkFixture, true);
await navigate("Modules");
await page
  .getByRole("button", { name: "Module catalogue", exact: true })
  .click();
const entry = page.getByRole("article", {
  name: "Fixture catalogue module",
  exact: true,
});
await entry.waitFor();
await page.waitForFunction(() => {
  const image = document.querySelector(".catalog-cover img");
  return image?.complete && image.naturalWidth > 0;
});
await entry
  .getByRole("button", { name: "Check for updates", exact: true })
  .click();
await entry
  .getByRole("button", { name: "Download release", exact: true })
  .click();
await entry
  .getByRole("button", { name: "Install verified package", exact: true })
  .waitFor();
await page.screenshot({ path: "build/catalogue-verified.png", fullPage: true });
page.once("dialog", (dialog) => dialog.accept());
await entry
  .getByRole("button", { name: "Install verified package", exact: true })
  .click();
await until((state) =>
  state.modules.some(
    (module) => module.id === fixtureModule && module.version === "1.0.0",
  ),
);
await publishFixture(folder, "2.0.0", false);
await entry
  .getByRole("button", { name: "Check for updates", exact: true })
  .click();
await until(
  (state) => state.moduleUpdates[fixtureModule]?.release?.version === "2.0.0",
);
await entry
  .getByRole("button", { name: "Download release", exact: true })
  .click();
await entry
  .getByText("The downloaded package failed SHA-256 verification", {
    exact: true,
  })
  .waitFor();
assert.equal(
  (await snapshot()).modules.find((module) => module.id === fixtureModule)
    .version,
  "1.0.0",
);
await publishFixture(folder, "2.0.0");
await entry
  .getByRole("button", { name: "Check for updates", exact: true })
  .click();
await until(
  (state) =>
    state.moduleUpdates[fixtureModule]?.release?.asset?.sha256 !==
      "0".repeat(64) && !state.moduleUpdates[fixtureModule]?.busy,
);
await entry
  .getByRole("button", { name: "Download release", exact: true })
  .click();
await entry
  .getByRole("button", { name: "Install verified package", exact: true })
  .waitFor();
page.once("dialog", (dialog) => dialog.accept());
await entry
  .getByRole("button", { name: "Install verified package", exact: true })
  .click();
await until(
  (state) =>
    state.modules.find((module) => module.id === fixtureModule)?.version ===
    "2.0.0",
);
await page.getByRole("button", { name: "Close dialog", exact: true }).click();
const module = page
  .locator(".cards > article")
  .filter({
    has: page.getByRole("heading", {
      name: "Fixture catalogue module",
      exact: true,
    }),
  });
page.once("dialog", (dialog) => dialog.accept());
await module.getByRole("button", { name: "Roll back", exact: true }).click();
await until(
  (state) =>
    state.modules.find((module) => module.id === fixtureModule)?.version ===
    "1.0.0",
);
await module
  .getByRole("button", { name: "Update history", exact: true })
  .click();
await page
  .getByRole("heading", { name: "Update history", exact: true })
  .waitFor();
assert.ok(
  (await snapshot()).updateHistory.some((row) => row.action === "install") &&
    (await snapshot()).updateHistory.some((row) => row.action === "rollback"),
);
await page.getByRole("button", { name: "Close dialog", exact: true }).click();
const mapping = JSON.parse(
  await readFile(resolve(folder, "responses.json"), "utf8"),
);
mapping[registry + "catalog.json"].status = 503;
await writeFile(resolve(folder, "responses.json"), JSON.stringify(mapping));
await page
  .getByRole("button", { name: "Module catalogue", exact: true })
  .click();
await page
  .getByRole("button", { name: "Refresh catalogue", exact: true })
  .click();
await page
  .getByText("Showing saved registry information.", { exact: true })
  .waitFor();
await entry.waitFor();
await page.screenshot({ path: "build/catalogue-offline.png", fullPage: true });
assert.deepEqual(errors, []);
console.log(
  "Packaged catalogue, artwork, release selection, verified download/install, bad digest, update, rollback, history and offline cache passed.",
);
await page.close();
await browser.close();

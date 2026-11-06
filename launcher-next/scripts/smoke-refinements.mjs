import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { mkdir } from "node:fs/promises";

const browser = await chromium.connectOverCDP(
  process.env.CEF_DEBUG_URL || "http://127.0.0.1:9223",
);
const page = browser
  .contexts()
  .flatMap((c) => c.pages())
  .find((p) => p.url() === "xenon://app/");
assert.ok(page);
page.setDefaultTimeout(15000);
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
          onFailure: (_c, message) => reject(Error(message)),
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
try {
  const initial = await rpc("application.snapshot");
  assert.equal(initial.developmentDemo, true);
  await nav("Home");
  const attention = page.getByRole("region", {
    name: "Needs attention",
    exact: true,
  });
  await attention.getByRole("button", { name: /Packages ready/ }).click();
  assert.equal(
    await page
      .getByRole("combobox", { name: "Show", exact: true })
      .inputValue(),
    "ready",
  );
  await page
    .getByRole("region", { name: "Ready to install", exact: true })
    .getByRole("article", { name: "Skyline Squadron module", exact: true })
    .waitFor();
  const search = page.getByRole("searchbox", {
    name: "Search current page",
    exact: true,
  });
  await search.fill("no matching package");
  await nav("Home");
  await attention.getByRole("button", { name: /Failed operations/ }).click();
  assert.equal(
    await page
      .getByRole("combobox", { name: "Show", exact: true })
      .inputValue(),
    "failed",
  );
  assert.equal(await search.inputValue(), "");
  await page.getByText("Earlier demo update check", { exact: true }).waitFor();
  await nav("Discover");
  const catalogue = page.getByRole("region", {
    name: "Module catalogue",
    exact: true,
  });
  assert.equal(await catalogue.getByRole("article").count(), 4);
  const filter = catalogue.getByRole("combobox", {
    name: "Show modules",
    exact: true,
  });
  await filter.selectOption("installed");
  assert.equal(await catalogue.getByRole("article").count(), 3);
  await filter.selectOption("ready");
  assert.equal(await catalogue.getByRole("article").count(), 1);
  await filter.selectOption("platform");
  assert.equal(await catalogue.getByRole("article").count(), 4);
  await search.fill("orbital frontier");
  assert.equal(await catalogue.getByRole("article").count(), 1);
  await catalogue
    .getByRole("button", { name: "Reset filters", exact: true })
    .click();
  assert.equal(await search.inputValue(), "");
  assert.equal(await filter.inputValue(), "all");
  assert.equal(await catalogue.getByRole("article").count(), 4);
  await search.fill("no matching game");
  assert.equal(await catalogue.getByRole("article").count(), 0);
  await catalogue
    .getByRole("button", { name: "Reset filters", exact: true })
    .click();
  await page.keyboard.press("Control+k");
  const commands = page.getByRole("combobox", {
    name: "Search commands",
    exact: true,
  });
  await commands.waitFor();
  for (let index = 0; index < 15; ++index) await commands.press("ArrowDown");
  assert.ok(
    await page.evaluate(() => {
      const input = document.querySelector(
        'input[aria-controls="command-results"]',
      );
      const row = document.getElementById(
        input?.getAttribute("aria-activedescendant"),
      );
      if (!row) return false;
      const r = row.getBoundingClientRect();
      return r.top >= 0 && r.bottom <= innerHeight;
    }),
    "Keyboard-selected command remains on screen",
  );
  await commands.fill("Browse module catalogue");
  await commands.press("Enter");
  await page
    .getByRole("heading", { name: "Discover", level: 1, exact: true })
    .waitFor();
  await action("settings.set", { key: "general/restoreLastPage", value: true });
  await action("navigation.remember", { page: "Discover" });
  await page.reload();
  await page
    .getByRole("heading", { name: "Discover", level: 1, exact: true })
    .waitFor();
  for (const [language, title] of [
    ["fr", "Découvrir"],
    ["de", "Entdecken"],
    ["es", "Descubrir"],
    ["en", "Discover"],
  ]) {
    await action("settings.set", { key: "general/language", value: language });
    await page
      .getByRole("heading", { name: title, level: 1, exact: true })
      .waitFor();
  }
  await page.setViewportSize({ width: 640, height: 800 });
  assert.ok(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= innerWidth + 1,
    ),
    "Discover fits a narrow window",
  );
  await mkdir("build/refinement-evidence", { recursive: true });
  await page.screenshot({ path: "build/refinement-evidence/discover.png" });
  await nav("Home");
  await page
    .getByRole("region", { name: "Needs attention", exact: true })
    .scrollIntoViewIfNeeded();
  await page.screenshot({ path: "build/refinement-evidence/home.png" });
  await page.setViewportSize({ width: 1280, height: 900 });
  await nav("Discover");
  await page.waitForFunction(() => window.scrollY === 0);
  await page.screenshot({
    path: "build/refinement-evidence/discover-desktop.png",
  });
  const final = await rpc("application.snapshot");
  assert.deepEqual(final.games, initial.games);
  assert.deepEqual(final.modules, initial.modules);
  console.log(
    "Discover search/filter/reset, cached package visibility, Home attention routing, remembered native page, four languages, command scrolling and narrow layout passed.",
  );
} finally {
  await page.close();
  await browser.close();
}

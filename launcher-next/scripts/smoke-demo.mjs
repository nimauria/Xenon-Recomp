import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { mkdir } from "node:fs/promises";
const browser = await chromium.connectOverCDP(
  process.env.CEF_DEBUG_URL || "http://127.0.0.1:9227",
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
const nav = (name) =>
  page
    .getByRole("navigation", { name: "Main navigation" })
    .getByRole("button", { name, exact: true })
    .click();
async function until(condition) {
  for (let i = 0; i < 300; ++i) {
    const state = await rpc("application.snapshot");
    if (condition(state)) return state;
    await page.waitForTimeout(50);
  }
  throw Error("Demo operation timed out");
}
try {
  let state = await rpc("application.snapshot");
  assert.equal(
    state.developmentDemo,
    true,
    "Only run against explicit demo data",
  );
  assert.equal(state.games.length, 4);
  assert.equal(state.profiles.length, 2);
  assert.equal(state.capabilities.networkFixture, true);
  assert.equal(state.session.fixture, true);
  await page
    .getByText(
      "Developer demo · Fictional sample data and test services. Your normal library is separate.",
      { exact: true },
    )
    .waitFor();
  await nav("Updates");
  const orbital = page.getByRole("article", {
    name: "Orbital Frontier module",
    exact: true,
  });
  assert.equal(
    await orbital
      .getByRole("button", { name: "Install verified package", exact: true })
      .isEnabled(),
    false,
  );
  await orbital
    .getByRole("button", { name: "Download release", exact: true })
    .click();
  await until((s) => s.moduleUpdates["demo.orbital"].canInstall);
  await nav("Downloads");
  const packages = page.getByRole("region", {
    name: "Ready to install",
    exact: true,
  });
  await packages
    .getByRole("article", { name: "Orbital Frontier module", exact: true })
    .getByRole("button", { name: "Install verified package", exact: true })
    .click();
  await until((s) =>
    s.modules.some((m) => m.id === "demo.orbital" && m.version === "2.0.0"),
  );
  await action("jobs.clearFinished");
  await page.reload();
  await nav("Downloads");
  await page
    .getByRole("region", { name: "Ready to install", exact: true })
    .getByRole("article", { name: "Skyline Squadron module", exact: true })
    .waitFor();
  await nav("Modules");
  await page
    .getByRole("button", { name: "Module catalogue", exact: true })
    .click();
  const skyline = page.getByRole("article", {
    name: "Skyline Squadron module",
    exact: true,
  });
  await skyline
    .getByRole("button", { name: "Install verified package", exact: true })
    .click();
  await until((s) =>
    s.modules.some((m) => m.id === "demo.skyline" && m.version === "1.1.0"),
  );
  await page
    .getByRole("dialog")
    .getByRole("button", { name: "Close dialog", exact: true })
    .click();
  await nav("Library");
  await page
    .getByRole("button", { name: "Game settings", exact: true })
    .click();
  await page
    .getByRole("dialog")
    .getByRole("tab", { name: "Game options", exact: true })
    .click();
  await page
    .getByRole("dialog")
    .getByRole("checkbox", { name: "Customize Difficulty", exact: true })
    .waitFor();
  await page
    .getByRole("dialog")
    .getByRole("button", { name: "Close dialog", exact: true })
    .click();
  await nav("Captures");
  await until((s) => s.captures?.length === 4);
  await nav("Profiles");
  await page
    .getByRole("heading", { name: "Demo Couch", exact: true })
    .waitFor();
  await nav("Library");
  await page.waitForFunction(() =>
    [
      ...document.querySelectorAll(".game-cover img,.game-details-hero img"),
    ].every((image) => image.complete && image.naturalWidth > 0),
  );
  await mkdir("build/demo-evidence", { recursive: true });
  await page.screenshot({ path: "build/demo-evidence/library.png" });
  state = await rpc("application.snapshot");
  assert.equal(state.developmentDemo, true);
  assert.equal(state.games.length, 4);
  console.log(
    "Populated native demo, fixture-only providers, visible Updates/Downloads/catalogue Install actions, verified local installs, cleared-history package recovery, lazy per-game settings, profiles/captures and decoded artwork passed.",
  );
} finally {
  if (!process.env.CEF_KEEP_OPEN) await page.close();
  await browser.close();
}

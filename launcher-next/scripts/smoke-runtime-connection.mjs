import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { mkdir } from "node:fs/promises";
const host = process.argv[2];
assert.ok(host, "Supply an existing unlaunched test executable");
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
          onFailure: (_c, m) => reject(Error(m)),
        }),
      ),
    { command, payload },
  );
  assert.equal(reply.ok, true, reply.error?.message);
  return reply.data;
}
async function runtimeSettings() {
  await page
    .getByRole("navigation", { name: "Main navigation" })
    .getByRole("button", { name: "Settings", exact: true })
    .click();
  await page
    .getByRole("navigation", { name: "Settings categories" })
    .getByRole("button", { name: "Runtime", exact: true })
    .click();
}
async function until(condition) {
  for (let i = 0; i < 100; ++i) {
    const state = await rpc("application.snapshot");
    if (condition(state)) return state;
    await page.waitForTimeout(50);
  }
  throw Error("Runtime selection did not settle");
}
try {
  await runtimeSettings();
  const region = page.getByRole("region", {
    name: "Xenon runtime",
    exact: true,
  });
  const input = region.getByRole("textbox", {
    name: "Runtime host executable",
    exact: true,
  });
  await input.fill(host + "-missing");
  await region
    .getByRole("button", { name: "Save runtime host", exact: true })
    .click();
  await page
    .getByRole("alert")
    .filter({ hasText: "Choose an existing runtime host executable" })
    .waitFor();
  assert.equal(
    (await rpc("application.snapshot")).runtimeConnection.configuredPath,
    "",
  );
  await input.fill(host);
  await region
    .getByRole("button", { name: "Save runtime host", exact: true })
    .click();
  const selected = await until(
    (s) =>
      s.runtimeConnection?.available && s.runtimeConnection?.source === "saved",
  );
  assert.equal(selected.session.fixture, false);
  assert.equal(
    selected.games.length,
    0,
    "Selecting a host must not seed games or execute it",
  );
  await page.reload();
  await runtimeSettings();
  assert.equal(await input.inputValue(), host);
  for (const [locale, label] of [
    ["fr", "Runtime Xenon"],
    ["de", "Xenon-Laufzeit"],
    ["es", "Runtime de Xenon"],
    ["en", "Xenon runtime"],
  ]) {
    await rpc("launcher.action", {
      action: "settings.set",
      data: { key: "general/language", value: locale },
    });
    await page.getByRole("region", { name: label, exact: true }).waitFor();
  }
  await page.setViewportSize({ width: 640, height: 800 });
  assert.ok(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= innerWidth + 1,
    ),
    "Runtime settings fit a narrow window",
  );
  await mkdir("build/runtime-evidence", { recursive: true });
  await region.scrollIntoViewIfNeeded();
  await page.screenshot({ path: "build/runtime-evidence/settings.png" });
  await region
    .getByRole("button", { name: "Use automatic discovery", exact: true })
    .click();
  await until((s) => s.runtimeConnection?.configuredPath === "");
  console.log(
    "Runtime selection UI, atomic invalid-path rejection, reload persistence, no automatic execution, four languages and narrow layout passed.",
  );
} finally {
  await page.close();
  await browser.close();
}

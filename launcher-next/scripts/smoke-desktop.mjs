import { chromium } from "playwright-core";
import { mkdir, writeFile, copyFile } from "node:fs/promises";
import { resolve } from "node:path";
import assert from "node:assert/strict";
import { fileURLToPath } from "node:url";
process.chdir(fileURLToPath(new URL("../", import.meta.url)));

const browser = await chromium.connectOverCDP(
  process.env.CEF_DEBUG_URL || "http://127.0.0.1:9223",
  { timeout: 10000 },
);
const page = browser
  .contexts()
  .flatMap((c) => c.pages())
  .find((p) => p.url() === "xenon://app/");
assert.ok(page, "Packaged launcher page must exist");
assert.deepEqual(
  (
    await page
      .getByRole("navigation", { name: "Main navigation" })
      .getByRole("button")
      .allTextContents()
  ).slice(0, 2),
  ["Home", "Library"],
  "Home must precede Library",
);
const errors = [];
page.on("pageerror", (error) => errors.push(error.message));
async function rpc(command, payload = {}) {
  return page.evaluate(
    ({ command, payload }) =>
      new Promise((resolve, reject) => {
        const id = crypto.randomUUID();
        window.cefQuery({
          request: JSON.stringify({ version: 1, id, command, payload }),
          onSuccess: (value) => resolve(JSON.parse(value)),
          onFailure: (_code, message) => reject(new Error(message)),
        });
      }),
    { command, payload },
  );
}
const stamp = Date.now();
const fixture = resolve("build/smoke-fixtures");
await mkdir(fixture, { recursive: true });
await mkdir(resolve(fixture, "local-game"), { recursive: true });
await mkdir(resolve(fixture, "module"), { recursive: true });
await mkdir(resolve(fixture, "module/art"), { recursive: true });
await copyFile(
  resolve("public/theme-art/backgrounds/theme-carbon-nebula.png"),
  resolve(fixture, "module/art/cover.PNG"),
);
await writeFile(
  resolve(fixture, "module/module.json"),
  JSON.stringify({
    id: `smoke.module.${stamp}`,
    name: "Fictional smoke module",
    version: "0.0.1",
    launcher: {
      tileArt: "art/cover.PNG",
      heroArt: "art/cover.PNG",
      tileArtFocalX: 0.25,
    },
    settings: [
      { id: "example", label: "Example setting", type: "bool", default: false },
    ],
  }),
);

await page
  .getByRole("navigation", { name: "Main navigation" })
  .getByRole("button", { name: "Profiles", exact: true })
  .click();
await page.getByRole("button", { name: "Create profile", exact: true }).click();
await page.getByLabel("Name", { exact: true }).fill(`Smoke pilot ${stamp}`);
await page
  .getByLabel("Description", { exact: true })
  .fill("Native persistence verification");
await page.getByRole("button", { name: "Save profile", exact: true }).click();
await page
  .getByRole("heading", { name: `Smoke pilot ${stamp}`, exact: true })
  .waitFor();
let snapshot = (await rpc("application.snapshot")).data;
const profile = snapshot.profiles.find(
  (p) => p.name === `Smoke pilot ${stamp}`,
);
assert.equal(profile.description, "Native persistence verification");
assert.equal((await rpc("profiles.activate", { id: profile.id })).ok, true);
assert.equal(
  (await rpc("profiles.remove", { id: profile.id })).ok,
  false,
  "Active profile must be protected",
);
assert.equal(
  (
    await rpc("launcher.action", {
      action: "library.import",
      data: { paths: [resolve(fixture, "local-game")] },
    })
  ).ok,
  true,
);
assert.equal(
  (
    await rpc("launcher.action", {
      action: "modules.import",
      data: { path: resolve(fixture, "module") },
    })
  ).ok,
  true,
);
assert.equal(
  (
    await rpc("launcher.action", {
      action: "settings.set",
      data: { key: "accessibility/textScale", value: 1.25 },
    })
  ).ok,
  true,
);
snapshot = (await rpc("application.snapshot")).data;
const game = snapshot.games.find(
  (g) => g.contentPath === resolve(fixture, "local-game"),
);
assert.ok(game);
assert.equal(
  (
    await rpc("launcher.action", {
      action: "library.update",
      data: { id: game.id, moduleId: `smoke.module.${stamp}` },
    })
  ).ok,
  true,
);
assert.equal(
  (
    await rpc("launcher.action", {
      action: "settings.set",
      data: { key: "accessibility/textScale", value: 8 },
    })
  ).ok,
  false,
);
assert.equal(
  (await rpc("session.launch", { gameId: "missing" })).ok,
  false,
  "Runtime must remain unavailable",
);
await page.getByRole("button", { name: "Refresh", exact: true }).click();
for (const name of [
  "Library",
  "Home",
  "Modules",
  "Profiles",
  "Captures",
  "Downloads",
  "Settings",
  "Support",
]) {
  await page
    .getByRole("navigation", { name: "Main navigation" })
    .getByRole("button", { name, exact: true })
    .click();
  await page.getByRole("heading", { name, exact: true, level: 1 }).waitFor();
  assert.equal(
    await page.locator("[role=alert]").count(),
    0,
    `${name} must not fail`,
  );
}
await page
  .getByRole("navigation", { name: "Main navigation" })
  .getByRole("button", { name: "Library", exact: true })
  .click();
await page.screenshot({ path: "build/desktop-smoke.png", fullPage: true });
await page
  .locator("label")
  .filter({ hasText: "View" })
  .locator("select")
  .selectOption({ label: "Carousel" });
await page.getByRole("region", { name: "Game carousel" }).waitFor();
await page.waitForFunction(() => {
  const images = [...document.querySelectorAll(".cover-image img")];
  return (
    images.length >= 2 &&
    images.every((image) => image.complete && image.naturalWidth > 0)
  );
});
assert.ok(
  (await page.locator(".carousel-shelf button").count()) > 0,
  "Carousel must use registered games",
);
await page.screenshot({ path: "build/library-carousel.png", fullPage: true });
await page
  .getByRole("button", { name: "Module settings", exact: true })
  .click();
await page
  .getByRole("heading", {
    name: "Fictional smoke module settings",
    exact: true,
  })
  .waitFor();
await page.getByRole("button", { name: "Close dialog", exact: true }).click();
await page
  .getByRole("button", { name: "Collapse navigation", exact: true })
  .click();
await page.waitForFunction(
  () => document.querySelector(".app")?.dataset.sidebar === "compact",
);
await page
  .getByRole("button", { name: "Expand navigation", exact: true })
  .click();
await page.waitForFunction(
  () => document.querySelector(".app")?.dataset.sidebar === "expanded",
);
const viewport = await page.evaluate(() => ({
  width: innerWidth,
  height: innerHeight,
}));
await rpc("launcher.action", {
  action: "settings.set",
  data: { key: "general/sidebarMode", value: "Auto" },
});
await page.getByRole("button", { name: "Refresh", exact: true }).click();
await page.setViewportSize({ width: 640, height: 800 });
await page.waitForFunction(
  () => document.querySelector(".app")?.dataset.sidebar === "compact",
);
assert.equal(
  await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth),
  true,
  "Native minimum-width layout must not overflow horizontally",
);
await page.screenshot({ path: "build/library-narrow.png", fullPage: true });
await page.setViewportSize(viewport);
await page
  .locator("label")
  .filter({ hasText: "View" })
  .locator("select")
  .selectOption({ label: "Focused" });
await page.reload();
await page
  .getByRole("heading", { name: "Library", exact: true, level: 1 })
  .waitFor();
snapshot = (await rpc("application.snapshot")).data;
assert.equal(
  snapshot.activeProfileId,
  profile.id,
  "State must survive renderer restart",
);
assert.ok(
  snapshot.games.some((g) => g.contentPath === resolve(fixture, "local-game")),
);
for (const theme of ["xenon-dark", "carbon", "industrial", "light"]) {
  assert.equal(
    (
      await rpc("launcher.action", {
        action: "settings.set",
        data: { key: "ui/theme", value: theme },
      })
    ).ok,
    true,
  );
  await page.getByRole("button", { name: "Refresh", exact: true }).click();
  await page.waitForFunction(
    (id) => document.documentElement.dataset.themeId === id,
    theme,
  );
  if (theme !== "light")
    await page.waitForFunction(() => {
      const image = document.querySelector(".backdrop-raster");
      return image?.complete && image.naturalWidth > 0;
    });
  await page.screenshot({ path: `build/theme-${theme}.png`, fullPage: true });
}
assert.equal(
  (
    await rpc("launcher.action", {
      action: "settings.set",
      data: { key: "ui/theme", value: "carbon" },
    })
  ).ok,
  true,
);
assert.equal(
  (
    await rpc("launcher.action", {
      action: "settings.set",
      data: { key: "ui/accent", value: "violet" },
    })
  ).ok,
  true,
);
assert.equal(
  (
    await rpc("launcher.action", {
      action: "settings.set",
      data: { key: "ui/corners", value: "square" },
    })
  ).ok,
  true,
);
await page.getByRole("button", { name: "Refresh", exact: true }).click();
await page.waitForFunction(
  () =>
    getComputedStyle(document.documentElement).getPropertyValue("--accent") ===
    "#A78BFA",
);
assert.equal(
  await page
    .locator("header button")
    .first()
    .evaluate((e) => getComputedStyle(e).borderRadius),
  "4px",
);
assert.equal(
  (
    await rpc("launcher.action", {
      action: "settings.set",
      data: { key: "accessibility/highContrast", value: true },
    })
  ).ok,
  true,
);
await page.getByRole("button", { name: "Refresh", exact: true }).click();
await page.waitForFunction(
  () => document.querySelector(".theme-backdrop") === null,
);
await rpc("launcher.action", {
  action: "settings.set",
  data: { key: "accessibility/highContrast", value: false },
});
await page
  .getByRole("navigation", { name: "Main navigation" })
  .getByRole("button", { name: "Settings", exact: true })
  .click();
await page
  .getByRole("navigation", { name: "Settings categories" })
  .getByRole("button", { name: "Appearance", exact: true })
  .click();
await page.getByRole("button", { name: "Refresh", exact: true }).click();
assert.equal(await page.locator(".theme-options button").count(), 5);
assert.equal(await page.locator(".background-options button").count(), 2);
await page.screenshot({
  path: "build/theme-appearance-settings.png",
  fullPage: true,
});
assert.deepEqual(errors, [], "No renderer exceptions");
const captureFolder = resolve(fixture, "captures");
await mkdir(captureFolder, { recursive: true });
const image = resolve(captureFolder, "fixture.png");
await writeFile(
  image,
  Buffer.from(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/l9sAAAAASUVORK5CYII=",
    "base64",
  ),
);
assert.equal(
  (
    await rpc("launcher.action", {
      action: "settings.set",
      data: { key: "paths/screenshots", value: captureFolder },
    })
  ).ok,
  true,
);
assert.equal(
  (
    await rpc("launcher.action", {
      action: "profiles.avatarImport",
      data: { id: profile.id, path: image },
    })
  ).ok,
  true,
);
assert.equal(
  (
    await rpc("launcher.action", {
      action: "profiles.update",
      data: {
        id: profile.id,
        name: profile.name,
        avatarFocalX: 0.3,
        avatarFocalY: 0.7,
        avatarZoom: 1.5,
      },
    })
  ).ok,
  true,
);
await page.getByRole("button", { name: "Refresh", exact: true }).click();
await page
  .getByRole("navigation", { name: "Main navigation" })
  .getByRole("button", { name: "Profiles", exact: true })
  .click();
await page.waitForFunction(() =>
  [...document.querySelectorAll(".avatar img")].some(
    (image) => image.complete && image.naturalWidth > 0,
  ),
);
await page
  .getByRole("navigation", { name: "Main navigation" })
  .getByRole("button", { name: "Captures", exact: true })
  .click();
await page.getByRole("heading", { name: "fixture.png", exact: true }).waitFor();
await page.getByRole("button", { name: "View", exact: true }).click();
await page.waitForFunction(() => {
  const image = document.querySelector(".capture-image");
  return image?.complete && image.naturalWidth > 0;
});
await page.getByRole("button", { name: "Close dialog", exact: true }).click();
assert.equal(
  (await rpc("launcher.action", { action: "support.bundle", data: {} })).ok,
  true,
);
// A failed module load must show a useful error, never a blank white window.
await page.evaluate(() => {
  const script = document.createElement("script");
  script.src = "/assets/intentionally-missing-smoke.js";
  document.body.append(script);
});
await page
  .getByRole("heading", {
    name: "Xenon could not open the interface",
    exact: true,
  })
  .waitFor({ timeout: 20000 });
await page.screenshot({ path: "build/startup-failure.png" });

await page.reload();
await page
  .getByRole("heading", { name: "Library", exact: true, level: 1 })
  .waitFor();
console.log(
  "CEF UI, native IPC, persistence, navigation, all legacy palettes/backgrounds, appearance controls, contrast and forced startup-failure recovery passed.",
);
await page.close();
await browser.close();

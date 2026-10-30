import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { mkdir, writeFile } from "node:fs/promises";
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
let discard = true;
page.on("dialog", (dialog) => (discard ? dialog.accept() : dialog.dismiss()));
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
          onFailure: (_code, message) => reject(Error(message)),
        }),
      ),
    { command, payload },
  );
  assert.ok(reply.ok, reply.error?.message);
  return reply.data;
}
const action = (action, data = {}) => rpc("launcher.action", { action, data });
async function nav(name){await page.getByRole("navigation",{name:"Main navigation"}).getByRole("button",{name,exact:true}).click();await page.evaluate(()=>new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve))));}
const folder = resolve("build/controller-fixture-" + Date.now());
await mkdir(folder, { recursive: true });
const paths = [];
for (let i = 0; i < 180; i++) {
  const path = resolve(
    folder,
    "Input fixture " + String(i).padStart(3, "0") + ".xex",
  );
  await writeFile(path, "fixture");
  paths.push(path);
}
try {
  await page.getByRole("navigation", { name: "Main navigation" }).waitFor();
  await action("library.import", { paths, mode: "keep" });
  await action("settings.set", { key: "library/viewMode", value: "Grid" });
  await page.reload();
  await nav("Library");
  await page.evaluate(()=>new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve))));
  await page.evaluate(() => {
    window.controllerFixture = {
      foreground: true,
      pad: {
        id: "Sony DualSense 054c",
        index: 0,
        connected: true,
        mapping: "standard",
        axes: [0, 0],
        buttons: Array.from({ length: 16 }, () => ({
          pressed: false,
          value: 0,
          touched: false,
        })),
      },
    };
    Object.defineProperty(navigator, "getGamepads", {
      configurable: true,
      value: () =>
        window.controllerFixture.pad ? [window.controllerFixture.pad] : [],
    });
    Object.defineProperty(document, "hasFocus", {
      configurable: true,
      value: () => window.controllerFixture.foreground,
    });
  });
  async function press(button, hold = 70) {
    await page.evaluate((button) => {
      window.controllerFixture.pad.buttons[button].pressed = true;
    }, button);
    await page.waitForTimeout(hold);
    await page.evaluate((button) => {
      window.controllerFixture.pad.buttons[button].pressed = false;
    }, button);
    await page.waitForTimeout(50);
  }
  const index = () =>
    page.evaluate(() =>
      Number(document.activeElement?.dataset.gameIndex ?? -1),
    );
  await page.locator('.virtual-games [data-game-index="0"]').focus();
  await press(15);
  assert.equal(
    await index(),
    1,
    "Right moves horizontally through the collection",
  );
  await press(13);
  assert.ok((await index()) > 1, "Down moves to the next visual row");
  await press(13, 1300);
  assert.ok(
    (await index()) > 20,
    "Held direction continues into virtualized rows",
  );
  assert.ok(
    (await page.locator(".virtual-games").evaluate((node) => node.scrollTop)) >
      0,
  );
  await press(3);
  const menu = page.getByRole("menu", { name: "Game actions" });
  await menu.waitFor();
  assert.ok(
    await menu
      .getByRole("menuitem", { name: "Play", exact: true })
      .isDisabled(),
  );
  await press(0, 600);
  await menu.waitFor({ state: "hidden" });
  const snapshot = await rpc("application.snapshot");
  assert.equal(
    snapshot.games.filter((game) => game.favorite).length,
    1,
    "Held activate executes a favorite action only once",
  );
  await press(3);
  await menu.waitFor();
  await press(1);
  await menu.waitFor({ state: "hidden" });
  assert.ok(
    (await index()) >= 0,
    "Closing a context menu restores the game focus",
  );
  await press(3);
  await menu.getByRole("menuitem", { name: "Properties", exact: true }).click();
  await page
    .getByRole("dialog")
    .getByLabel("Title", { exact: true })
    .fill("Unsaved controller title");
  discard = false;
  await press(1);
  assert.equal(
    await page.getByRole("dialog").count(),
    1,
    "Back must honor the dirty form guard",
  );
  const before = await rpc("application.snapshot");
  await press(2);
  assert.deepEqual(
    (await rpc("application.snapshot")).games,
    before.games,
    "A modal owns secondary actions",
  );
  discard = true;
  await press(1);
  await page.getByRole("dialog").waitFor({ state: "hidden" });
  await nav("Settings");
  await nav("Profiles");
  await press(1);
  await page
    .getByRole("heading", { name: "Settings", level: 1, exact: true })
    .waitFor();
  await page.keyboard.press("F1");
  await page.getByText(/Cross: activate/).waitFor();
  await press(1);
  await press(9);
  await page.getByRole("menu", { name: "Quick actions" }).waitFor();
  await press(1);
  await nav("Profiles");
  await page.getByRole("button", { name: /Actions for Local player/ }).click();
  assert.ok(
    await page
      .getByRole("menu")
      .getByRole("menuitem", { name: "Remove", exact: true })
      .isDisabled(),
  );
  await page.keyboard.press("Escape");
  await page.setViewportSize({ width: 640, height: 800 });
  await page.getByRole("button", { name: /Actions for Local player/ }).focus();
  await page.keyboard.press("Shift+F10");
  await page.getByRole("menu").waitFor();
  assert.ok(
    await page.getByRole("menu").evaluate((node) => {
      const r = node.getBoundingClientRect();
      return (
        r.left >= 0 && r.right <= innerWidth + 1 && r.bottom <= innerHeight + 1
      );
    }),
  );
  await press(1);
  await nav("Library");
  await page.locator("[data-quick-actions]").focus();
  const focused = await page.evaluate(() => document.activeElement.outerHTML);
  await page.evaluate(() => {
    window.controllerFixture.foreground = false;
  });
  await press(15);
  assert.equal(
    await page.evaluate(() => document.activeElement.outerHTML),
    focused,
    "Background input must be ignored",
  );
  await page.evaluate(() => {
    window.controllerFixture.foreground = true;
    window.controllerFixture.pad = null;
  });
  await page.waitForTimeout(80);
  await mkdir("build/controller-evidence", { recursive: true });
  await page.screenshot({ path: "build/controller-evidence/collection.png" });
  console.log(
    "CEF spatial/virtualized controller navigation, held-button edges, modal ownership/dirty guard, device glyphs, history back, accessible menus, disabled policies, focus restoration, reconnect/disconnect and background-input suppression passed.",
  );
} finally {
  if (!process.env.CEF_KEEP_OPEN) await page.close();
  await browser.close();
}

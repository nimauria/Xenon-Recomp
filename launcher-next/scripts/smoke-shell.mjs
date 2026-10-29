import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { mkdir, writeFile } from "node:fs/promises";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
process.chdir(fileURLToPath(new URL("../", import.meta.url)));
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
  const result = await page.evaluate(
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
  assert.ok(result.ok, result.error?.message);
  return result.data;
}
const action = (action, data = {}) => rpc("launcher.action", { action, data });
const nav = (name) =>
  page
    .getByRole("navigation", { name: "Main navigation" })
    .getByRole("button", { name, exact: true })
    .click();
await page.reload();
await page.getByRole("navigation",{name:"Main navigation"}).waitFor();
let before = await rpc("application.snapshot");
if(!before.profiles.some(p=>p.name==="Palette fixture pilot")) await rpc("profiles.create", { name: "Palette fixture pilot" });
const folder = resolve(`build/shell-fixture-${Date.now()}`);
await mkdir(folder, { recursive: true });
await writeFile(
  resolve(folder, "module.json"),
  JSON.stringify({
    id: "shell.fixture",
    name: "Palette fixture module",
    version: "1.0.0",
  }),
);
if(!before.modules.some(m=>m.id==="shell.fixture")) await action("modules.import", { path: folder });
async function palette(query) {
  await page.keyboard.press("Control+k");
  const field = page.getByRole("combobox", {
    name: "Search commands",
    exact: true,
  });
  await field.fill(query);
  return field;
}
await palette("master volume");
await page.getByRole("listbox",{name:"Commands",exact:true}).getByRole("option").first().click();
await page.getByRole("heading", { name: "Audio", exact: true }).waitFor();
await palette("create profile");
await page.keyboard.press("Enter");
await page
  .getByRole("dialog")
  .getByRole("heading", { name: "Create profile", exact: true })
  .waitFor();
await page.getByRole("button", { name: "Close dialog", exact: true }).click();
await palette("palette fixture module");
await page.keyboard.press("Enter");
await page.waitForFunction(
  () => document.activeElement?.id === "module-shell.fixture",
);
await nav("Library");
await page
  .getByRole("searchbox", { name: "Search current page", exact: true })
  .fill("keep library search");
await palette("palette fixture pilot");
await page.keyboard.press("Escape");
assert.equal(
  await page
    .getByRole("searchbox", { name: "Search current page", exact: true })
    .inputValue(),
  "keep library search",
  "Command queries must not alter the page's search",
);
await action("notifications.clear");
await action("notifications.push", {
  title: "Shell test",
  message: "A repeated shell event",
});
await action("notifications.push", {
  title: "Shell test",
  message: "A repeated shell event",
});
await page.getByRole("button", { name: /^Notifications,/ }).click();
await page
  .getByRole("dialog")
  .getByRole("heading", { name: "Shell test", exact: true })
  .waitFor();
assert.ok(
  (await page.getByRole("dialog").innerText()).includes("2 occurrences"),
);
await page.getByRole("button", { name: "Mark all read", exact: true }).click();
assert.ok(
  (await rpc("application.snapshot")).notifications.every((n) => n.read),
);
await page
  .getByRole("button", { name: "View activity", exact: true })
  .filter({ visible: true })
  .first()
  .click();
await page
  .getByRole("heading", { name: "Downloads", exact: true, level: 1 })
  .waitFor();
await page.getByRole("button", { name: /^Notifications,/ }).click();
await page
  .getByRole("button", { name: "Dismiss notification", exact: true })
  .click();
assert.equal((await rpc("application.snapshot")).notifications.length, 0);
await page.getByRole("button", { name: "Close dialog", exact: true }).click();
await action("settings.set", { key: "general/restoreLastPage", value: true });
await nav("Captures");
await new Promise((resolve) => setTimeout(resolve, 400));
await page.reload();
await page
  .getByRole("heading", { name: "Captures", exact: true, level: 1 })
  .waitFor();
console.log(
  "Packaged ranked command routing, settings/profile/module targets, independent queries, notifications/dedup/actions/read-all/dismiss and remembered-page reload passed.",
);
if (!process.env.CEF_KEEP_OPEN) await page.close();
await browser.close();

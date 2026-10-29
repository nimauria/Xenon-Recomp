import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { mkdir, writeFile, unlink, utimes } from "node:fs/promises";
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
const folder = resolve(`build/capture-ui-${Date.now()}`);
await mkdir(folder, { recursive: true });
const png = Buffer.from(
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Y9Zl1sAAAAASUVORK5CYII=",
  "base64",
);
for (let i = 0; i < 1200; i++) {
  const file = resolve(folder, `capture-${String(i).padStart(4, "0")}.png`);
  await writeFile(file, png);
  await utimes(file, 1000000000, 1000000000);
}
await writeFile(resolve(folder, "newest.png"), png);
await action("settings.set", { key: "paths/screenshots", value: folder });
await page
  .getByRole("navigation", { name: "Main navigation" })
  .getByRole("button", { name: "Captures", exact: true })
  .click();
await page.getByRole("heading", { name: "newest.png", exact: true }).waitFor();
await page.getByRole("status").filter({ hasText: "1201 captures" }).waitFor();
assert.ok(
  (await page.locator(".capture-card").count()) < 30,
  "Large capture folders must window the card DOM",
);
await page.waitForFunction(() =>
  [...document.querySelectorAll(".capture-thumbnail img")].some(
    (image) => image.complete && image.naturalWidth > 0,
  ),
);
await page
  .getByRole("button", { name: "View newest.png", exact: true })
  .click();
await page.getByRole("button", { name: "Zoom in", exact: true }).click();
await page.getByRole("status").filter({ hasText: "125%" }).waitFor();
await page.getByRole("button", { name: "Next capture", exact: true }).click();
await page
  .getByRole("dialog")
  .getByRole("heading", { name: "capture-0000.png", exact: true })
  .waitFor();
const viewer = page.getByRole("region", {
  name: "Capture viewer",
  exact: true,
});
await viewer.focus();
await page.keyboard.press("+");
await page.getByRole("status").filter({ hasText: "125%" }).waitFor();
await page.keyboard.press("0");
await page.getByRole("status").filter({ hasText: "100%" }).waitFor();
await page.getByRole("button", { name: "Close dialog", exact: true }).click();
await page
  .getByRole("combobox", { name: "Capture order", exact: true })
  .selectOption("Oldest first");
await page
  .getByRole("heading", { name: "capture-0000.png", exact: true })
  .waitFor();
await writeFile(resolve(folder, "added.png"), png);
await page
  .getByRole("button", { name: "Rescan captures", exact: true })
  .click();
await page.getByRole("status").filter({ hasText: "1202 captures" }).waitFor();
await unlink(resolve(folder, "added.png"));
await page
  .getByRole("button", { name: "Rescan captures", exact: true })
  .click();
await page.getByRole("status").filter({ hasText: "1201 captures" }).waitFor();
await page
  .getByRole("article", { name: "capture-0000.png", exact: true })
  .getByRole("button", { name: "Delete", exact: true })
  .click();
await page.getByRole("status").filter({ hasText: "1200 captures" }).waitFor();
await page.screenshot({ path: "build/captures-verified.png" });
console.log(
  "Packaged capture scan/refresh, 1,201-file DOM windowing, thumbnail decoding, ordering, viewer zoom/keys/navigation and confirmed deletion passed.",
);
if (!process.env.CEF_KEEP_OPEN) await page.close();
await browser.close();

import { chromium } from "playwright-core";
import assert from "node:assert/strict";
const endpoint = process.env.CEF_DEBUG_URL || "http://127.0.0.1:9223";
async function rpc(page, action, data = {}) {
  return page.evaluate(
    ({ action, data }) =>
      new Promise((resolve, reject) => {
        const id = crypto.randomUUID();
        window.cefQuery({
          request: JSON.stringify({
            version: 1,
            id,
            command: "launcher.action",
            payload: { action, data },
          }),
          onSuccess: (value) => resolve(JSON.parse(value)),
          onFailure: (_code, message) => reject(new Error(message)),
        });
      }),
    { action, data },
  );
}
let browser = await chromium.connectOverCDP(endpoint, { timeout: 10000 });
let page = browser
  .contexts()[0]
  .pages()
  .find((p) => p.url() === "xenon://app/");
assert.ok(page);
assert.equal(
  (await rpc(page, "application.restart", { safeMode: true })).ok,
  true,
);
await browser.close();
let safe = false;
for (let attempt = 0; attempt < 40 && !safe; attempt++) {
  await new Promise((resolve) => setTimeout(resolve, 250));
  try {
    browser = await chromium.connectOverCDP(endpoint, { timeout: 750 });
    page = browser
      .contexts()[0]
      .pages()
      .find((p) => p.url() === "xenon://app/");
    if (!page) {
      await browser.close();
      continue;
    }
    await page
      .getByRole("heading", { name: "Library", exact: true, level: 1 })
      .waitFor({ timeout: 750 });
    safe = await page.evaluate(
      () =>
        document.documentElement.dataset.themeId === "xenon-dark" &&
        !document.querySelector(".theme-backdrop"),
    );
    if (!safe) await browser.close();
  } catch {}
}
assert.equal(
  safe,
  true,
  "Restarted safe-mode application must render with decorations disabled",
);
const snapshot = await page.evaluate(
  () =>
    new Promise((resolve) =>
      window.cefQuery({
        request: JSON.stringify({
          version: 1,
          id: "restart-check",
          command: "application.snapshot",
          payload: {},
        }),
        onSuccess: (value) => resolve(JSON.parse(value).data),
        onFailure: () => resolve(null),
      }),
    ),
);
assert.equal(snapshot.recovery.safeMode, true);
assert.equal(
  snapshot.recovery.previousUncleanExit,
  false,
  "A normal restart must not appear as a crash",
);
await page.close();
await browser.close();
console.log(
  "Native safe-mode restart, clean shutdown marker and preserved state passed.",
);

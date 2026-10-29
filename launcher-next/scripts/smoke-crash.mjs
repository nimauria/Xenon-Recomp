import { chromium } from "playwright-core";
import assert from "node:assert/strict";
const endpoint = process.env.CEF_DEBUG_URL || "http://127.0.0.1:9223";
let browser = await chromium.connectOverCDP(endpoint);
let page = browser
  .contexts()
  .flatMap((c) => c.pages())
  .find((p) => p.url() === "xenon://app/");
assert.ok(page);
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
          onFailure: (code, message) =>
            resolve({ ok: false, error: { code, message } }),
        }),
      ),
    { command, payload },
  );
  assert.ok(result.ok, result.error?.message);
  return result.data;
}
await rpc("profiles.create", {
  name: "Crash survivor",
  description: "Native data must survive a renderer failure",
});
async function crash() {
  const session = await browser.contexts()[0].newCDPSession(page);
  await Promise.race([
    session.send("Page.crash").catch(() => {}),
    new Promise((resolve) => setTimeout(resolve, 2000)),
  ]);
  await browser.close();
}
async function reconnect(predicate) {
  for (let attempt = 0; attempt < 60; attempt++) {
    try {
      browser = await chromium.connectOverCDP(endpoint, { timeout: 500 });
      page = browser.contexts().flatMap((c) => c.pages())[0];
      if (page && (await predicate(page))) return;
      await browser.close();
    } catch {}
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error("Renderer recovery did not reach the expected page");
}
await crash();
await reconnect(
  async (p) =>
    p.url() === "xenon://app/" &&
    (await p.getByRole("navigation", { name: "Main navigation" }).isVisible()),
);
assert.ok(
  (await rpc("application.snapshot")).profiles.some(
    (p) => p.name === "Crash survivor",
  ),
);
await crash();
await reconnect(
  async (p) =>
    p.url().startsWith("data:") &&
    (await p
      .getByRole("link", { name: "Restart in safe mode", exact: true })
      .isVisible()),
);
const rejected = await page.evaluate(
  () =>
    new Promise((resolve) =>
      window.cefQuery({
        request: JSON.stringify({
          version: 1,
          id: "untrusted-recovery",
          command: "application.snapshot",
          payload: {},
        }),
        onSuccess: () => resolve(false),
        onFailure: (code) => resolve(code === 403),
      }),
    ),
);
assert.equal(
  rejected,
  true,
  "Recovery document must not acquire privileged native IPC",
);
await page.screenshot({ path: "build/renderer-recovery.png" });
await page
  .getByRole("link", { name: "Restart in safe mode", exact: true })
  .click();
await browser.close();
await reconnect(
  async (p) =>
    p.url() === "xenon://app/" &&
    (await p.getByRole("navigation", { name: "Main navigation" }).isVisible()),
);
const state = await rpc("application.snapshot");
assert.equal(state.recovery.safeMode, true);
assert.equal(state.recovery.previousUncleanExit, false);
assert.ok(state.profiles.some((p) => p.name === "Crash survivor"));
console.log(
  "Renderer crash reload, repeat-crash fallback, origin isolation, safe restart, clean shutdown and preserved native state passed.",
);
if (!process.env.CEF_KEEP_OPEN) await page.close();
await browser.close();

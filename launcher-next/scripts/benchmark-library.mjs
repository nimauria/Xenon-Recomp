// Isolated synthetic library measurements; no installed games or user state are modified.
import { chromium } from "playwright-core";
import { mkdir, writeFile, readFile, realpath } from "node:fs/promises";
import { resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { cpus, totalmem, release } from "node:os";
import assert from "node:assert/strict";
process.chdir(fileURLToPath(new URL("../", import.meta.url)));
const fixture = "xenon-launcher-library-benchmark";
if (process.argv[2] === "prepare") {
  const root = resolve(`build/library-benchmark-${Date.now()}`);
  await mkdir(root, { recursive: true });
  const source = resolve(root, "Synthetic.iso");
  await writeFile(source, "synthetic benchmark source; not a game");
  const locations = [];
  for (const count of [0, 100, 1000, 10000]) {
    const folder = resolve(root, String(count));
    await mkdir(folder, { recursive: true });
    const state = {
      schemaVersion: 1,
      revision: 0,
      mode: "native",
      profiles: [
        {
          id: "local",
          name: "Benchmark profile",
          description: "",
          region: "Auto (Global)",
          offline: true,
          paths: {},
        },
      ],
      activeProfileId: "local",
      preferences: { theme: "dark", reducedMotion: true, startPage: "Library" },
      settings: {
        "updates/automaticChecks": false,
        "accessibility/reduceMotion": true,
        "library/viewMode": "Grid",
      },
      modules: [],
      notifications: [],
      history: [],
      jobs: [],
      games: Array.from({ length: count }, (_, i) => ({
        id: `benchmark-${i}`,
        title: `Benchmark ${String(i).padStart(5, "0")}`,
        module: "No module selected",
        moduleId: "",
        description: "Synthetic benchmark entry",
        favorite: false,
        lastPlayed: null,
        contentPath: source,
        mediaModelVersion: 1,
        activityModelVersion: 1,
        playCount: 0,
        totalPlayTimeMs: 0,
        fixtureActivity: { playCount: 0, totalPlayTimeMs: 0 },
        media: [
          {
            id: `media-${i}`,
            name: "Synthetic disc",
            sourcePath: source,
            sourceType: ".iso",
            mediaId: "",
            discNumber: 0,
            discCount: 0,
            region: "",
            importMode: "kept",
            isPrimary: true,
            identityVerified: false,
          },
        ],
      })),
    };
    const json = JSON.stringify(state);
    assert.ok(Buffer.byteLength(json) < 16 * 1024 * 1024);
    await writeFile(resolve(folder, "state.json"), json);
    await writeFile(
      resolve(folder, "benchmark-fixture.json"),
      JSON.stringify({ fixture, count, stateBytes: Buffer.byteLength(json) }),
    );
    locations.push(folder);
  }
  await writeFile(
    resolve(root, "fixtures.json"),
    JSON.stringify({ fixture, locations }, null, 2),
  );
  console.log(root);
} else if (process.argv[2] === "measure") {
  const folder = await realpath(resolve(process.argv[3]));
  assert.ok(folder.startsWith(resolve("build") + sep));
  const descriptor = JSON.parse(
    await readFile(resolve(folder, "benchmark-fixture.json"), "utf8"),
  );
  assert.equal(descriptor.fixture, fixture);
  let browser;
  for (let i = 0; i < 200; i++) {
    try {
      browser = await chromium.connectOverCDP(
        process.env.CEF_DEBUG_URL || "http://127.0.0.1:9223",
      );
      break;
    } catch (error) {
      if (i === 199) throw error;
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
  }
  const page = browser
    .contexts()
    .flatMap((c) => c.pages())
    .find((p) => p.url() === "xenon://app/");
  assert.ok(page);
  page.setDefaultTimeout(30000);
  const ready = () =>
    page.waitForFunction(
      (count) =>
        document.body.innerText.includes("Desktop launcher") &&
        document
          .querySelector(".toolbar")
          ?.textContent.includes(`${count} games`),
      descriptor.count,
    );
  await ready();
  await page.reload();
  await ready();
  const rendererReloadReadyMs = await page.evaluate(() => performance.now());
  const search = page.getByRole("searchbox", {
    name: "Search current page",
    exact: true,
  });
  await search.waitFor();
  await search.evaluate((node) => (node.dataset.benchmarkSearch = "true"));
  await page
    .getByLabel("Show")
    .evaluate((node) => (node.dataset.benchmarkFilter = "true"));
  async function eventFrame(selector, value, kind = "input") {
    return page.evaluate(
      ({ selector, value, kind }) =>
        new Promise((resolve) => {
          const node = document.querySelector(selector),
            start = performance.now();
          const prototype =
            kind === "change"
              ? HTMLSelectElement.prototype
              : HTMLInputElement.prototype;
          Object.getOwnPropertyDescriptor(prototype, "value").set.call(
            node,
            value,
          );
          node.dispatchEvent(new Event(kind, { bubbles: true }));
          requestAnimationFrame(() =>
            requestAnimationFrame(() => resolve(performance.now() - start)),
          );
        }),
      { selector, value, kind },
    );
  }
  const input = [],
    sort = [];
  for (let i = 0; i < 15; i++)
    input.push(
      await eventFrame(
        'input[data-benchmark-search="true"]',
        i % 2 ? "" : "Benchmark 000",
      ),
    );
  assert.ok(
    (await page.locator(".toolbar").innerText()).includes(
      `${Math.min(descriptor.count, 100)} games`,
    ),
    "Search timing requires the filtered results to have rendered",
  );
  await eventFrame('input[data-benchmark-search="true"]', "");
  if (descriptor.count)
    for (let i = 0; i < 10; i++)
      sort.push(
        await eventFrame(
          'select[data-benchmark-filter="true"]',
          i % 2 ? "All games" : "Favourites",
          "change",
        ),
      );
  // Restore all games before measuring sort explicitly.
  if (descriptor.count)
    await eventFrame(
      'select[data-benchmark-filter="true"]',
      "All games",
      "change",
    );
  const sortSelect = page.getByLabel("Sort");
  await sortSelect.evaluate((node) => (node.dataset.benchmarkSort = "true"));
  const sortFrames = [];
  for (let i = 0; i < 10; i++) {
    sortFrames.push(
      await eventFrame(
        'select[data-benchmark-sort="true"]',
        i % 2 ? "A\u2013Z" : "Z\u2013A",
        "change",
      ),
    );
    if (i === 0 && descriptor.count)
      assert.equal(
        await page
          .locator(".virtual-games button")
          .first()
          .getAttribute("aria-label"),
        `Benchmark ${String(descriptor.count - 1).padStart(5, "0")}`,
        "Sort timing requires the result order to have changed",
      );
  }
  const snapshots = [];
  for (let i = 0; i < 5; i++)
    snapshots.push(
      await page.evaluate(
        () =>
          new Promise((resolve, reject) => {
            const start = performance.now();
            window.cefQuery({
              request: JSON.stringify({
                version: 1,
                id: crypto.randomUUID(),
                command: "application.snapshot",
                payload: {},
              }),
              onSuccess: (raw) => {
                const state = JSON.parse(raw);
                if (!state.ok) reject(Error(state.error.message));
                else resolve(performance.now() - start);
              },
              onFailure: (_code, message) => reject(Error(message)),
            });
          }),
      ),
    );
  const cdp = await page.context().newCDPSession(page);
  await cdp.send("Performance.enable");
  const metrics = await cdp.send("Performance.getMetrics");
  const heap = metrics.metrics.find((m) => m.name === "JSHeapUsedSize")?.value;
  const dom = await page.evaluate(() => ({
    gameButtons: document.querySelectorAll(".virtual-games button").length,
    nodes: document.querySelectorAll("*").length,
  }));
  assert.ok(dom.gameButtons <= 100);
  const percentile = (values) => {
    const sorted = [...values].sort((a, b) => a - b);
    return (
      sorted[
        Math.min(sorted.length - 1, Math.ceil(sorted.length * 0.95) - 1)
      ] ?? null
    );
  };
  const result = {
    ...descriptor,
    measuredAt: new Date().toISOString(),
    environment: {
      cpu: cpus()[0]?.model,
      logicalCpus: cpus().length,
      totalMemoryGiB: Math.round(totalmem() / 1024 ** 3),
      os: release(),
      userAgent: await page.evaluate(() => navigator.userAgent),
    },
    rendererReloadReadyMs,
    inputFrameP95Ms: percentile(input),
    filterFrameP95Ms: percentile(sort),
    sortFrameP95Ms: percentile(sortFrames),
    snapshotP95Ms: percentile(snapshots),
    rendererJsHeapMiB: heap ? heap / 1024 ** 2 : null,
    dom,
    samples: { input, filter: sort, sort: sortFrames, snapshots },
    limits:
      "Automation DOM-event to two animation frames; renderer reload excludes native cold startup. Synthetic shared-source entries, no artwork. JS heap excludes native/CEF process memory. Indicative samples, not a full release performance acceptance.",
  };
  await writeFile(
    resolve(folder, "result.json"),
    JSON.stringify(result, null, 2),
  );
  console.log(
    JSON.stringify({
      count: result.count,
      inputFrameP95Ms: result.inputFrameP95Ms,
      sortFrameP95Ms: result.sortFrameP95Ms,
      snapshotP95Ms: result.snapshotP95Ms,
      gameButtons: dom.gameButtons,
    }),
  );
  await page.close();
  await browser.close();
} else throw Error("Use prepare, or measure <isolated-fixture-folder>");

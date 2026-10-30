import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { readFile, writeFile, mkdir } from "node:fs/promises";
import { resolve, relative, sep } from "node:path";
import { fileURLToPath } from "node:url";
process.chdir(fileURLToPath(new URL("../", import.meta.url)));
const catalogue = JSON.parse(
  await readFile("contracts/localization.json", "utf8"),
);
const originals = new Map(
  Object.entries(catalogue.sources).map(([key, value]) => [
    value.original,
    key,
  ]),
);
const text = (english, language) =>
  catalogue.translations[language][originals.get(english)] ?? english;
const webRoot = resolve(process.argv[2] ?? "build/native/desktop/web");
const within = relative(resolve("build"), webRoot);
assert.ok(
  within &&
    !within.startsWith(".." + sep) &&
    within !== ".." &&
    !resolve(within).startsWith("\\\\"),
);
const external = resolve(webRoot, "localization.json"),
  originalFile = await readFile(external);
let browser;
for (let attempt = 0; attempt < 100; attempt++) {
  try {
    browser = await chromium.connectOverCDP(
      process.env.CEF_DEBUG_URL || "http://127.0.0.1:9223",
    );
    break;
  } catch (error) {
    if (attempt === 99) throw error;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
}
const page = browser
  .contexts()
  .flatMap((context) => context.pages())
  .find((page) => page.url() === "xenon://app/");
assert.ok(page);
page.setDefaultTimeout(15000);
const errors = [];
page.on("pageerror", (error) => errors.push(error.message));
async function rpc(command, payload = {}) {
  const result = await page.evaluate(
    ({ command, payload }) =>
      new Promise((resolve, reject) =>
        window.cefQuery({
          request: JSON.stringify({
            version: 1,
            id: crypto.randomUUID(),
            command,
            payload,
          }),
          onSuccess: (value) => resolve(JSON.parse(value)),
          onFailure: (_code, message) => reject(Error(message)),
        }),
      ),
    { command, payload },
  );
  assert.ok(result.ok, result.error?.message);
  return result.data;
}
const action = (action, data = {}) => rpc("launcher.action", { action, data });
const nav = (english, lang) =>
  page
    .getByRole("navigation", { name: text("Main navigation", lang) })
    .getByRole("button", { name: text(english, lang), exact: true })
    .click();
async function inLanguage(lang) {
  await page.waitForFunction(
    (lang) => document.documentElement.lang === lang,
    lang,
  );
  await page
    .getByRole("navigation", { name: text("Main navigation", lang) })
    .waitFor();
}
async function settings(lang) {
  await nav("Settings", lang);
  await page
    .getByRole("navigation", { name: text("Settings categories", lang) })
    .getByRole("button", { name: text("General", lang), exact: true })
    .click();
}
try {
  await page.getByRole("navigation").first().waitFor();
  await action("settings.set", { key: "general/language", value: "en" });
  await action("settings.set", {
    key: "general/startupPage",
    value: "Library",
  });
  await action("settings.set", {
    key: "general/restoreLastPage",
    value: false,
  });
  await page.reload();
  await inLanguage("en");
  await rpc("profiles.create", {
    name: "Library",
    description: "User text stays in English",
  });
  await action("notifications.push", { title: "Settings", message: "Library" });
  let previous = "en";
  for (const lang of ["fr", "de", "es", "en"]) {
    await settings(previous);
    await page
      .getByLabel(text("Display language", previous))
      .selectOption(lang);
    await inLanguage(lang);
    assert.equal(
      (await rpc("application.snapshot")).settings["general/language"],
      lang,
    );
    await page
      .getByRole("button", {
        name: new RegExp("^" + text("Notifications", lang) + ","),
      })
      .click();
    const inbox = page.getByRole("dialog");
    await inbox
      .getByRole("heading", {
        name: text("Profile created", lang),
        level: 3,
        exact: true,
      })
      .waitFor();
    await inbox
      .getByRole("heading", { name: "Settings", level: 3, exact: true })
      .waitFor();
    await inbox
      .getByRole("button", { name: text("Close dialog", lang), exact: true })
      .click();
    await nav("Library", lang);
    await page
      .getByRole("heading", {
        name: text("Library", lang),
        level: 1,
        exact: true,
      })
      .waitFor();
    await page.getByLabel(text("Show", lang)).selectOption("Favourites");
    assert.equal(
      await page.getByLabel(text("Show", lang)).inputValue(),
      "Favourites",
    );
    await page.getByLabel(text("Show", lang)).selectOption("All games");
    await page.getByLabel(text("Sort", lang)).selectOption("Z–A");
    assert.equal(await page.getByLabel(text("Sort", lang)).inputValue(), "Z–A");
    await nav("Profiles", lang);
    await page
      .getByRole("heading", { name: "Library", level: 2, exact: true })
      .waitFor();
    await page.keyboard.press("Control+k");
    await page
      .getByRole("combobox", {
        name: text("Search commands", lang),
        exact: true,
      })
      .fill(
        catalogue.translations[lang]["command.goto"].replace(
          "{page}",
          text("Library", lang),
        ),
      );
    await page.getByRole("listbox").getByRole("option").first().click();
    await page
      .getByRole("heading", {
        name: text("Library", lang),
        level: 1,
        exact: true,
      })
      .waitFor();
    await page.reload();
    await inLanguage(lang);
    await page
      .getByRole("heading", {
        name: text("Library", lang),
        level: 1,
        exact: true,
      })
      .waitFor();
    previous = lang;
  }
  // Edit only the isolated package's runtime copy, demonstrating that English is file-backed.
  const custom = structuredClone(catalogue),
    key = originals.get("Library");
  custom.translations.en[key] = "Library catalogue override";
  delete custom.translations.fr[key];
  await writeFile(external, JSON.stringify(custom));
  await action("settings.set", { key: "general/language", value: "fr" });
  await page.reload();
  await inLanguage("fr");
  await page
    .getByRole("heading", {
      name: "Library catalogue override",
      level: 1,
      exact: true,
    })
    .waitFor();
  await page
    .getByRole("navigation", { name: text("Main navigation", "fr") })
    .getByRole("button", { name: text("Settings", "fr"), exact: true })
    .waitFor();
  await writeFile(external, "{invalid JSON");
  await page.reload();
  await page
    .getByRole("heading", { name: "Library", level: 1, exact: true })
    .waitFor();
  await writeFile(external, originalFile);
  await page.reload();
  await inLanguage("fr");
  await page.setViewportSize({ width: 640, height: 800 });
  assert.ok(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= innerWidth + 1,
    ),
  );
  await settings("fr");
  await mkdir("build/localization-evidence", { recursive: true });
  await page.screenshot({
    path: "build/localization-evidence/french-settings.png",
    fullPage: true,
  });
  await action("settings.set", { key: "general/language", value: "en" });
  await page.reload();
  await inLanguage("en");
  assert.deepEqual(errors, []);
  console.log(
    "Packaged EN/FR/DE/ES switching, saved locale/reload, translated command search, stable filter/sort IDs, original profile names, editable English, per-key fallback, malformed-catalogue recovery and 640px layout passed.",
  );
} finally {
  await writeFile(external, originalFile);
  if (!process.env.CEF_KEEP_OPEN) await page.close();
  await browser.close();
}

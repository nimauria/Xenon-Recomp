import { beforeEach, afterEach, describe, expect, it, vi } from "vitest";
import { readFileSync, readdirSync } from "node:fs";
import { fileURLToPath } from "node:url";
import parser from "prettier/plugins/typescript";
import builtin from "../contracts/localization.json";
import {
  loadLocalization,
  placeholders,
  resolveLanguage,
  setLanguage,
  t,
  tCount,
  tText,
  validateCatalogue,
} from "./localization";

afterEach(async () => {
  setLanguage("en");
  vi.stubGlobal("fetch", vi.fn().mockRejectedValue(Error("offline")));
  await loadLocalization();
  vi.unstubAllGlobals();
});
const external = (catalogue: unknown) =>
  vi.stubGlobal(
    "fetch",
    vi.fn().mockResolvedValue({
      ok: true,
      text: async () => JSON.stringify(catalogue),
    }),
  );
beforeEach(async () => {
  external(builtin);
  await loadLocalization();
});
describe("launcher localisation", () => {
  it("validates the master and covers all UI keys in the four launch languages", () => {
    expect(validateCatalogue(builtin)).toBe(builtin);
    for (const id of ["en", "fr", "de", "es"] as const)
      for (const [key, value] of Object.entries(builtin.translations.en)) {
        if (key.startsWith("native.")) continue; // Native message conversion is tracked separately.
        const translated = (builtin.translations[id] as Record<string, string>)[
          key
        ];
        expect(translated, `${id}:${key}`).toBeTruthy();
        expect(placeholders(translated)).toBe(placeholders(value));
      }
  });
  it("switches labels and formats counted messages without changing user parameters", () => {
    setLanguage("fr");
    expect(tText("Library")).toBe("Bibliothèque");
    expect(t("command.goto", { page: "MY_CUSTOM_TITLE" })).toBe(
      "Aller à MY_CUSTOM_TITLE",
    );
    expect(tCount("library.sourcesSelected", 1)).toBe("1 source sélectionnée.");
    expect(tCount("library.sourcesSelected", 2)).toBe(
      "2 sources sélectionnées.",
    );
    setLanguage("de");
    expect(tText("Library")).toBe("Bibliothek");
    setLanguage("es");
    expect(tText("Library")).toBe("Biblioteca");
    expect(tText("A user name never registered as a label")).toBe(
      "A user name never registered as a label",
    );
  });
  it("resolves regional system languages and safely handles unavailable IDs", () => {
    expect(
      resolveLanguage("auto", validateCatalogue(builtin).languages, "fr-CA"),
    ).toBe("fr");
    expect(resolveLanguage("DE-de")).toBe("de");
    expect(resolveLanguage("es_MX")).toBe("es");
    expect(resolveLanguage("ja-JP")).toBe("en");
    expect(resolveLanguage("constructor")).toBe("en");
  });
  it("loads editable English copy and falls back per key when a translation is absent", async () => {
    const c = structuredClone(builtin);
    delete (c.translations.fr as Record<string, string>)["language.choose"];
    c.translations.en["language.choose"] = "Choose your display language";
    external(c);
    await loadLocalization();
    setLanguage("fr");
    expect(t("language.choose")).toBe("Choose your display language");
    expect(tText("Library")).toBe("Bibliothèque");
    expect(t("missing.key")).toBe("[missing.key]");
  });
  it("keeps bundled text available for malformed or missing external catalogues", async () => {
    for (const bad of [
      null,
      { ...builtin, languages: [] },
      { ...builtin, translations: { en: null } },
      { ...builtin, sources: { broken: null } },
    ]) {
      expect(() => validateCatalogue(bad)).toThrow();
      external(bad);
      await loadLocalization();
      setLanguage("de");
      expect(tText("Library")).toBe("Library");
    }
    const c = structuredClone(builtin);
    c.translations.fr["command.goto"] = "Aller à {wrong}";
    expect(() => validateCatalogue(c)).toThrow(/placeholders/);
    vi.stubGlobal("fetch", vi.fn().mockRejectedValue(Error("missing")));
    await loadLocalization();
    setLanguage("en");
    expect(tText("Library")).toBe("Library");
  });
  it("keeps every referenced message in the master and technical JSX attributes unlocalized", () => {
    const folder = fileURLToPath(new URL("./ui/", import.meta.url));
    for (const file of readdirSync(folder).filter((file) =>
      file.endsWith(".tsx"),
    )) {
      const source = readFileSync(`${folder}/${file}`, "utf8");
      for (const match of source.matchAll(/\bt\("([A-Za-z0-9_.-]+)"/g))
        expect(
          Object.hasOwn(builtin.translations.en, match[1]),
          `${file}:${match[1]}`,
        ).toBe(true);
      const ast = parser.parsers.typescript.parse(
        source,
        {} as import("prettier").ParserOptions,
      );
      function visit(node: unknown) {
        if (!node || typeof node !== "object") return;
        const n = node as Record<string, unknown>;
        if (n.type === "JSXAttribute") {
          const name = (n.name as { name?: string })?.name ?? "";
          if (
            [
              "value",
              "defaultValue",
              "role",
              "type",
              "className",
              "aria-current",
              "data-sidebar",
              "step",
            ].includes(name)
          )
            expect(source.slice(...(n.range as [number, number]))).not.toMatch(
              /\bt(?:Text)?\(/,
            );
        }
        for (const [key, value] of Object.entries(n))
          if (!["tokens", "comments", "loc", "range"].includes(key)) {
            if (Array.isArray(value)) value.forEach(visit);
            else if (value && typeof value === "object") visit(value);
          }
      }
      visit(ast);
    }
  });
});

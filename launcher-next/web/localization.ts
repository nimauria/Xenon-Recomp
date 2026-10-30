import { useSyncExternalStore } from "react";
import builtin from "./generated/localization.json";
export interface Language {
  name: string;
  nativeName: string;
  direction: "ltr" | "rtl";
}
export interface Catalogue {
  schema: string;
  version: number;
  defaultLanguage: string;
  languages: Record<string, Language>;
  translations: Record<string, Record<string, string>>;
  sources: Record<string, { original: string; locations: string[] }>;
}
type Parameters = Record<string, string | number | null | undefined>;
const fallback = builtin as Catalogue;
let catalog: Catalogue = fallback,
  selected = "auto",
  revision = 0;
const listeners = new Set<() => void>();
let originalKeys = new Map(
  Object.entries(fallback.sources).map(([key, value]) => [value.original, key]),
);
const own = <T>(record: Record<string, T>, key: string): T | undefined =>
  Object.hasOwn(record, key) ? record[key] : undefined;
export function resolveLanguage(
  value: string,
  languages: Catalogue["languages"] = catalog.languages,
  system = globalThis.navigator?.language ?? "en",
) {
  const id = (value === "auto" ? system : value).replaceAll("_", "-");
  const exact = Object.keys(languages).find(
    (key) => key.toLowerCase() === id.toLowerCase(),
  );
  const base = id.split("-")[0].toLowerCase();
  return exact ?? (Object.hasOwn(languages, base) ? base : "en");
}
export function language() {
  return resolveLanguage(selected);
}
export function t(key: string, parameters: Parameters = {}) {
  const value =
    own(catalog.translations[language()] ?? {}, key) ??
    own(catalog.translations.en, key) ??
    own(fallback.translations.en, key) ??
    `[${key}]`;
  return value.replace(/\{([A-Za-z][A-Za-z0-9_]*)\}/g, (match, name: string) =>
    Object.hasOwn(parameters, name) ? String(parameters[name] ?? "") : match,
  );
}
/** Program-owned labels/messages only. Never use for game/profile names or arbitrary content. */
export function tText(english: string) {
  const key = originalKeys.get(english);
  return key ? t(key) : english;
}
export function tCount(key: string, count: number) {
  const form = new Intl.PluralRules(language()).select(count);
  const candidate = `${key}.${form}`;
  return t(
    Object.hasOwn(fallback.translations.en, candidate)
      ? candidate
      : `${key}.other`,
    { count },
  );
}
export function setLanguage(value: string) {
  if (value === selected) return;
  selected = value;
  changed();
}
function changed() {
  revision++;
  if (typeof document !== "undefined") {
    document.documentElement.lang = language();
    document.documentElement.dir = catalog.languages[language()].direction;
  }
  listeners.forEach((listener) => listener());
}
export function useLocalization() {
  useSyncExternalStore(
    (listener) => {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
    () => revision,
  );
  return {
    t,
    tText,
    language: language(),
    selected,
    languages: catalog.languages,
  };
}
function record(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}
export function placeholders(value: string) {
  return [...new Set(value.match(/\{[A-Za-z][A-Za-z0-9_]*\}/g) ?? [])]
    .sort()
    .join(",");
}
export function validateCatalogue(value: unknown): Catalogue {
  if (
    !record(value) ||
    value.schema !== "xenon.localization" ||
    value.version !== 1 ||
    value.defaultLanguage !== "en" ||
    !record(value.languages) ||
    !record(value.translations) ||
    !record(value.translations.en) ||
    !record(value.sources) ||
    !Object.hasOwn(value.languages, "en") ||
    Object.keys(value.languages).length > 64
  )
    throw Error("Invalid localisation catalogue");
  const c = value as unknown as Catalogue;
  for (const [id, definition] of Object.entries(c.languages)) {
    if (
      !/^[a-z]{2,3}(?:-[A-Za-z0-9]{2,8})*$/.test(id) ||
      !record(definition) ||
      typeof definition.name !== "string" ||
      typeof definition.nativeName !== "string" ||
      !["ltr", "rtl"].includes(definition.direction) ||
      definition.name.length > 100 ||
      definition.nativeName.length > 100
    )
      throw Error("Invalid language definition");
  }
  for (const [id, translations] of Object.entries(c.translations)) {
    if (
      !Object.hasOwn(c.languages, id) ||
      !record(translations) ||
      Object.keys(translations).length > 20000
    )
      throw Error("Invalid translations");
    for (const [key, text] of Object.entries(translations)) {
      if (
        !/^[A-Za-z][A-Za-z0-9_.-]{0,100}$/.test(key) ||
        typeof text !== "string" ||
        !text.trim() ||
        text.length > 20000
      )
        throw Error("Invalid translation value");
      const english =
        own(c.translations.en, key) ?? own(fallback.translations.en, key);
      if (
        id !== "en" &&
        (!english || placeholders(text) !== placeholders(english))
      )
        throw Error("Translation placeholders do not match English");
    }
  }
  if (Object.keys(c.sources).length > 20000)
    throw Error("Invalid message sources");
  for (const source of Object.values(c.sources)) {
    if (
      !record(source) ||
      typeof source.original !== "string" ||
      source.original.length > 20000 ||
      !Array.isArray(source.locations) ||
      source.locations.length > 1000 ||
      !source.locations.every(
        (location) => typeof location === "string" && location.length < 1000,
      )
    )
      throw Error("Invalid message source");
  }
  return c;
}
export async function loadLocalization() {
  try {
    const response = await fetch("./localization.json", { cache: "no-store" });
    if (!response.ok) throw Error("Catalogue unavailable");
    const text = await response.text();
    if (text.length > 4 * 1024 * 1024) throw Error("Catalogue too large");
    catalog = validateCatalogue(JSON.parse(text));
  } catch {
    catalog = fallback;
  }
  originalKeys = new Map(
    [
      ...Object.entries(fallback.sources),
      ...Object.entries(catalog.sources),
    ].map(([key, value]) => [value.original, key]),
  );
  changed();
}
export function translationCoverage(id: string) {
  const keys = Object.keys(catalog.translations.en);
  return {
    translated: keys.filter(
      (key) => own(catalog.translations[id] ?? {}, key) !== undefined,
    ).length,
    total: keys.length,
  };
}

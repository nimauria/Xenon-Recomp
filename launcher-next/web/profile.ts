import type { Profile, Snapshot } from "../contracts/launcher";
import runtime from "../contracts/profile-runtime.json";
import definitions from "../contracts/profiles.json";
export function namePrefix(name: string, limit = definitions.nameLimit) {
  let result = name.slice(0, limit);
  const last = result.charCodeAt(result.length - 1);
  if (last >= 0xd800 && last <= 0xdbff) result = result.slice(0, -1);
  return result;
}
export function duplicateProfileName(profiles: Profile[], name: string) {
  const base = namePrefix(name, definitions.nameLimit - 5) + " copy";
  let candidate = base;
  for (
    let suffix = 2;
    profiles.some(
      (profile) =>
        profile.name.toLocaleLowerCase() === candidate.toLocaleLowerCase(),
    );
    suffix++
  ) {
    const ending = ` (${suffix})`;
    candidate =
      namePrefix(base, definitions.nameLimit - ending.length) + ending;
  }
  return candidate;
}
export function normalizeProfile(
  profile: Profile,
  settings: Record<string, unknown>,
  original?: Profile,
): Profile {
  if (
    profile.description !== original?.description &&
    profile.description.length > definitions.descriptionLimit
  )
    throw new Error(
      `Use a description of at most ${definitions.descriptionLimit} characters.`,
    );
  const normalized = { ...profile };
  const aliases: Record<string, string> = {
    Europe: "PAL",
    "North America": "NTSC-U",
    Japan: "NTSC-J",
    Asia: "NTSC-J",
  };
  const region = aliases[profile.region ?? ""];
  normalized.region =
    definitions.regions.find(
      (option) =>
        option.toLowerCase() === (region ?? profile.region ?? "").toLowerCase(),
    ) ?? definitions.regions[0];
  normalized.startupPage =
    definitions.startupPages.find(
      (option) =>
        option.toLowerCase() === (profile.startupPage ?? "").toLowerCase(),
    ) ?? "Launcher default";
  if (!profile.isolatedSettings) {
    normalized.runtimeOverrides = {};
    return normalized;
  }
  const overrides = profile.runtimeOverrides ?? {};
  if (typeof overrides !== "object" || Array.isArray(overrides))
    throw new Error("Invalid runtime overrides");
  normalized.runtimeOverrides = Object.keys(overrides).length
    ? { ...overrides }
    : Object.fromEntries(
        runtime.map((definition) => [
          definition.key,
          settings[definition.key] ?? definition.default,
        ]),
      );
  for (const [key, value] of Object.entries(normalized.runtimeOverrides)) {
    const definition = runtime.find((definition) => definition.key === key);
    if (!definition)
      throw new Error(`Unsupported profile runtime override: ${key}`);
    if (
      (definition.type === "bool" && typeof value !== "boolean") ||
      (definition.type === "number" &&
        (typeof value !== "number" || !Number.isFinite(value))) ||
      (definition.type === "enum" &&
        !definition.options.some((option) => option.value === value))
    )
      throw new Error("Invalid runtime override value");
    const range = definition as { minimum?: number; maximum?: number };
    if (
      range.minimum !== undefined &&
      (Number(value) < range.minimum || Number(value) > range.maximum!)
    )
      throw new Error("Runtime override outside allowed range");
  }
  return normalized;
}
export function projectProfiles(snapshot: Snapshot): Snapshot {
  const projected = structuredClone(snapshot);
  for (const profile of projected.profiles)
    profile.effectiveRuntimeSettings = Object.fromEntries(
      runtime.map((definition) => [
        definition.key,
        profile.isolatedSettings &&
        profile.runtimeOverrides?.[definition.key] !== undefined
          ? profile.runtimeOverrides[definition.key]
          : (projected.settings?.[definition.key] ?? definition.default),
      ]),
    );
  const active = projected.profiles.find(
    (profile) => profile.id === projected.activeProfileId,
  );
  if (projected.settings?.["general/restoreLastPage"] && projected.lastPage)
    projected.preferences.startPage = projected.lastPage;
  else if (active?.startupPage && active.startupPage !== "Launcher default")
    projected.preferences.startPage =
      active.startupPage as Snapshot["preferences"]["startPage"];
  return projected;
}

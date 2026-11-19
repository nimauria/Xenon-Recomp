import type { Snapshot } from "../contracts/launcher";
export const legacyFeatures = [
  "settings.general",
  "settings.system",
  "settings.appearance",
  "settings.library",
  "settings.paths",
  "settings.runtime",
  "settings.graphics",
  "settings.input",
  "settings.audio",
  "settings.network",
  "settings.filesystem",
  "settings.updates",
  "settings.community",
  "settings.accessibility",
  "settings.developer",
  "settings.about",
  "library.dlc",
  "library.manageFiles",
  "modules.updates",
  "modules.catalog",
  "modules.settings",
  "profiles.paths",
  "profiles.avatars",
] as const;
export function featureEnabled(state: Snapshot, feature: string): boolean {
  if (!legacyFeatures.includes(feature as (typeof legacyFeatures)[number]))
    return false;
  if (feature === "settings.network")
    return state.runtimeServices?.capabilities.networkConfiguration === true;
  return true;
}

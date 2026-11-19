import type { Game, Module, Snapshot } from "../contracts/launcher";
import definitions from "../contracts/profile-runtime.json";
export function inheritedGameSettings(state: Snapshot) {
  const profile = state.profiles.find((p) => p.id === state.activeProfileId);
  return Object.fromEntries(
    definitions.map((definition) => [
      definition.key,
      profile?.isolatedSettings &&
      profile.runtimeOverrides?.[definition.key] !== undefined
        ? profile.runtimeOverrides[definition.key]
        : (state.settings?.[definition.key] ?? definition.default),
    ]),
  );
}
export function validateGameSettings(
  runtime: unknown,
  moduleValues: unknown,
  module?: Module,
) {
  if (
    !runtime ||
    typeof runtime !== "object" ||
    Array.isArray(runtime) ||
    !moduleValues ||
    typeof moduleValues !== "object" ||
    Array.isArray(moduleValues)
  )
    throw Error("Game settings must be an object");
  for (const [key, value] of Object.entries(runtime)) {
    const definition = definitions.find((d) => d.key === key);
    if (!definition) throw Error("Unsupported game setting");
    const range = definition as { minimum?: number; maximum?: number };
    if (
      (definition.type === "bool" && typeof value !== "boolean") ||
      (definition.type === "enum" &&
        !definition.options.some((o) => o.value === value)) ||
      (definition.type === "number" &&
        (typeof value !== "number" ||
          !Number.isFinite(value) ||
          (range.minimum !== undefined && value < range.minimum) ||
          (range.maximum !== undefined && value > range.maximum)))
    )
      throw Error("Invalid game setting value");
  }
  for (const [key, value] of Object.entries(moduleValues)) {
    const definition = module?.schema.find((d) => d.id === key);
    if (!definition)
      throw Error(
        "This game option is no longer available; reset the game options",
      );
    if (
      (definition.type === "bool" && typeof value !== "boolean") ||
      (definition.type === "string" && typeof value !== "string") ||
      (definition.type === "number" &&
        (typeof value !== "number" ||
          !Number.isFinite(value) ||
          (definition.minimum !== undefined && value < definition.minimum) ||
          (definition.maximum !== undefined && value > definition.maximum))) ||
      (definition.type === "choice" &&
        !definition.options?.some(
          (o) => (typeof o === "object" ? o.value : o) === value,
        ))
    )
      throw Error("Invalid game setting value");
  }
  return {
    runtime: runtime as Record<string, unknown>,
    module: moduleValues as Record<string, unknown>,
  };
}
export function gameSettingsDraft(game: Game) {
  return {
    runtime: { ...game.runtimeOverrides },
    module: { ...game.moduleOverrides?.[game.moduleId ?? ""] },
  };
}

import type { Snapshot } from "../contracts/launcher";

export type CatalogueFilter = "all" | "platform" | "installed" | "ready";
export function catalogueEntries(
  state: Snapshot,
  query: string,
  filter: CatalogueFilter,
) {
  const installed = new Set(state.modules?.map((module) => module.id));
  const words = query.trim().toLocaleLowerCase().split(/\s+/).filter(Boolean);
  return (state.catalog?.entries ?? []).filter((entry) => {
    if (filter === "platform" && !entry.assetName) return false;
    if (filter === "installed" && !installed.has(entry.id)) return false;
    if (
      filter === "ready" &&
      state.moduleUpdates?.[entry.id]?.state !== "ready-to-install"
    )
      return false;
    const text =
      `${entry.name} ${entry.description} ${entry.publisher} ${entry.id}`.toLocaleLowerCase();
    return words.every((word) => text.includes(word));
  });
}

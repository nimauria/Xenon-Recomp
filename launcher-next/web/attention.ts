import type { Page, Snapshot } from "../contracts/launcher";
import { updatePackages } from "./updates";

export interface AttentionItem {
  key: "ready" | "updates" | "failed" | "sources";
  count: number;
  page: Page;
}
export function attentionItems(state: Snapshot): AttentionItem[] {
  const packages = updatePackages(state);
  const installed = new Set(state.modules?.map((module) => module.id));
  const items: AttentionItem[] = [
    {
      key: "ready",
      count: packages.filter((p) => p.value?.state === "ready-to-install")
        .length,
      page: "Downloads",
    },
    {
      key: "updates",
      count: packages.filter(
        (p) =>
          (p.launcher || installed.has(p.id)) &&
          ["available", "update-available"].includes(p.value?.state ?? ""),
      ).length,
      page: "Updates",
    },
    {
      key: "failed",
      count: state.jobs?.filter((job) => job.state === "failed").length ?? 0,
      page: "Downloads",
    },
    {
      key: "sources",
      count: state.games.filter((game) =>
        game.media?.some((media) => media.missing),
      ).length,
      page: "Library",
    },
  ];
  return items.filter((item) => item.count > 0);
}

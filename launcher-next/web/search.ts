import type { LauncherIntent, Snapshot } from "../contracts/launcher";
import definitions from "../contracts/settings.json";
import { navigationGroups } from "./navigation";
export interface SearchResult {
  id: string;
  group: string;
  title: string;
  description: string;
  keywords?: string;
  priority: number;
  quick?: boolean;
  intent: LauncherIntent;
}
export function commandResults(
  state: Snapshot,
  query: string,
  limit = 24,
): SearchResult[] {
  const result: SearchResult[] = [];
  const add = (value: SearchResult) => result.push(value);
  for (const group of navigationGroups)
    if (!group.developer || state.settings?.["developer/modeEnabled"])
      for (const page of group.pages)
        add({
          id: `page:${page}`,
          group: "Pages",
          title: `Go to ${page}`,
          description: `Open ${page}`,
          priority: page === "Home" ? 100 : 90,
          quick: true,
          intent: { type: "navigate", page },
        });
  add({
    id: "profile:create",
    group: "Commands",
    title: "Create profile",
    description: "Create a launcher profile",
    keywords: "new user add",
    priority: 72,
    quick: true,
    intent: { type: "navigate", page: "Profiles", section: "create" },
  });
  add({
    id: "catalog",
    group: "Commands",
    title: "Browse module catalogue",
    description: "Browse the Xenon Modules registry",
    keywords: "catalog registry discover install packages",
    priority: 70,
    quick: true,
    intent: { type: "navigate", page: "Modules", section: "catalog" },
  });
  if (state.capabilities.online) {
    add({
      id: "launcher:check",
      group: "Commands",
      title: "Check for launcher updates",
      description: "Check the launcher release channel",
      priority: 66,
      quick: true,
      intent: { type: "action", name: "updates.check" },
    });
    add({
      id: "modules:check",
      group: "Commands",
      title: "Check module updates",
      description: "Check all registered modules",
      priority: 64,
      quick: true,
      intent: { type: "action", name: "modules.checkAllUpdates" },
    });
  }
  if (state.mode === "native")
    add({
      id: "support:bundle",
      group: "Commands",
      title: "Create support bundle",
      description: "Export sanitized diagnostics",
      keywords: "logs zip bug report",
      priority: 62,
      quick: true,
      intent: { type: "action", name: "support.bundle" },
    });
  add({
    id: "community:discord",
    group: "Commands",
    title: "Join Xenon Discord",
    description: "Open community and support",
    keywords: "help server",
    priority: 58,
    quick: true,
    intent: {
      type: "action",
      name: "system.openUrl",
      data: { url: "https://discord.gg/zVGw3HADgA" },
    },
  });
  if (
    state.session &&
    ["starting", "running", "stopping"].includes(state.session.state)
  )
    add({
      id: "session:stop",
      group: "Session",
      title: `Stop ${state.session.title ?? "current session"}`,
      description: "Stop or cancel the current session",
      priority: 130,
      quick: true,
      intent: { type: "action", name: "session.stop" },
    });
  for (const category of new Set(
    definitions.filter((d) => d.category !== "internal").map((d) => d.category),
  ))
    add({
      id: `settings:${category}`,
      group: "Settings",
      title: category[0].toUpperCase() + category.slice(1),
      description: `Settings · ${category}`,
      keywords: definitions
        .filter((d) => d.category === category)
        .map((d) => `${d.label} ${d.key}`)
        .join(" "),
      priority: 30,
      intent: { type: "navigate", page: "Settings", targetId: category },
    });
  for (const game of state.games)
    add({
      id: `game:${game.id}`,
      group: "Games",
      title: game.title,
      description: `Library · ${game.module}`,
      keywords: `${game.id} ${game.moduleId ?? ""} ${game.description}`,
      priority: 46,
      intent: { type: "navigate", page: "Library", targetId: game.id },
    });
  for (const module of state.modules ?? [])
    add({
      id: `module:${module.id}`,
      group: "Modules",
      title: module.name,
      description: `Module · Version ${module.version}`,
      keywords: module.id,
      priority: 44,
      intent: { type: "navigate", page: "Modules", targetId: module.id },
    });
  for (const profile of state.profiles)
    add({
      id: `profile:${profile.id}`,
      group: "Profiles",
      title: profile.name,
      description:
        profile.id === state.activeProfileId
          ? "Profiles · Active profile"
          : "Profiles · Launcher profile",
      keywords: `${profile.description} ${profile.region ?? ""}`,
      priority: 40,
      intent: { type: "navigate", page: "Profiles", targetId: profile.id },
    });
  const normalized = (value: string) =>
    value.toLocaleLowerCase().trim().replace(/\s+/g, " ");
  const needle = normalized(query),
    tokens = needle.split(" ").filter(Boolean);
  return result
    .map((value) => {
      if (!needle) return { value, score: value.quick ? value.priority : -1 };
      const title = normalized(value.title),
        secondary = normalized(`${value.description} ${value.keywords ?? ""}`);
      if (
        tokens.some(
          (token) => !title.includes(token) && !secondary.includes(token),
        )
      )
        return { value, score: -1 };
      const score =
        value.priority +
        tokens.reduce(
          (total, token) =>
            total +
            (title.startsWith(token) ? 120 : title.includes(token) ? 80 : 32),
          0,
        ) +
        (title === needle
          ? 600
          : title.startsWith(needle)
            ? 350
            : title.includes(needle)
              ? 180
              : 0);
      return { value, score };
    })
    .filter((entry) => entry.score >= 0)
    .sort(
      (a, b) =>
        b.score - a.score ||
        a.value.group.localeCompare(b.value.group) ||
        a.value.title.localeCompare(b.value.title),
    )
    .slice(0, Math.min(50, Math.max(1, limit)))
    .map((entry) => entry.value);
}

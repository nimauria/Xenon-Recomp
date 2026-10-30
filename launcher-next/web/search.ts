import { t, tText } from "./localization";
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
          group: tText("Pages"),
          title: t("command.goto", { page: tText(page) }),
          description: t("command.open", { page: tText(page) }),
          priority: page === "Home" ? 100 : 90,
          quick: true,
          intent: { type: "navigate", page },
        });
  add({
    id: "profile:create",
    group: tText("Commands"),
    title: tText("Create profile"),
    description: tText("Create a launcher profile"),
    keywords: "new user add",
    priority: 72,
    quick: true,
    intent: { type: "navigate", page: "Profiles", section: "create" },
  });
  add({
    id: "catalog",
    group: tText("Commands"),
    title: tText("Browse module catalogue"),
    description: tText("Browse the Xenon Modules registry"),
    keywords: "catalog registry discover install packages",
    priority: 70,
    quick: true,
    intent: { type: "navigate", page: "Modules", section: "catalog" },
  });
  if (state.capabilities.online) {
    add({
      id: "launcher:check",
      group: tText("Commands"),
      title: tText("Check for launcher updates"),
      description: tText("Check the launcher release channel"),
      priority: 66,
      quick: true,
      intent: { type: "action", name: "updates.check" },
    });
    add({
      id: "modules:check",
      group: tText("Commands"),
      title: tText("Check module updates"),
      description: tText("Check all registered modules"),
      priority: 64,
      quick: true,
      intent: { type: "action", name: "modules.checkAllUpdates" },
    });
  }
  if (state.mode === "native")
    add({
      id: "support:bundle",
      group: tText("Commands"),
      title: tText("Create support bundle"),
      description: tText("Export sanitized diagnostics"),
      keywords: "logs zip bug report",
      priority: 62,
      quick: true,
      intent: { type: "action", name: "support.bundle" },
    });
  add({
    id: "community:discord",
    group: tText("Commands"),
    title: tText("Join Xenon Discord"),
    description: tText("Open community and support"),
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
      group: tText("Session"),
      title: t("command.stop", {
        title: state.session.title ?? tText("current session"),
      }),
      description: tText("Stop or cancel the current session"),
      priority: 130,
      quick: true,
      intent: { type: "action", name: "session.stop" },
    });
  for (const category of new Set(
    definitions.filter((d) => d.category !== "internal").map((d) => d.category),
  ))
    add({
      id: `settings:${category}`,
      group: tText("Settings"),
      title: tText(category[0].toUpperCase() + category.slice(1)),
      description: t("command.settings", {
        category: tText(category[0].toUpperCase() + category.slice(1)),
      }),
      keywords: definitions
        .filter((d) => d.category === category)
        .map((d) => `${tText(d.label)} ${d.key}`)
        .join(" "),
      priority: 30,
      intent: { type: "navigate", page: "Settings", targetId: category },
    });
  for (const game of state.games)
    add({
      id: `game:${game.id}`,
      group: tText("Games"),
      title: game.title,
      description: t("command.library", { module: game.module }),
      keywords: `${game.id} ${game.moduleId ?? ""} ${game.description}`,
      priority: 46,
      intent: { type: "navigate", page: "Library", targetId: game.id },
    });
  for (const module of state.modules ?? [])
    add({
      id: `module:${module.id}`,
      group: tText("Modules"),
      title: module.name,
      description: t("command.version", { version: module.version }),
      keywords: module.id,
      priority: 44,
      intent: { type: "navigate", page: "Modules", targetId: module.id },
    });
  for (const profile of state.profiles)
    add({
      id: `profile:${profile.id}`,
      group: tText("Profiles"),
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

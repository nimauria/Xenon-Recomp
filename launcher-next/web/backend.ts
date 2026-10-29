import {
  protocolVersion,
  type Command,
  type Commands,
  type Snapshot,
  type Transport,
} from "../contracts/launcher";
import definitions from "../contracts/settings.json";
import {
  normalizeProfile,
  duplicateProfileName,
  projectProfiles,
} from "./profile";

export function initialState(): Snapshot {
  return {
    revision: 0,
    mode: "preview",
    profiles: [
      {
        id: "local",
        name: "Local player",
        description: "Your launcher profile",
      },
    ],
    activeProfileId: "local",
    games: [],
    preferences: { theme: "dark", reducedMotion: false, startPage: "Library" },
    settings: {},
    modules: [],
    notifications: [],
    history: [],
    jobs: [],
    captures: [],
    capabilities: {
      runtime: false,
      importContent: false,
      modules: false,
      captures: false,
    },
  };
}
const pages = [
  "Library",
  "Home",
  "Modules",
  "Profiles",
  "Captures",
  "Downloads",
  "Settings",
  "Support",
  "Network",
  "Filesystem",
  "Developer",
];
export function validateState(value: unknown): Snapshot {
  if (!value || typeof value !== "object")
    throw new Error("Invalid launcher state");
  const s = value as Snapshot;
  if (
    !Number.isSafeInteger(s.revision) ||
    s.revision < 0 ||
    !Array.isArray(s.profiles) ||
    !s.profiles.length ||
    !s.profiles.every(
      (p) =>
        p &&
        typeof p.id === "string" &&
        p.id.length > 0 &&
        typeof p.name === "string" &&
        p.name.trim().length > 0 &&
        typeof p.description === "string",
    ) ||
    new Set(s.profiles.map((p) => p.id)).size !== s.profiles.length ||
    !s.profiles.some((p) => p.id === s.activeProfileId) ||
    !Array.isArray(s.games) ||
    !s.games.every(
      (g) =>
        g &&
        typeof g.id === "string" &&
        typeof g.title === "string" &&
        typeof g.module === "string" &&
        typeof g.description === "string" &&
        typeof g.favorite === "boolean" &&
        (g.lastPlayed === null || typeof g.lastPlayed === "string"),
    ) ||
    !s.preferences ||
    !["dark", "light"].includes(s.preferences.theme) ||
    typeof s.preferences.reducedMotion !== "boolean" ||
    !pages.includes(s.preferences.startPage) ||
    !s.capabilities ||
    !["runtime", "importContent", "modules", "captures"].every(
      (k) =>
        typeof s.capabilities[k as keyof Snapshot["capabilities"]] ===
        "boolean",
    ) ||
    !["native", "preview"].includes(s.mode)
  )
    throw new Error("Unsupported or corrupt launcher state");
  return s;
}
/** Browser-only development adapter. Never loads fictional games automatically. */
export class PreviewTransport implements Transport {
  constructor(
    private state: Snapshot,
    private persist: (state: Snapshot) => void,
  ) {}
  async request<K extends Command>(
    command: K,
    input: Commands[K]["input"],
  ): Promise<Commands[K]["output"]> {
    const next = structuredClone(this.state);
    switch (command) {
      case "application.snapshot":
        return projectProfiles(this.state) as Commands[K]["output"];
      case "profiles.create": {
        const name = (
          input as Commands["profiles.create"]["input"]
        ).name.trim();
        if (!name || name.length > 48)
          throw new Error("Use a profile name between 1 and 48 characters.");
        if (
          next.profiles.some(
            (p) => p.name.toLocaleLowerCase() === name.toLocaleLowerCase(),
          )
        )
          throw new Error("That profile name is already in use.");
        const data = input as Commands["profiles.create"]["input"];
        next.profiles.push(
          normalizeProfile(
            {
              ...data,
              id: crypto.randomUUID(),
              name,
              description: data.description ?? "",
            },
            next.settings ?? {},
          ),
        );
        break;
      }
      case "profiles.activate": {
        const { id } = input as Commands["profiles.activate"]["input"];
        if (!next.profiles.some((p) => p.id === id))
          throw new Error("Profile no longer exists.");
        next.activeProfileId = id;
        break;
      }
      case "profiles.remove": {
        const { id } = input as Commands["profiles.remove"]["input"];
        if (id === next.activeProfileId)
          throw new Error("Activate another profile before removing this one.");
        if (!next.profiles.some((p) => p.id === id))
          throw new Error("Profile no longer exists.");
        next.profiles = next.profiles.filter((p) => p.id !== id);
        break;
      }
      case "settings.update":
        Object.assign(next.preferences, input);
        break;
      case "library.favorite": {
        const { id, favorite } = input as Commands["library.favorite"]["input"];
        const game = next.games.find((g) => g.id === id);
        if (!game) throw new Error("Game no longer exists.");
        game.favorite = favorite;
        break;
      }
      case "launcher.action": {
        const { action, data } = input as Commands["launcher.action"]["input"];
        next.settings ??= {};
        next.notifications ??= [];
        next.history ??= [];
        if (action === "settings.set") {
          if (
            data.key === "community/discordRichPresence" &&
            data.value !== false
          )
            throw new Error("Discord Rich Presence provider is unavailable");
          if (typeof data.key !== "string")
            throw new Error("Invalid setting key");
          const definition = definitions.find((d) => d.key === data.key);
          if (!definition) throw new Error("Unknown setting");
          if (
            (definition.type === "bool" && typeof data.value !== "boolean") ||
            (definition.type === "number" &&
              (typeof data.value !== "number" ||
                !Number.isFinite(data.value))) ||
            (["enum", "string"].includes(definition.type) &&
              typeof data.value !== "string")
          )
            throw new Error("Invalid setting type");
          if (
            definition.type === "enum" &&
            !definition.options.some((o) => o.value === data.value)
          )
            throw new Error("Invalid setting option");
          const range = definition as typeof definition & {
            minimum?: number;
            maximum?: number;
          };
          if (
            range.minimum !== undefined &&
            (Number(data.value) < range.minimum ||
              Number(data.value) > range.maximum!)
          )
            throw new Error("Setting outside permitted range");
          next.settings[data.key] = data.value;
          if (data.key === "general/startupPage")
            next.preferences.startPage =
              data.value as Snapshot["preferences"]["startPage"];
          if (data.key === "accessibility/reduceMotion")
            next.preferences.reducedMotion = Boolean(data.value);
        } else if (action === "settings.resetKey") {
          const definition = definitions.find((d) => d.key === data.key);
          if (!definition) throw new Error("Unknown setting");
          delete next.settings[String(data.key)];
          if (data.key === "general/startupPage")
            next.preferences.startPage =
              definition.default as Snapshot["preferences"]["startPage"];
          if (data.key === "accessibility/reduceMotion")
            next.preferences.reducedMotion = Boolean(definition.default);
        } else if (action === "settings.reset") {
          for (const key of Object.keys(next.settings))
            if (
              !data.category ||
              key.startsWith(`${data.category}/`) ||
              definitions.find((d) => d.key === key)?.category === data.category
            )
              delete next.settings[key];
          if (!data.category || data.category === "general")
            next.preferences.startPage = "Library";
          if (!data.category || data.category === "appearance")
            next.preferences.theme = "dark";
          if (!data.category || data.category === "accessibility")
            next.preferences.reducedMotion = false;
        } else if (
          action === "profiles.update" ||
          action === "profiles.duplicate"
        ) {
          const profile = next.profiles.find((p) => p.id === data.id);
          if (!profile) throw new Error("Profile no longer exists.");
          const name = String(
            action === "profiles.duplicate"
              ? duplicateProfileName(next.profiles, profile.name)
              : data.name,
          ).trim();
          if (
            !name ||
            name.length > 48 ||
            next.profiles.some(
              (p) =>
                (action === "profiles.duplicate" || p.id !== profile.id) &&
                p.name.toLocaleLowerCase() === name.toLocaleLowerCase(),
            )
          )
            throw new Error(
              "Choose a unique profile name, up to 48 characters.",
            );
          const edited = normalizeProfile(
            { ...profile, ...data, name, id: profile.id },
            next.settings ?? {},
            profile,
          );
          if (action === "profiles.duplicate")
            next.profiles.push({ ...edited, id: crypto.randomUUID(), name });
          else Object.assign(profile, edited);
        } else if (action === "navigation.remember") {
          if (!pages.includes(String(data.page)))
            throw new Error("Unknown navigation destination");
          next.lastPage = data.page as Snapshot["lastPage"];
        } else if (action === "notifications.readAll")
          next.notifications.forEach((n) => (n.read = true));
        else if (action === "notifications.dismiss")
          next.notifications = next.notifications.filter(
            (n) => n.id !== data.id,
          );
        else if (action === "notifications.clear") next.notifications = [];
        else if (action === "notifications.read") {
          const notification = next.notifications.find((n) => n.id === data.id);
          if (notification) notification.read = Boolean(data.read);
        } else if (action === "history.clear") next.history = [];
        else if (action === "library.remove")
          next.games = next.games.filter((g) => g.id !== data.id);
        else
          throw new Error(
            "This operation requires the native desktop launcher.",
          );
        break;
      }
      default:
        throw new Error(
          command === "session.launch"
            ? "The Xenon runtime is not connected."
            : "This action requires the native launcher service; it is unavailable in browser preview.",
        );
    }
    next.revision++;
    validateState(next);
    this.persist(next); // Commit memory only after durable storage succeeds.
    this.state = next;
    return projectProfiles(next) as Commands[K]["output"];
  }
}
declare global {
  interface Window {
    cefQuery?: (query: {
      request: string;
      onSuccess: (value: string) => void;
      onFailure: (code: number, message: string) => void;
    }) => void;
  }
}
class CefTransport implements Transport {
  subscribe(listener: (revision: number) => void) {
    const receive = (event: Event) => {
      const detail = (event as CustomEvent).detail;
      if (
        detail?.version === 1 &&
        Number.isSafeInteger(detail.revision) &&
        detail.revision >= 0
      )
        listener(detail.revision);
    };
    window.addEventListener("xenon:revision", receive);
    return () => window.removeEventListener("xenon:revision", receive);
  }
  request<K extends Command>(
    command: K,
    payload: Commands[K]["input"],
  ): Promise<Commands[K]["output"]> {
    const id = crypto.randomUUID();
    return new Promise((resolve, reject) => {
      const timeout = window.setTimeout(
        () => reject(new Error("The launcher service did not respond.")),
        command === "native.dialog" ||
          (command === "launcher.action" &&
            (payload as { action?: string }).action === "updates.install")
          ? 300000
          : 15000,
      );
      window.cefQuery!({
        request: JSON.stringify({
          version: protocolVersion,
          id,
          command,
          payload,
        }),
        onSuccess: (raw) => {
          clearTimeout(timeout);
          try {
            const result = JSON.parse(raw);
            if (
              result.version !== protocolVersion ||
              result.id !== id ||
              typeof result.ok !== "boolean"
            )
              throw new Error("Invalid launcher response");
            if (!result.ok)
              throw new Error(
                result.error?.message || "Launcher command failed",
              );
            if (command === "native.dialog") {
              if (
                !result.data ||
                !Array.isArray(result.data.paths) ||
                !result.data.paths.every((p: unknown) => typeof p === "string")
              )
                throw new Error("Invalid file dialog response");
              resolve(result.data);
            } else if (command === "session.launch") {
              if (
                !result.data ||
                typeof result.data.jobId !== "string" ||
                !result.data.jobId
              )
                throw new Error("Invalid launch response");
              resolve(result.data);
            } else resolve(validateState(result.data) as Commands[K]["output"]);
          } catch (error) {
            reject(error);
          }
        },
        onFailure: (_code, message) => {
          clearTimeout(timeout);
          reject(new Error(message));
        },
      });
    });
  }
}
export function connect(): Transport {
  if (window.cefQuery) return new CefTransport();
  const stored = localStorage.getItem("xenon.launcher-next.preview.v1");
  const state = stored ? validateState(JSON.parse(stored)) : initialState();
  // Stored data cannot grant native capabilities to a browser preview.
  state.mode = "preview";
  state.capabilities = initialState().capabilities;
  return new PreviewTransport(state, (value) =>
    localStorage.setItem(
      "xenon.launcher-next.preview.v1",
      JSON.stringify(value),
    ),
  );
}

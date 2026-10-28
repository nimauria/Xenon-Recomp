/** Wire version is independent of launcher and runtime versions. No Qt types. */
export const protocolVersion = 1 as const;
export type Page =
  | "Library"
  | "Home"
  | "Modules"
  | "Profiles"
  | "Captures"
  | "Downloads"
  | "Settings"
  | "Support"
  | "Network"
  | "Filesystem"
  | "Developer";
export interface Profile {
  id: string;
  name: string;
  description: string;
  region?: string;
  offline?: boolean;
  startupPage?: string;
  gamePath?: string;
  savePath?: string;
  screenshotPath?: string;
  isolatedSettings?: boolean;
  avatarPath?: string;
  avatarKey?: string;
  avatarFocalX?: number;
  avatarFocalY?: number;
  avatarZoom?: number;
}
export interface Dlc {
  id: string;
  name: string;
  path: string;
  verified: boolean;
}
export interface Game {
  id: string;
  title: string;
  module: string;
  description: string;
  favorite: boolean;
  lastPlayed: string | null;
  contentPath?: string;
  moduleId?: string;
  dlc?: Dlc[];
  verified?: boolean;
  addedAt?: string;
  tileArt?: string;
  heroArt?: string;
  tileArtFocalX?: number;
  tileArtFocalY?: number;
  heroArtFocalX?: number;
  heroArtFocalY?: number;
}
export interface ModuleSetting {
  id: string;
  label?: string;
  description?: string;
  type: string;
  default?: unknown;
  options?: (string | { label: string; value: string })[];
}
export interface Module {
  id: string;
  name: string;
  version: string;
  path: string;
  enabled: boolean;
  schema: ModuleSetting[];
  settings: Record<string, unknown>;
  previousVersions?: { version: string; path: string }[];
}
export interface Notification {
  id: string;
  title: string;
  message: string;
  read: boolean;
  time: string;
}
export interface Capture {
  id: string;
  name: string;
  size: number;
}
export interface Job {
  id: string;
  title: string;
  state: string;
  progress: number;
  error?: string;
}
export interface Preferences {
  theme: "dark" | "light";
  reducedMotion: boolean;
  startPage: Page;
}
export interface Snapshot {
  revision: number;
  mode: "preview" | "native";
  profiles: Profile[];
  activeProfileId: string;
  games: Game[];
  preferences: Preferences;
  capabilities: {
    runtime: boolean;
    importContent: boolean;
    modules: boolean;
    captures: boolean;
  };
  settings?: Record<string, unknown>;
  modules?: Module[];
  notifications?: Notification[];
  history?: { id: string; title: string; time: string; state: string }[];
  jobs?: Job[];
  captures?: Capture[];
  storagePath?: string;
  session?: {
    sessionId?: string;
    gameId?: string;
    title?: string;
    state: string;
    fixture: boolean;
    lastError?: string;
    logTail?: string;
    startedAtEpochMs?: number;
    endedAtEpochMs?: number;
  };
  recovery?: { previousUncleanExit: boolean; safeMode: boolean };
}
export interface Commands {
  "application.snapshot": { input: Record<string, never>; output: Snapshot };
  "profiles.create": {
    input: Partial<Omit<Profile, "id">> & { name: string };
    output: Snapshot;
  };
  "profiles.activate": { input: { id: string }; output: Snapshot };
  "profiles.remove": { input: { id: string }; output: Snapshot };
  "settings.update": { input: Partial<Preferences>; output: Snapshot };
  "library.favorite": {
    input: { id: string; favorite: boolean };
    output: Snapshot;
  };
  "library.import": { input: Record<string, never>; output: Snapshot };
  "session.launch": { input: { gameId: string }; output: { jobId: string } };
  "launcher.action": {
    input: { action: string; data: Record<string, unknown> };
    output: Snapshot;
  };
  "native.dialog": {
    input: { mode: "open" | "multiple" | "folder" | "save" };
    output: { paths: string[] };
  };
}
export type Command = keyof Commands;
export interface Request<K extends Command = Command> {
  version: typeof protocolVersion;
  id: string;
  command: K;
  payload: Commands[K]["input"];
}
export type Response<T> =
  | { version: 1; id: string; ok: true; data: T }
  | {
      version: 1;
      id: string;
      ok: false;
      error: { code: string; message: string };
    };
export interface Transport {
  request<K extends Command>(
    command: K,
    input: Commands[K]["input"],
  ): Promise<Commands[K]["output"]>;
}

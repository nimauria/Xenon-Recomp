/** Wire version is independent of launcher and runtime versions. No Qt types. */
import type { HostServices } from "./services";
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
  runtimeOverrides?: Record<string, unknown>;
  effectiveRuntimeSettings?: Record<string, unknown>;
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
  managed?: boolean;
  receiptVerified?: boolean;
  lastCheckedAt?: string;
  catalogId?: string;
}
export interface Game {
  playCount?: number;
  totalPlayTimeMs?: number;
  fixtureActivity?: {
    playCount: number;
    totalPlayTimeMs: number;
    lastPlayed?: string;
  };
  media?: GameMedia[];
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
  managedCopies?: {
    root: string;
    originalPath: string;
    originalRetained: boolean;
    entryId: string;
    locationId?: string;
    type: "game" | "dlc";
    previous?: boolean;
  }[];
  tileArt?: string;
  heroArt?: string;
  tileArtFocalX?: number;
  tileArtFocalY?: number;
  heroArtFocalX?: number;
  heroArtFocalY?: number;
  contentReport?: {
    fixture: boolean;
    identified: boolean;
    integrityVerified: boolean;
    message: string;
  };
}
export interface GameMedia {
  missing?: boolean;
  id: string;
  name: string;
  sourcePath: string;
  sourceType: string;
  mediaId: string;
  discNumber: number;
  discCount: number;
  region: string;
  importMode: "kept" | "copied" | "moved";
  isPrimary: boolean;
  identityVerified: boolean;
}
export type ModuleSettingValue = string | number | boolean;
export interface ModuleSetting {
  id: string;
  label?: string;
  description?: string;
  type: string;
  default?: unknown;
  options?: (
    ModuleSettingValue | { label: string; value: ModuleSettingValue }
  )[];
  minimum?: number;
  maximum?: number;
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
  severity?: string;
  repeatCount?: number;
  action?: LauncherIntent;
}
export type LauncherIntent =
  | { type: "navigate"; page: Page; targetId?: string; section?: string }
  | { type: "action"; name: string; data?: Record<string, unknown> };
export interface Capture {
  id: string;
  name: string;
  size: number;
  modified?: number;
  version?: string;
  viewable?: boolean;
}
export interface Job {
  id: string;
  title: string;
  state: string;
  progress: number;
  error?: string;
  kind?: string;
  phase?: string;
  canCancel?: boolean;
  canRetry?: boolean;
  bytesDone?: number;
  bytesTotal?: number;
  cleanupError?: string;
  warning?: string;
  cleaningUp?: boolean;
  sourceName?: string;
  retryOf?: string;
}
export interface Preferences {
  theme: "dark" | "light";
  reducedMotion: boolean;
  startPage: Page;
}
export interface CatalogEntry {
  id: string;
  name: string;
  description: string;
  repository: string;
  repositoryUrl: string;
  publisher: string;
  verified: boolean;
  hostKey: string;
  assetName: string;
  launcher?: {
    gameTitle?: string;
    description?: string;
    tileArt?: string;
    heroArt?: string;
  };
  dlc?: {
    id: string;
    name: string;
    description?: string;
    category?: string;
    group?: string;
    identityStatus?: string;
  }[];
}
export interface Release {
  version: string;
  name: string;
  notes: string;
  url: string;
  prerelease: boolean;
  publishedAt: string;
  asset?: { name: string; url: string; size: number; sha256: string } | null;
}
export interface UpdateState {
  state: string;
  release?: Release | null;
  lastError?: string;
  lastChecked?: string;
  installedVersion?: string;
  downloadVersion?: string;
  busy?: boolean;
  canCheck?: boolean;
  canDownload?: boolean;
  canInstall?: boolean;
  updateAvailable?: boolean;
}
export interface UpdateHistory {
  id: string;
  moduleId: string;
  action: string;
  outcome: string;
  message: string;
  time: string;
}
export interface Snapshot {
  activitySummary?: {
    playCount: number;
    totalPlayTimeMs: number;
    fixturePlayCount: number;
    fixturePlayTimeMs: number;
  };
  dlcCatalog?: Record<
    string,
    {
      dlcId: string;
      name: string;
      description: string;
      version: string;
      contentIds: string[];
      category: string;
      group: string;
    }[]
  >;
  revision: number;
  mode: "preview" | "native";
  profiles: Profile[];
  activeProfileId: string;
  games: Game[];
  preferences: Preferences;
  lastPage?: Page;
  runtimeServices?: HostServices;
  serviceResult?: {
    command: string;
    result: Record<string, unknown>;
    jobId: string;
    fixture: boolean;
  };
  capabilities: {
    runtime: boolean;
    importContent: boolean;
    modules: boolean;
    captures: boolean;
    online?: boolean;
    networkFixture?: boolean;
  };
  settings?: Record<string, unknown>;
  modules?: Module[];
  notifications?: Notification[];
  history?: {
    id: string;
    gameId?: string;
    mediaId?: string;
    durationMs?: number;
    reachedRunning?: boolean;
    title: string;
    time: string;
    state: string;
    fixture?: boolean;
  }[];
  jobs?: Job[];
  captures?: Capture[];
  captureFolderPath?: string;
  captureScan?: { state: string; truncated: boolean; error?: string };
  migration?: {
    id: string;
    state: string;
    source: string;
    counts: Record<
      "profiles" | "avatars" | "games" | "modules" | "settings" | "skipped",
      number
    >;
    warnings: string[];
    changes?: Record<string, string[]>;
    replaceSettings: boolean;
    activateProfile: boolean;
    backupPath?: string;
  };
  catalog?: {
    state: string;
    entries: CatalogEntry[];
    lastError?: string;
    lastChecked?: string;
    errors?: string[];
  };
  moduleUpdates?: Record<string, UpdateState>;
  launcherUpdate?: UpdateState;
  updateHistory?: UpdateHistory[];
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
  subscribe?(listener: (revision: number) => void): () => void;
  request<K extends Command>(
    command: K,
    input: Commands[K]["input"],
  ): Promise<Commands[K]["output"]>;
}

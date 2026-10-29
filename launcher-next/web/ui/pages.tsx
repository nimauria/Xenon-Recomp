import { useEffect, useMemo, useState } from "react";
import type { Game, Module, Profile } from "../../contracts/launcher";
import { playtime, activityTime } from "../activity";
import { GameMedia } from "./GameMedia";
import { GameAddons } from "./GameAddons";
import { GameCollection } from "./GameCollection";
import { GameCarousel } from "./GameCarousel";
import { CoverImage } from "./CoverImage";
import { AppearanceSettings } from "./AppearanceSettings";
import { LegacyMigration } from "./LegacyMigration";
import { InputSettings } from "./RuntimeServices";
import definitions from "../../contracts/settings.json";
import profileRuntime from "../../contracts/profile-runtime.json";
import profileDefinitions from "../../contracts/profiles.json";
import { isActiveJob } from "../jobs";
import {
  ModuleCatalogue,
  UpdateControls,
  UpdatesPanel,
  UpdateHistory,
} from "./ModuleCatalogue";
import { Empty, Modal, downloadJson, useLauncher } from "./context";

export function Library() {
  const { state, busy, query, action, pick, libraryBrowse, setLibraryBrowse } =
    useLauncher();
  const { selected, filter, sort, scroll } = libraryBrowse;
  const select = (id: string) =>
    setLibraryBrowse((previous) => ({ ...previous, selected: id }));
  const setFilter = (filter: string) =>
    setLibraryBrowse((previous) => ({ ...previous, filter, scroll: 0 }));
  const setSort = (sort: string) =>
    setLibraryBrowse((previous) => ({ ...previous, sort, scroll: 0 }));
  const [properties, setProperties] = useState<Game>();
  const [importPaths, setImportPaths] = useState<string[]>([]);
  const [moduleSettings, setModuleSettings] = useState<Module>();
  const view = String(state.settings?.["library/viewMode"] ?? "Focused");
  useEffect(() => {
    if (
      busy ||
      !selected ||
      state.settings?.["library/rememberSelection"] === false ||
      state.settings?.["library/lastSelectedGameId"] === selected
    )
      return;
    const timer = window.setTimeout(
      () =>
        void action("settings.set", {
          key: "library/lastSelectedGameId",
          value: selected,
        }),
      400,
    );
    return () => window.clearTimeout(timer);
  }, [
    selected,
    busy,
    state.settings?.["library/rememberSelection"],
    state.settings?.["library/lastSelectedGameId"],
  ]);
  const games = useMemo(
    () =>
      state.games
        .filter(
          (g) =>
            g.title.toLowerCase().includes(query.toLowerCase()) &&
            (filter !== "Favourites" || g.favorite),
        )
        .sort((a, b) =>
          sort === "A–Z"
            ? a.title.localeCompare(b.title)
            : sort === "Z–A"
              ? b.title.localeCompare(a.title)
              : sort === "Recently played"
                ? activityTime(b.lastPlayed) - activityTime(a.lastPlayed)
                : sort === "Most played"
                  ? (b.playCount ?? 0) - (a.playCount ?? 0)
                  : activityTime(b.addedAt) - activityTime(a.addedAt),
        ),
    [state.games, query, filter, sort],
  );
  const game = games.find((g) => g.id === selected) ?? games[0];
  const sessionActive = Boolean(
    state.session &&
    !["idle", "unavailable", "failed", "stopped"].includes(state.session.state),
  );
  async function add(folder: boolean) {
    const paths = await pick(folder ? "folder" : "multiple");
    if (paths.length) setImportPaths(paths);
  }
  return (
    <>
      <div className="actions">
        <button
          className="primary"
          disabled={busy || !state.capabilities.importContent}
          onClick={() => void add(false)}
        >
          Add game files
        </button>
        <button
          disabled={busy || !state.capabilities.importContent}
          onClick={() => void add(true)}
        >
          Add game folder
        </button>
        <label>
          Show
          <select value={filter} onChange={(e) => setFilter(e.target.value)}>
            <option>All games</option>
            <option>Favourites</option>
          </select>
        </label>
        <label>
          Sort
          <select value={sort} onChange={(e) => setSort(e.target.value)}>
            <option>A–Z</option>
            <option>Z–A</option>
            <option>Recently added</option>
            <option>Recently played</option>
            <option>Most played</option>
          </select>
        </label>
        <label>
          View
          <select
            value={view}
            onChange={(e) => {
              void action("settings.set", {
                key: "library/viewMode",
                value: e.target.value,
              });
            }}
          >
            <option>Focused</option>
            <option>Grid</option>
            <option>Carousel</option>
          </select>
        </label>
      </div>
      <div className="toolbar">
        <span>{games.length} games</span>
        <span>Local collection</span>
      </div>
      {!games.length ? (
        <Empty
          title={
            state.games.length
              ? "No matching games"
              : "Your library starts here"
          }
        >
          {state.mode === "preview"
            ? "Open the native desktop launcher to register local content. Preview never invents installed games."
            : "Add your own game files or folders. Compatibility is supplied by the Xenon content adapter."}
        </Empty>
      ) : (
        <div className={view !== "Focused" ? "library grid-mode" : "library"}>
          {view === "Carousel" ? (
            <GameCarousel
              games={games}
              selected={game?.id}
              select={select}
              wrap={state.settings?.["library/wrapNavigation"] !== false}
            />
          ) : (
            <GameCollection
              scroll={scroll}
              setScroll={(scroll) =>
                setLibraryBrowse((previous) => ({ ...previous, scroll }))
              }
              density={String(
                state.settings?.["library/gridDensity"] ?? "Auto",
              )}
              games={games}
              selected={game?.id}
              grid={view === "Grid"}
              select={select}
              scale={Number(state.settings?.["accessibility/textScale"] ?? 1)}
            />
          )}
          {game && (
            <article className="detail">
              {game.heroArt &&
                state.settings?.["appearance/artworkBackgrounds"] !== false && (
                  <CoverImage
                    source={game.heroArt}
                    label={game.title}
                    className="game-details-hero"
                    x={game.heroArtFocalX}
                    y={game.heroArtFocalY}
                  />
                )}
              <p className="eyebrow">{game.module}</p>
              <h2>{game.title}</h2>
              <p>{game.description}</p>
              <div className="actions">
                <button
                  className="primary"
                  disabled={
                    busy || !state.capabilities.runtime || sessionActive
                  }
                  onClick={() =>
                    void action("session.launch", { gameId: game.id })
                  }
                >
                  {state.session?.fixture ? "Start test session" : "Play"}
                </button>
                <button
                  disabled={busy}
                  onClick={() =>
                    void action("library.favorite", {
                      id: game.id,
                      favorite: !game.favorite,
                    })
                  }
                >
                  {game.favorite ? "Unfavourite" : "Favourite"}
                </button>
                <button onClick={() => setProperties(game)}>Properties</button>
                {game.moduleId &&
                  state.modules?.some((m) => m.id === game.moduleId) && (
                    <button
                      onClick={() =>
                        setModuleSettings(
                          state.modules?.find((m) => m.id === game.moduleId),
                        )
                      }
                    >
                      Module settings
                    </button>
                  )}
              </div>
              <p className="status">
                {game.verified
                  ? "Content verified"
                  : "Compatibility not verified"}{" "}
                · Runtime{" "}
                {state.capabilities.runtime ? "available" : "not connected"}
              </p>
              <dl>
                <dt>Source</dt>
                <dd className="path">{game.contentPath || "Not registered"}</dd>
                <dt>Module</dt>
                <dd>{game.module}</dd>
              </dl>
              <GameAddons game={game} />
            </article>
          )}
        </div>
      )}
      {importPaths.length > 0 && (
        <Modal
          title="Add games to the library"
          close={() => setImportPaths([])}
        >
          <p>
            {importPaths.length} source{importPaths.length === 1 ? "" : "s"}{" "}
            selected.
          </p>
          <p>
            Keep the current location, or move the files into your configured
            Games folder. Moves run in Downloads and verify the copy before
            removing unchanged originals.
          </p>
          <div className="actions">
            <button
              disabled={busy}
              onClick={async () => {
                if (
                  await action("library.import", {
                    paths: importPaths,
                    mode: "keep",
                  })
                )
                  setImportPaths([]);
              }}
            >
              Keep current location
            </button>
            <button
              disabled={busy || state.mode !== "native"}
              onClick={async () => {
                if (
                  await action("library.import", {
                    paths: importPaths,
                    mode: "move",
                  })
                )
                  setImportPaths([]);
              }}
            >
              Move into Xenon Library
            </button>
          </div>
        </Modal>
      )}
      {properties && (
        <GameProperties
          game={state.games.find((g) => g.id === properties.id) ?? properties}
          close={() => setProperties(undefined)}
        />
      )}
      {moduleSettings && (
        <ModuleSettings
          module={
            state.modules?.find((m) => m.id === moduleSettings.id) ??
            moduleSettings
          }
          close={() => setModuleSettings(undefined)}
        />
      )}
    </>
  );
}
function GameProperties({ game, close }: { game: Game; close: () => void }) {
  const { state, busy, action } = useLauncher();
  const [mediaDirty, setMediaDirty] = useState(false);
  const [title, setTitle] = useState(game.title);
  const [moduleId, setModule] = useState(game.moduleId ?? "");
  return (
    <Modal
      title="Game properties"
      close={close}
      dirty={
        mediaDirty || title !== game.title || moduleId !== (game.moduleId ?? "")
      }
    >
      <form
        className="form"
        onSubmit={async (e) => {
          e.preventDefault();
          if (mediaDirty) return;
          if (
            await action("library.update", {
              id: game.id,
              title,
              moduleId,
            })
          )
            close();
        }}
      >
        <label>
          Title
          <input
            required
            value={title}
            onChange={(e) => setTitle(e.target.value)}
          />
        </label>
        <label>
          Module
          <select value={moduleId} onChange={(e) => setModule(e.target.value)}>
            <option value="">No module selected</option>
            {state.modules?.map((m) => (
              <option key={m.id} value={m.id}>
                {m.name} · {m.version}
              </option>
            ))}
          </select>
        </label>
        <p>
          Recorded playtime: {playtime(game.totalPlayTimeMs)} ·{" "}
          {game.playCount ?? 0} plays
        </p>
        <GameMedia game={game} reportDirty={setMediaDirty} />
        <p className="path">{game.contentPath}</p>
        <p>
          {game.managedCopies?.length
            ? `${game.managedCopies.length} managed content location(s).`
            : "Files are kept at their original location."}
        </p>
        {game.managedCopies?.map((copy) => (
          <button
            key={copy.root}
            type="button"
            disabled={busy || state.mode !== "native"}
            onClick={() =>
              void action("system.openPath", {
                target: "managed",
                id: game.id,
                copyId: copy.entryId,
                locationId: copy.locationId,
              })
            }
          >
            Open {copy.previous ? "previous" : "managed"}{" "}
            {copy.type === "game" ? "game" : "DLC"} folder
          </button>
        ))}
        <div className="actions">
          <button
            className="primary"
            disabled={busy || state.mode !== "native" || mediaDirty}
          >
            Save
          </button>
          <button
            type="button"
            disabled={busy || state.mode !== "native"}
            onClick={() => void action("library.verify", { id: game.id })}
          >
            Check source path
          </button>
          <button
            type="button"
            disabled={busy || state.mode !== "native" || !game.moduleId}
            onClick={async () => {
              if (await action("library.refreshMetadata", { id: game.id }))
                close();
            }}
          >
            Refresh module metadata
          </button>
          <button
            type="button"
            disabled={busy || !state.runtimeServices?.capabilities.content}
            onClick={() => void action("content.identify", { gameId: game.id })}
          >
            Identify with content adapter
          </button>
          <button
            type="button"
            disabled={busy || !state.runtimeServices?.capabilities.content}
            onClick={() => void action("content.verify", { gameId: game.id })}
          >
            Verify with content adapter
          </button>
          <button
            type="button"
            disabled={busy || Boolean(game.managedCopies?.length)}
            onClick={async () => {
              if (
                window.confirm(
                  `Remove “${game.title}” from the library? Original files are kept.`,
                ) &&
                (await action("library.remove", { id: game.id }))
              )
                close();
            }}
          >
            Remove reference
          </button>
          {!game.managedCopies?.some((copy) => copy.type === "game") &&
            game.contentPath && (
              <button
                type="button"
                disabled={busy || state.mode !== "native"}
                onClick={async () => {
                  if (
                    window.confirm(
                      `Move the files for “${game.title}” into your configured Games folder? Unchanged original files will be removed after verification.`,
                    ) &&
                    (await action("library.transfer", {
                      id: game.id,
                      path: game.contentPath,
                      type: "game",
                      mode: "move",
                    }))
                  )
                    close();
                }}
              >
                Move into Xenon Library
              </button>
            )}
          {Boolean(game.managedCopies?.length) && (
            <button
              type="button"
              disabled={busy || state.mode !== "native"}
              onClick={async () => {
                if (
                  window.confirm(
                    `Permanently delete launcher-managed files for “${game.title}”? Original sources outside the managed library are kept. The library entry will remain.`,
                  ) &&
                  (await action("library.deleteManaged", { id: game.id }))
                )
                  close();
              }}
            >
              Delete managed files
            </button>
          )}
        </div>
        {game.contentReport && (
          <p role="status">
            {game.contentReport.fixture ? "Fixture: " : ""}
            {game.contentReport.message}
          </p>
        )}
      </form>
    </Modal>
  );
}

function Avatar({
  profile,
  large = false,
}: {
  profile: Profile;
  large?: boolean;
}) {
  return (
    <div className={large ? "avatar avatar-large" : "avatar"}>
      <span>{profile.name.slice(0, 2).toUpperCase()}</span>
      {profile.avatarKey && (
        <img
          key={profile.avatarKey}
          src={`xenon://app/avatars/${encodeURIComponent(profile.id)}?v=${encodeURIComponent(profile.avatarKey)}`}
          alt=""
          onError={(e) => {
            e.currentTarget.style.display = "none";
          }}
          style={{
            objectPosition: `${(profile.avatarFocalX ?? 0.5) * 100}% ${(profile.avatarFocalY ?? 0.5) * 100}%`,
            transform: `scale(${profile.avatarZoom ?? 1})`,
          }}
        />
      )}
    </div>
  );
}
export function Profiles() {
  const {
    state,
    query,
    busy,
    action,
    pick,
    navigationIntent,
    clearNavigationIntent,
  } = useLauncher();
  const [editor, setEditor] = useState<Profile | "new">();
  useEffect(() => {
    if (navigationIntent?.page !== "Profiles") return;
    if (navigationIntent.section === "create") setEditor("new");
    if (navigationIntent.targetId) {
      const card = document.getElementById(
        `profile-${navigationIntent.targetId}`,
      );
      card?.scrollIntoView({ block: "center" });
      card?.focus();
    }
    clearNavigationIntent();
  }, [navigationIntent]);
  return (
    <>
      <div className="actions">
        <button className="primary" onClick={() => setEditor("new")}>
          Create profile
        </button>
        <button
          disabled={busy || state.mode !== "native"}
          onClick={async () => {
            const [path] = await pick("open");
            if (path) await action("profiles.import", { path });
          }}
        >
          Import profile
        </button>
      </div>
      <div className="cards">
        {state.profiles
          .filter((p) => p.name.toLowerCase().includes(query.toLowerCase()))
          .map((p) => (
            <article
              className="card"
              key={p.id}
              id={`profile-${p.id}`}
              tabIndex={-1}
            >
              <Avatar profile={p} />
              <h2>{p.name}</h2>
              <p>{p.description || "Local launcher profile"}</p>
              <p className="status">
                {p.id === state.activeProfileId
                  ? "Active profile"
                  : (p.region ?? "Auto (Global)")}
              </p>
              <div className="actions">
                <button
                  disabled={busy || state.mode !== "native"}
                  onClick={async () => {
                    const [path] = await pick("open");
                    if (path)
                      await action("profiles.avatarImport", { id: p.id, path });
                  }}
                >
                  Choose avatar
                </button>
                {p.avatarKey && (
                  <button
                    disabled={busy}
                    onClick={() =>
                      void action("profiles.avatarRemove", { id: p.id })
                    }
                  >
                    Remove avatar
                  </button>
                )}
                <button
                  disabled={busy || p.id === state.activeProfileId}
                  onClick={() => void action("profiles.activate", { id: p.id })}
                >
                  Activate
                </button>
                <button onClick={() => setEditor(p)}>Edit</button>
                <button
                  disabled={busy || state.mode !== "native"}
                  onClick={() =>
                    void action("system.openPath", {
                      target: "profile",
                      id: p.id,
                    })
                  }
                >
                  Open profile storage
                </button>
                <button
                  disabled={busy}
                  onClick={() =>
                    void action("profiles.duplicate", { id: p.id })
                  }
                >
                  Duplicate
                </button>
                <button
                  disabled={busy || state.mode !== "native"}
                  onClick={async () => {
                    const [path] = await pick("save");
                    if (path)
                      await action("profiles.export", { id: p.id, path });
                  }}
                >
                  Export
                </button>
                <button
                  disabled={busy || p.id === state.activeProfileId}
                  onClick={() => {
                    if (window.confirm(`Remove profile “${p.name}”?`))
                      void action("profiles.remove", { id: p.id });
                  }}
                >
                  Remove
                </button>
              </div>
            </article>
          ))}
      </div>
      {editor && (
        <ProfileEditor profile={editor} close={() => setEditor(undefined)} />
      )}
    </>
  );
}
function ProfileEditor({
  profile,
  close,
}: {
  profile: Profile | "new";
  close: () => void;
}) {
  const { state, busy, action, pick } = useLauncher();
  const original: Profile =
    profile === "new"
      ? {
          id: "",
          name: "",
          description: "",
          region: "Auto (Global)",
          offline: true,
        }
      : profile;
  const [draft, setDraft] = useState(original);
  const [changed, change] = useState(false);
  function field(key: keyof Profile, value: string | boolean | number) {
    setDraft({ ...draft, [key]: value });
    change(true);
  }
  return (
    <Modal
      title={profile === "new" ? "Create profile" : "Edit profile"}
      close={close}
      dirty={changed}
    >
      <form
        className="form"
        onSubmit={async (e) => {
          e.preventDefault();
          const ok = await action(
            profile === "new" ? "profiles.create" : "profiles.update",
            { ...draft },
          );
          if (ok) close();
        }}
      >
        <label>
          Name
          <input
            required
            maxLength={profileDefinitions.nameLimit}
            value={draft.name}
            onChange={(e) => field("name", e.target.value)}
          />
        </label>
        <label>
          Description
          <textarea
            maxLength={profileDefinitions.descriptionLimit}
            value={draft.description}
            onChange={(e) => field("description", e.target.value)}
          />
        </label>
        {draft.avatarKey && (
          <section className="avatar-editor">
            <Avatar profile={draft} large />
            {(["avatarFocalX", "avatarFocalY", "avatarZoom"] as const).map(
              (key) => (
                <label key={key}>
                  {key === "avatarFocalX"
                    ? "Horizontal position"
                    : key === "avatarFocalY"
                      ? "Vertical position"
                      : "Zoom"}
                  <input
                    type="range"
                    min={key === "avatarZoom" ? 1 : 0}
                    max={
                      key === "avatarZoom"
                        ? profileDefinitions.avatarZoomMax
                        : 1
                    }
                    step=".01"
                    value={draft[key] ?? (key === "avatarZoom" ? 1 : 0.5)}
                    onChange={(e) => field(key, Number(e.target.value))}
                  />
                </label>
              ),
            )}
          </section>
        )}
        <label>
          Region
          <select
            value={draft.region ?? "Auto (Global)"}
            onChange={(e) => field("region", e.target.value)}
          >
            {profileDefinitions.regions.map((r) => (
              <option key={r}>{r}</option>
            ))}
          </select>
        </label>
        <label className="row">
          Offline mode
          <input
            type="checkbox"
            checked={draft.offline ?? true}
            onChange={(e) => field("offline", e.target.checked)}
          />
        </label>
        <label className="row">
          Isolated runtime settings
          <input
            type="checkbox"
            checked={draft.isolatedSettings ?? false}
            onChange={(e) => {
              const isolated = e.target.checked;
              setDraft({
                ...draft,
                isolatedSettings: isolated,
                runtimeOverrides:
                  isolated && !Object.keys(draft.runtimeOverrides ?? {}).length
                    ? Object.fromEntries(
                        profileRuntime.map((definition) => [
                          definition.key,
                          state.settings?.[definition.key] ??
                            definition.default,
                        ]),
                      )
                    : (draft.runtimeOverrides ?? {}),
              });
              change(true);
            }}
          />
        </label>
        <label>
          Startup page
          <select
            value={draft.startupPage ?? "Launcher default"}
            onChange={(event) => field("startupPage", event.target.value)}
          >
            {profileDefinitions.startupPages.map((page) => (
              <option key={page}>{page}</option>
            ))}
          </select>
        </label>
        <fieldset
          className="profile-runtime"
          disabled={busy || !draft.isolatedSettings}
        >
          <legend>Profile runtime settings</legend>
          {profileRuntime.map((definition) => {
            const value =
              draft.runtimeOverrides?.[definition.key] ??
              state.settings?.[definition.key] ??
              definition.default;
            const update = (value: unknown) => {
              setDraft({
                ...draft,
                runtimeOverrides: {
                  ...draft.runtimeOverrides,
                  [definition.key]: value,
                },
              });
              change(true);
            };
            const range = definition as { minimum?: number; maximum?: number };
            return (
              <label key={definition.key}>
                <span>
                  {definition.label}
                  <small>{definition.description}</small>
                </span>
                {definition.type === "bool" ? (
                  <input
                    aria-label={definition.label}
                    type="checkbox"
                    checked={Boolean(value)}
                    onChange={(event) => update(event.target.checked)}
                  />
                ) : definition.type === "enum" ? (
                  <select
                    aria-label={definition.label}
                    value={String(value)}
                    onChange={(event) => update(event.target.value)}
                  >
                    {definition.options.map((option) => (
                      <option key={String(option.value)} value={option.value}>
                        {option.label}
                      </option>
                    ))}
                  </select>
                ) : (
                  <>
                    <input
                      aria-label={definition.label}
                      type="range"
                      min={range.minimum ?? 0}
                      max={range.maximum ?? 1}
                      step=".01"
                      value={Number(value)}
                      onChange={(event) => update(Number(event.target.value))}
                    />
                    <output>{Math.round(Number(value) * 100)}%</output>
                  </>
                )}
              </label>
            );
          })}
        </fieldset>
        <details>
          <summary>Content path overrides</summary>
          {(["gamePath", "savePath", "screenshotPath"] as const).map((key) => (
            <label key={key}>
              {key.replace("Path", " folder")}
              <input
                value={draft[key] ?? ""}
                placeholder="Use launcher default"
                onChange={(e) => field(key, e.target.value)}
              />
              <button
                type="button"
                disabled={busy || state.mode !== "native"}
                onClick={async () => {
                  const [path] = await pick("folder");
                  if (path) field(key, path);
                }}
              >
                Browse {key.replace("Path", " folder")}
              </button>
            </label>
          ))}
        </details>
        <button className="primary" disabled={busy}>
          Save profile
        </button>
      </form>
    </Modal>
  );
}

export function Modules() {
  const {
    state,
    busy,
    query,
    action,
    pick,
    navigationIntent,
    clearNavigationIntent,
  } = useLauncher();
  const [settings, setSettings] = useState<Module>();
  const [catalog, setCatalog] = useState(false);
  const [updateHistory, setUpdateHistory] = useState<string>();
  useEffect(() => {
    if (navigationIntent?.page !== "Modules") return;
    if (navigationIntent.section === "catalog") setCatalog(true);
    if (navigationIntent.targetId) {
      const card = document.getElementById(
        `module-${navigationIntent.targetId}`,
      );
      card?.scrollIntoView({ block: "center" });
      card?.focus();
    }
    clearNavigationIntent();
  }, [navigationIntent]);
  const modules =
    state.modules?.filter((m) =>
      m.name.toLowerCase().includes(query.toLowerCase()),
    ) ?? [];
  return (
    <>
      <div className="actions">
        <button
          className="primary"
          disabled={busy || !state.capabilities.modules}
          onClick={async () => {
            const [path] = await pick("folder");
            if (path) await action("modules.import", { path });
          }}
        >
          Register module folder
        </button>
        <button
          disabled={busy || !state.capabilities.modules}
          onClick={async () => {
            const [path] = await pick("open");
            if (path) await action("modules.installPackage", { path });
          }}
        >
          Install module ZIP
        </button>
        <button onClick={() => setCatalog(true)}>Module catalogue</button>
        <button
          disabled={busy || !state.capabilities.modules}
          onClick={() => void action("modules.rescan")}
        >
          Rescan installed modules
        </button>
      </div>
      {!modules.length && (
        <Empty title="No modules registered">
          Register an extracted module folder containing a Xenon module
          manifest. Game content is imported separately.
        </Empty>
      )}
      <div className="cards">
        {modules.map((m) => (
          <article
            key={m.id}
            className="card"
            id={`module-${m.id}`}
            tabIndex={-1}
          >
            <p className="eyebrow">{m.enabled ? "ENABLED" : "DISABLED"}</p>
            <h2>{m.name}</h2>
            <p>Version {m.version}</p>
            <p className="path">{m.path}</p>
            <div className="actions">
              <button
                disabled={busy}
                onClick={() =>
                  void action("modules.enable", {
                    id: m.id,
                    enabled: !m.enabled,
                  })
                }
              >
                {m.enabled ? "Disable" : "Enable"}
              </button>
              <button onClick={() => setSettings(m)}>Settings</button>
              <button
                disabled={
                  busy || !state.games.some((game) => game.moduleId === m.id)
                }
                onClick={() => {
                  if (
                    window.confirm(
                      `Unlink games from ${m.name}? The module and all game files will be kept.`,
                    )
                  )
                    void action("modules.unlinkGames", { id: m.id });
                }}
              >
                Unlink games
              </button>
              <button onClick={() => setUpdateHistory(m.id)}>
                Update history
              </button>
              <button
                disabled={busy || !m.previousVersions?.length}
                onClick={() => {
                  if (
                    window.confirm(
                      `Roll back ${m.name} to its previous installed version?`,
                    )
                  )
                    void action("modules.rollback", { id: m.id });
                }}
              >
                Roll back
              </button>
              <button
                disabled={busy}
                onClick={() =>
                  void action("system.openPath", { target: "module", id: m.id })
                }
              >
                Open folder
              </button>
              <button
                disabled={busy}
                onClick={() => void action("modules.verify", { id: m.id })}
              >
                Check manifest
              </button>
              <button
                disabled={busy}
                onClick={() => {
                  if (
                    window.confirm(
                      `Unregister “${m.name}”? The original module files are kept.`,
                    )
                  )
                    void action("modules.remove", { id: m.id });
                }}
              >
                Unregister
              </button>
            </div>
            <UpdateControls id={m.id} name={m.name} />
          </article>
        ))}
      </div>
      {settings && (
        <ModuleSettings
          module={state.modules?.find((m) => m.id === settings.id) ?? settings}
          close={() => setSettings(undefined)}
        />
      )}{" "}
      {catalog && <ModuleCatalogue close={() => setCatalog(false)} />}
      {updateHistory && (
        <UpdateHistory
          id={updateHistory}
          close={() => setUpdateHistory(undefined)}
        />
      )}
    </>
  );
}
function ModuleSettings({
  module,
  close,
}: {
  module: Module;
  close: () => void;
}) {
  const { busy, action } = useLauncher();
  return (
    <Modal title={`${module.name} settings`} close={close}>
      {!module.schema.length && <p>This manifest does not declare settings.</p>}
      <div className="settings">
        {module.schema.map((d) => {
          const value = module.settings[d.id] ?? d.default ?? "";
          const options = d.options?.map((option) =>
            typeof option === "object"
              ? option
              : { label: String(option), value: option },
          );
          return (
            <label key={d.id}>
              <span>
                {d.label ?? d.id}
                <small>{d.description}</small>
              </span>
              {d.type === "bool" || d.type === "boolean" ? (
                <input
                  type="checkbox"
                  disabled={busy}
                  checked={Boolean(value)}
                  onChange={(e) =>
                    void action("modules.setting", {
                      id: module.id,
                      key: d.id,
                      value: e.target.checked,
                    })
                  }
                />
              ) : options?.length ? (
                <select
                  disabled={busy}
                  value={options.findIndex((option) => option.value === value)}
                  onChange={(e) =>
                    void action("modules.setting", {
                      id: module.id,
                      key: d.id,
                      value: options[Number(e.target.value)].value,
                    })
                  }
                >
                  {options.map((option, index) => (
                    <option key={index} value={index}>
                      {option.label}
                    </option>
                  ))}
                </select>
              ) : (
                <input
                  key={JSON.stringify(value)}
                  disabled={busy}
                  defaultValue={String(value)}
                  type={d.type === "number" ? "number" : "text"}
                  min={d.minimum}
                  max={d.maximum}
                  step="any"
                  onBlur={async (e) => {
                    const input = e.currentTarget;
                    if (
                      d.type === "number" &&
                      (!input.value.trim() || !input.checkValidity())
                    ) {
                      input.reportValidity();
                      input.value = String(value);
                      return;
                    }
                    if (String(value) !== input.value) {
                      const saved = await action("modules.setting", {
                        id: module.id,
                        key: d.id,
                        value:
                          d.type === "number"
                            ? Number(input.value)
                            : input.value,
                      });
                      if (!saved) input.value = String(value);
                    }
                  }}
                />
              )}
            </label>
          );
        })}
      </div>
      <button
        disabled={busy || !module.schema.length}
        onClick={() => {
          if (window.confirm("Reset this module to its declared defaults?"))
            void action("modules.resetSettings", { id: module.id });
        }}
      >
        Reset module settings
      </button>
    </Modal>
  );
}

const categories = [
  "general",
  "appearance",
  "library",
  "paths",
  "runtime",
  "graphics",
  "input",
  "audio",
  "network",
  "filesystem",
  "updates",
  "community",
  "accessibility",
  "system",
  "developer",
  "about",
];
export function Settings() {
  const {
    state,
    busy,
    query,
    action,
    pick,
    navigationIntent,
    clearNavigationIntent,
  } = useLauncher();
  const [category, setCategory] = useState("general");
  useEffect(() => {
    if (navigationIntent?.page !== "Settings") return;
    if (
      navigationIntent.targetId &&
      categories.includes(navigationIntent.targetId)
    )
      setCategory(navigationIntent.targetId);
    clearNavigationIntent();
  }, [navigationIntent]);
  const visible = definitions.filter(
    (d) =>
      d.category !== "internal" &&
      (query ||
        category !== "appearance" ||
        (![
          "ui/theme",
          "ui/accent",
          "ui/corners",
          "ui/customAccent",
          "appearance/themeBackdrop",
          "appearance/backdropIntensity",
          "appearance/panelOpacity",
          "appearance/decorLevel",
        ].includes(d.key) &&
          !d.key.startsWith("appearance/backdropVariant/"))) &&
      (query
        ? `${d.label} ${d.key}`.toLowerCase().includes(query.toLowerCase())
        : d.category === category),
  );
  return (
    <div className="settings-layout">
      <nav aria-label="Settings categories" className="category-nav">
        {categories.map((c) => (
          <button
            key={c}
            aria-current={category === c ? "page" : undefined}
            onClick={() => setCategory(c)}
          >
            {c[0].toUpperCase() + c.slice(1)}
          </button>
        ))}
      </nav>
      <section className="card settings">
        <div className="heading">
          <h2>
            {query
              ? "Search results"
              : category[0].toUpperCase() + category.slice(1)}
          </h2>
          <button
            disabled={busy}
            onClick={() => {
              if (window.confirm("Reset these settings to defaults?"))
                void action("settings.reset", {
                  category: query ? "" : category,
                });
            }}
          >
            Reset
          </button>
        </div>
        {category === "appearance" && !query && <AppearanceSettings />}
        {category === "general" && !query && <LegacyMigration />}
        {category === "input" && !query && <InputSettings />}
        {visible.map((d) => {
          const value = state.settings?.[d.key] ?? d.default;
          const definition = d as typeof d & {
            minimum?: number;
            maximum?: number;
          };
          return (
            <div className="setting-row" key={d.key}>
              <label>
                <span>
                  {d.label}
                  <small>{d.key}</small>
                </span>
                {d.type === "bool" ? (
                  <input
                    type="checkbox"
                    disabled={busy || d.key === "community/discordRichPresence"}
                    checked={Boolean(value)}
                    onChange={(e) =>
                      void action("settings.set", {
                        key: d.key,
                        value: e.target.checked,
                      })
                    }
                  />
                ) : d.options.length ? (
                  <select
                    value={JSON.stringify(value)}
                    disabled={busy}
                    onChange={(e) =>
                      void action("settings.set", {
                        key: d.key,
                        value: JSON.parse(e.target.value),
                      })
                    }
                  >
                    {d.options.map((o) => (
                      <option
                        key={JSON.stringify(o.value)}
                        value={JSON.stringify(o.value)}
                      >
                        {o.label}
                      </option>
                    ))}
                  </select>
                ) : (
                  <input
                    key={`${d.key}-${JSON.stringify(value)}`}
                    type={d.type === "number" ? "number" : "text"}
                    defaultValue={String(value)}
                    min={definition.minimum}
                    max={definition.maximum}
                    step={d.type === "number" ? "any" : undefined}
                    disabled={busy}
                    onBlur={(e) => {
                      const edited =
                        d.type === "number"
                          ? Number(e.target.value)
                          : e.target.value;
                      if (edited !== value)
                        void action("settings.set", {
                          key: d.key,
                          value: edited,
                        });
                    }}
                  />
                )}
              </label>
              <button
                type="button"
                aria-label={`Reset ${d.label}`}
                disabled={busy || state.settings?.[d.key] === undefined}
                onClick={() => void action("settings.resetKey", { key: d.key })}
              >
                Reset
              </button>
            </div>
          );
        })}
        {category === "paths" &&
          !query &&
          ["games", "saves", "profiles", "modules", "screenshots", "cache"].map(
            (key) => (
              <label key={key}>
                <span>
                  {key[0].toUpperCase() + key.slice(1)}
                  <small className="path">
                    {String(
                      state.settings?.[`paths/${key}`] ?? "Launcher default",
                    )}
                  </small>
                </span>
                <button
                  disabled={busy || state.mode !== "native"}
                  onClick={async () => {
                    const [path] = await pick("folder");
                    if (path)
                      await action("settings.set", {
                        key: `paths/${key}`,
                        value: path,
                      });
                  }}
                >
                  Choose folder
                </button>
              </label>
            ),
          )}
        {category === "about" && (
          <p>
            Xenon Launcher Next · CEF + React · protocol v1. Qt-free conversion
            in progress.
          </p>
        )}
        {[
          "runtime",
          "graphics",
          "input",
          "audio",
          "network",
          "filesystem",
        ].includes(category) && (
          <p>
            These preferences are saved for the future runtime adapter. Changing
            them does not activate Xenon services.
          </p>
        )}
        {category === "input" && (
          <p>
            Controller navigation works while the launcher has focus. Runtime
            device routing, profiles and rumble require the input provider.
          </p>
        )}
        {category === "updates" && <UpdatesPanel />}
        {category === "community" && (
          <>
            <p>
              Discord Rich Presence is unavailable in this build. Enabling it
              will require an available provider and a new explicit opt-in.
            </p>
            <div className="actions">
              <button
                disabled={busy || state.mode !== "native"}
                onClick={() =>
                  void action("system.openUrl", {
                    url: "https://discord.gg/zVGw3HADgA",
                  })
                }
              >
                Join the Xenon Discord
              </button>
              <button
                disabled={busy || state.mode !== "native"}
                onClick={() =>
                  void action("system.openUrl", {
                    url: "https://github.com/nimauria/Xenon-Recomp",
                  })
                }
              >
                Open project community
              </button>
            </div>
          </>
        )}
      </section>
    </div>
  );
}

function HomeShelf({ title, games }: { title: string; games: Game[] }) {
  const { openGame } = useLauncher();
  if (!games.length) return null;
  return (
    <section className="home-shelf" aria-label={title}>
      <h2>{title}</h2>
      <div className="home-games">
        {games.slice(0, 6).map((game) => (
          <button
            key={game.id}
            aria-label={`Open ${game.title}`}
            onClick={() => openGame(game.id)}
          >
            <CoverImage
              source={game.tileArt}
              label={game.title}
              className="home-cover"
              x={game.tileArtFocalX}
              y={game.tileArtFocalY}
            />
            <strong>{game.title}</strong>
            <small>{game.module}</small>
          </button>
        ))}
      </div>
    </section>
  );
}
function timestamp(value: string) {
  const date = new Date(/^\d+$/.test(value) ? Number(value) : value);
  return Number.isNaN(date.getTime())
    ? "Time unavailable"
    : date.toLocaleString();
}
export function Activity() {
  const { state, action, busy, navigate, openGame, query } = useLauncher();
  const games = state.games.filter((game) =>
    game.title.toLowerCase().includes(query.toLowerCase()),
  );
  const recent = [...games].sort(
    (a, b) => activityTime(b.addedAt) - activityTime(a.addedAt),
  );
  const sessions =
    state.history
      ?.filter((row) => row.title.toLowerCase().includes(query.toLowerCase()))
      .slice(-10)
      .reverse() ?? [];
  return (
    <>
      <section className="card">
        <p className="eyebrow">WELCOME BACK</p>
        <h2>
          {state.profiles.find((p) => p.id === state.activeProfileId)?.name}
        </h2>
        <p>
          {state.games.length} local games · {state.modules?.length ?? 0}{" "}
          registered modules
        </p>
        <button className="primary" onClick={() => navigate("Library")}>
          Browse library
        </button>
      </section>
      <div className="home-stats" aria-label="Launcher overview">
        <button onClick={() => navigate("Library")}>
          <strong>{state.games.length}</strong>
          <span>Games</span>
        </button>
        <button onClick={() => navigate("Modules")}>
          <strong>
            {state.modules?.filter((module) => module.enabled).length ?? 0} /{" "}
            {state.modules?.length ?? 0}
          </strong>
          <span>Enabled modules</span>
        </button>
        <button onClick={() => navigate("Captures")}>
          <strong>{state.captures?.length ?? 0}</strong>
          <span>Captures</span>
        </button>
        <div>
          <strong>{state.history?.length ?? 0}</strong>
          <span>History entries</span>
        </div>
        <div>
          <strong>{playtime(state.activitySummary?.totalPlayTimeMs)}</strong>
          <span>Recorded playtime</span>
        </div>
        <div>
          <strong>{state.activitySummary?.playCount ?? 0}</strong>
          <span>Recorded plays</span>
        </div>
      </div>
      <HomeShelf
        title="Favourites"
        games={games.filter((game) => game.favorite)}
      />
      {(state.activitySummary?.fixturePlayCount ?? 0) > 0 && (
        <p>
          Test activity: {state.activitySummary?.fixturePlayCount} sessions ·{" "}
          {playtime(state.activitySummary?.fixturePlayTimeMs)}. Excluded from
          recorded playtime.
        </p>
      )}
      <HomeShelf
        title="Recently played"
        games={[...games]
          .filter((game) => Boolean(game.lastPlayed))
          .sort(
            (a, b) => activityTime(b.lastPlayed) - activityTime(a.lastPlayed),
          )}
      />
      <HomeShelf title="Recently added" games={recent} />
      <div className="heading">
        <h2>Recent sessions</h2>
        <button
          disabled={busy || !state.history?.length}
          onClick={() => {
            if (window.confirm("Clear session history?"))
              void action("history.clear");
          }}
        >
          Clear history
        </button>
      </div>
      {!sessions.length ? (
        <Empty title="No sessions yet">
          {query
            ? "No sessions match this search."
            : "Session history will appear when a runtime adapter supplies it."}
        </Empty>
      ) : (
        sessions.map((h) => (
          <article className="card" key={h.id}>
            <h3>
              {h.title}
              {h.fixture && <span className="badge">Test session</span>}
            </h3>
            <p>
              {h.state} · {timestamp(h.time)} · {playtime(h.durationMs)}
            </p>
            {h.gameId && state.games.some((game) => game.id === h.gameId) && (
              <button onClick={() => openGame(h.gameId!)}>Open game</button>
            )}
          </article>
        ))
      )}
    </>
  );
}
export { Captures } from "./Captures";
export function Downloads() {
  const { state, query, action, busy } = useLauncher();
  const [filter, setFilter] = useState("All");
  const jobs =
    state.jobs
      ?.filter(
        (j) =>
          j.title.toLowerCase().includes(query.toLowerCase()) &&
          (filter === "All" ||
            (filter === "Active" ? isActiveJob(j) : j.state === filter)),
      )
      .slice()
      .reverse() ?? [];
  return (
    <>
      <div className="actions">
        <label>
          Show
          <select value={filter} onChange={(e) => setFilter(e.target.value)}>
            {[
              "All",
              "Active",
              "completed",
              "failed",
              "cancelled",
              "interrupted",
            ].map((f) => (
              <option key={f} value={f}>
                {f[0].toUpperCase() + f.slice(1)}
              </option>
            ))}
          </select>
        </label>
        <button
          disabled={
            busy || !state.jobs?.some((j) => !isActiveJob(j) && !j.cleaningUp)
          }
          onClick={() => {
            if (
              window.confirm(
                "Clear finished operation records? Imported files and current library data are kept.",
              )
            )
              void action("jobs.clearFinished");
          }}
        >
          Clear finished
        </button>
      </div>
      {!jobs.length ? (
        <Empty title="No operations to show">
          Module installations, catalogue refreshes and verified update
          downloads run here in the background.
        </Empty>
      ) : (
        jobs.map((j) => (
          <article
            className="card operation"
            key={j.id}
            data-job-id={j.id}
            data-state={j.state}
            aria-label={j.title}
          >
            <h3>{j.title}</h3>
            <progress
              aria-label={`${j.title} progress`}
              max={100}
              value={j.progress}
            />
            <p>
              <span className="operation-state">
                {j.state === "committing" ? "Finishing operation" : j.state}
              </span>
              {j.state === "running" &&
                j.phase &&
                j.phase.toLowerCase() !== j.state.toLowerCase() && (
                  <> · {j.phase}</>
                )}{" "}
              · {Math.round(j.progress)}%
            </p>
            {isActiveJob(j) && Boolean(j.bytesTotal) && (
              <small>
                {((j.bytesDone ?? 0) / 1048576).toFixed(1)} /{" "}
                {((j.bytesTotal ?? 0) / 1048576).toFixed(1)} MiB in this step
              </small>
            )}
            {j.error && <p>{j.error}</p>}
            {j.warning && <p role="status">{j.warning}</p>}
            {j.cleaningUp && <p role="status">Removing temporary files…</p>}
            {j.cleanupError && (
              <p>Temporary files could not be removed: {j.cleanupError}</p>
            )}
            <div className="actions">
              {j.canCancel && (
                <button
                  disabled={busy}
                  onClick={() => void action("jobs.cancel", { id: j.id })}
                >
                  Cancel
                </button>
              )}
              {j.state === "cancelling" && (
                <span role="status">Cancelling…</span>
              )}
              {j.canRetry && (
                <button
                  disabled={busy}
                  onClick={() => void action("jobs.retry", { id: j.id })}
                >
                  Retry
                </button>
              )}
            </div>
          </article>
        ))
      )}
    </>
  );
}
export function Support() {
  const { state, busy, action } = useLauncher();
  const [copied, setCopied] = useState(false);
  const report = {
    launcher: "Xenon Launcher Next",
    mode: state.mode,
    protocolVersion: 1,
    userAgent: navigator.userAgent,
    runtimeConnected: state.capabilities.runtime,
    counts: {
      games: state.games.length,
      profiles: state.profiles.length,
      modules: state.modules?.length ?? 0,
    },
  };
  return (
    <section className="card">
      <h2>Diagnostics and support</h2>
      <p>
        Support summaries omit profile names, content paths and game files.
        Nothing is uploaded automatically.
      </p>
      <pre>{JSON.stringify(report, null, 2)}</pre>
      <div className="actions">
        <button
          onClick={async () => {
            try {
              await navigator.clipboard.writeText(
                JSON.stringify(report, null, 2),
              );
              setCopied(true);
            } catch {
              setCopied(false);
            }
          }}
        >
          {copied ? "Copied" : "Copy diagnostics"}
        </button>
        <button onClick={() => downloadJson("xenon-diagnostics.json", report)}>
          Export summary
        </button>
        <button
          disabled={busy || state.mode !== "native"}
          onClick={() => void action("support.bundle")}
        >
          Create support ZIP
        </button>
      </div>
      <div className="actions">
        <button
          disabled={busy || state.mode !== "native"}
          onClick={() => void action("system.openPath", { target: "support" })}
        >
          Open support folder
        </button>
        <button
          disabled={busy || state.mode !== "native"}
          onClick={() => void action("system.openPath", { target: "storage" })}
        >
          Open launcher data
        </button>
        <button
          disabled={busy || state.mode !== "native"}
          onClick={() => void action("application.restart", { safeMode: true })}
        >
          Restart in safe mode
        </button>
        {state.recovery?.safeMode && (
          <button
            disabled={busy}
            onClick={() =>
              void action("application.restart", { safeMode: false })
            }
          >
            Restart normally
          </button>
        )}
      </div>
      <p>
        The ZIP includes a filtered diagnostics summary and bounded
        launcher/runtime log excerpts. Known local paths, profile names,
        email-shaped strings and credential-bearing log lines are filtered.
        Review the files before sharing module output.
      </p>
    </section>
  );
}
export { RuntimeServices as RuntimeTools } from "./RuntimeServices";

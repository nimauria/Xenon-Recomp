import { useMemo, useState } from "react";
import type { Game, Module, Page, Profile } from "../../contracts/launcher";
import { GameCollection } from "./GameCollection";
import { GameCarousel } from "./GameCarousel";
import { CoverImage } from "./CoverImage";
import { AppearanceSettings } from "./AppearanceSettings";
import definitions from "../../contracts/settings.json";
import { Empty, Modal, downloadJson, useLauncher } from "./context";

export function Library() {
  const { state, busy, query, action, pick } = useLauncher();
  const [selected, select] = useState(
    String(state.settings?.["library/lastSelectedGameId"] ?? ""),
  );
  const [filter, setFilter] = useState("All games");
  const [sort, setSort] = useState("A–Z");
  const [properties, setProperties] = useState<Game>();
  const [moduleSettings, setModuleSettings] = useState<Module>();
  const [view, setView] = useState(
    String(state.settings?.["library/viewMode"] ?? "Focused"),
  );
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
              : Number(b.addedAt ?? 0) - Number(a.addedAt ?? 0),
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
    if (paths.length) await action("library.import", { paths });
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
          </select>
        </label>
        <label>
          View
          <select
            value={view}
            onChange={(e) => {
              setView(e.target.value);
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
              <h3>Add-ons</h3>
              <div className="dlc-list">
                {!game.dlc?.length && <p>No add-ons registered.</p>}
                {game.dlc?.map((d) => (
                  <div className="row" key={d.id}>
                    <span>
                      {d.name}
                      <small>{d.verified ? "Verified" : "Not verified"}</small>
                    </span>
                    <button
                      disabled={busy}
                      onClick={() => {
                        if (
                          window.confirm(
                            `Remove add-on reference “${d.name}”? Original files are kept.`,
                          )
                        )
                          void action("dlc.remove", {
                            id: game.id,
                            dlcId: d.id,
                          });
                      }}
                    >
                      Remove
                    </button>
                  </div>
                ))}
              </div>
              <button
                disabled={busy || state.mode !== "native"}
                onClick={async () => {
                  const paths = await pick("multiple");
                  if (paths.length)
                    await action("dlc.import", { id: game.id, paths });
                }}
              >
                Add local DLC
              </button>
            </article>
          )}
        </div>
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
  const [title, setTitle] = useState(game.title);
  const [moduleId, setModule] = useState(game.moduleId ?? "");
  return (
    <Modal
      title="Game properties"
      close={close}
      dirty={title !== game.title || moduleId !== (game.moduleId ?? "")}
    >
      <form
        className="form"
        onSubmit={async (e) => {
          e.preventDefault();
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
        <p className="path">{game.contentPath}</p>
        <div className="actions">
          <button
            className="primary"
            disabled={busy || state.mode !== "native"}
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
            disabled={busy}
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
        </div>
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
  const { state, query, busy, action, pick } = useLauncher();
  const [editor, setEditor] = useState<Profile | "new">();
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
            <article className="card" key={p.id}>
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
  const { busy, action } = useLauncher();
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
            maxLength={40}
            value={draft.name}
            onChange={(e) => field("name", e.target.value)}
          />
        </label>
        <label>
          Description
          <textarea
            maxLength={500}
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
                    max={key === "avatarZoom" ? 4 : 1}
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
            {["Auto (Global)", "North America", "Europe", "Japan", "Asia"].map(
              (r) => (
                <option key={r}>{r}</option>
              ),
            )}
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
            onChange={(e) => field("isolatedSettings", e.target.checked)}
          />
        </label>
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
  const { state, busy, query, action, pick } = useLauncher();
  const [settings, setSettings] = useState<Module>();
  const [catalog, setCatalog] = useState(false);
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
      </div>
      {!modules.length && (
        <Empty title="No modules registered">
          Register an extracted module folder containing a Xenon module
          manifest. Game content is imported separately.
        </Empty>
      )}
      <div className="cards">
        {modules.map((m) => (
          <article key={m.id} className="card">
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
          </article>
        ))}
      </div>
      {settings && (
        <ModuleSettings
          module={state.modules?.find((m) => m.id === settings.id) ?? settings}
          close={() => setSettings(undefined)}
        />
      )}{" "}
      {catalog && (
        <Modal title="Module catalogue" close={() => setCatalog(false)}>
          <Empty title="Catalogue provider not connected">
            Online module discovery and downloads are not connected yet. Local
            module ZIP installation and rollback are available from the Modules
            page.
          </Empty>
        </Modal>
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
              ) : d.options?.length ? (
                <select
                  disabled={busy}
                  value={String(value)}
                  onChange={(e) =>
                    void action("modules.setting", {
                      id: module.id,
                      key: d.id,
                      value: e.target.value,
                    })
                  }
                >
                  {d.options.map((o) => (
                    <option
                      key={typeof o === "string" ? o : o.value}
                      value={typeof o === "string" ? o : o.value}
                    >
                      {typeof o === "string" ? o : o.label}
                    </option>
                  ))}
                </select>
              ) : (
                <input
                  disabled={busy}
                  defaultValue={String(value)}
                  type={d.type === "number" ? "number" : "text"}
                  onBlur={(e) => {
                    if (String(value) !== e.target.value)
                      void action("modules.setting", {
                        id: module.id,
                        key: d.id,
                        value:
                          d.type === "number"
                            ? Number(e.target.value)
                            : e.target.value,
                      });
                  }}
                />
              )}
            </label>
          );
        })}
      </div>
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
  const { state, busy, query, action, pick } = useLauncher();
  const [category, setCategory] = useState("general");
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
        {visible.map((d) => {
          const value = state.settings?.[d.key] ?? d.default;
          const definition = d as typeof d & {
            minimum?: number;
            maximum?: number;
          };
          return (
            <label key={d.key}>
              <span>
                {d.label}
                <small>{d.key}</small>
              </span>
              {d.type === "bool" ? (
                <input
                  type="checkbox"
                  disabled={busy}
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
        {category === "updates" && (
          <p>
            Update preferences are stored. The update provider and installer are
            not connected yet.
          </p>
        )}
        {category === "community" && (
          <p>Discord presence is not connected yet.</p>
        )}
      </section>
    </div>
  );
}

export function Activity() {
  const { state, action, busy, navigate } = useLauncher();
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
      {!state.history?.length ? (
        <Empty title="No sessions yet">
          Real session history will appear when a runtime adapter supplies it.
        </Empty>
      ) : (
        state.history
          .slice()
          .reverse()
          .map((h) => (
            <article className="card" key={h.id}>
              <h3>{h.title}</h3>
              <p>
                {h.state} · {h.time}
              </p>
            </article>
          ))
      )}
    </>
  );
}
export function Captures() {
  const { state, busy, query, action } = useLauncher();
  const [selected, setSelected] = useState("");
  const captures =
    state.captures?.filter((c) =>
      c.name.toLowerCase().includes(query.toLowerCase()),
    ) ?? [];
  return (
    <>
      <p>
        Images from your configured screenshots folder. Original files stay on
        your machine.
      </p>
      {!captures.length ? (
        <Empty title="No captures found">
          Choose a screenshots folder in Settings → Paths, then refresh the
          page.
        </Empty>
      ) : (
        <div className="cards">
          {captures.map((c) => (
            <article className="card" key={c.id}>
              <h3>{c.name}</h3>
              <p>{Math.ceil(c.size / 1024)} KiB</p>
              <button onClick={() => setSelected(c.id)}>View</button>
              <button
                disabled={busy}
                onClick={() => {
                  if (window.confirm(`Permanently delete “${c.name}”?`))
                    void action("captures.delete", { id: c.id });
                }}
              >
                Delete
              </button>
            </article>
          ))}
        </div>
      )}
      {selected && (
        <Modal title={selected} close={() => setSelected("")}>
          <img
            className="capture-image"
            src={`xenon://app/captures/${encodeURIComponent(selected)}`}
            alt={selected}
          />
        </Modal>
      )}
    </>
  );
}
export function Downloads() {
  const { state, query } = useLauncher();
  const [filter, setFilter] = useState("All");
  const jobs =
    state.jobs?.filter(
      (j) =>
        j.title.toLowerCase().includes(query.toLowerCase()) &&
        (filter === "All" || j.state === filter),
    ) ?? [];
  return (
    <>
      <label>
        Show
        <select value={filter} onChange={(e) => setFilter(e.target.value)}>
          {["All", "running", "completed", "failed", "cancelled"].map((f) => (
            <option key={f}>{f}</option>
          ))}
        </select>
      </label>
      {!jobs.length ? (
        <Empty title="No operations to show">
          Local module installs will appear here after completion. Online update
          downloads and cancellable background jobs are still being converted.
        </Empty>
      ) : (
        jobs.map((j) => (
          <article className="card" key={j.id}>
            <h3>{j.title}</h3>
            <progress max={100} value={j.progress} />
            <p>
              {j.state} · {j.progress}%
            </p>
            {j.error && <p role="alert">{j.error}</p>}
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
        The native ZIP includes a privacy-filtered summary. Full sanitized log
        collection remains to be converted.
      </p>
    </section>
  );
}
export function RuntimeTools({ page }: { page: Page }) {
  const { state } = useLauncher();
  return (
    <section className="card">
      <h2>
        {page === "Developer" ? "Developer diagnostics" : `${page} service`}
      </h2>
      <p>
        Runtime adapter:{" "}
        {state.capabilities.runtime ? "available" : "not connected"}
      </p>
      {page === "Developer" ? (
        <pre>
          {JSON.stringify(
            {
              revision: state.revision,
              mode: state.mode,
              protocolVersion: 1,
              capabilities: state.capabilities,
            },
            null,
            2,
          )}
        </pre>
      ) : (
        <Empty title="Runtime service unavailable">
          The corresponding Xenon service must provide live state before these
          controls can be enabled.
        </Empty>
      )}
    </section>
  );
}

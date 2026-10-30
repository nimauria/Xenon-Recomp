import { t, tText, tCount, language } from "../localization";
import { LanguageSettings } from "./LanguageSettings";
import { ActionMenu, type MenuRequest } from "./ActionMenu";
import { gamePolicy, profilePolicy } from "../policies";
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
import { StorageSettings } from "./StorageSettings";
import { profileNameKey, trimProfileName } from "../profileNames";
import { featureEnabled } from "../features";
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
  const [gameMenu, setGameMenu] = useState<MenuRequest>();
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
  const selectedModule = state.modules?.find(
    (module) => module.id === game?.moduleId,
  );
  const launchProblem =
    selectedModule?.enabled === false
      ? "The selected module is disabled."
      : selectedModule?.compatibility?.issues[0];
  const policy = game ? gamePolicy(state, game, busy) : undefined;
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
          {t("ui.8166f48e1aad")}
        </button>
        <button
          disabled={busy || !state.capabilities.importContent}
          onClick={() => void add(true)}
        >
          {t("ui.68cbb049cb15")}
        </button>
        <label>
          {t("ui.0df6f1cad36c")}
          <select value={filter} onChange={(e) => setFilter(e.target.value)}>
            <option value="All games">{t("ui.cf2bf152b629")}</option>
            <option value="Favourites">{t("ui.d97d51d37092")}</option>
          </select>
        </label>
        <label>
          {t("ui.bec69036aa27")}
          <select value={sort} onChange={(e) => setSort(e.target.value)}>
            <option value="A–Z">{t("ui.f9f6c8502eb0")}</option>
            <option value="Z–A">{t("ui.5a5261ed1547")}</option>
            <option value="Recently added">{t("ui.96ec5d338ae3")}</option>
            <option value="Recently played">{t("ui.452ec9f553a3")}</option>
            <option value="Most played">{t("ui.e7f81bd3aee5")}</option>
          </select>
        </label>
        <label>
          {t("ui.955303856d4b")}
          <select
            value={view}
            onChange={(e) => {
              void action("settings.set", {
                key: "library/viewMode",
                value: e.target.value,
              });
            }}
          >
            <option value="Focused">{t("ui.4f9a966fb271")}</option>
            <option value="Grid">{t("ui.0d7d12ac624c")}</option>
            <option value="Carousel">{t("ui.37219fbc65a8")}</option>
          </select>
        </label>
      </div>
      <div className="toolbar">
        <span>
          {games.length}
          {t("ui.025de403fc6e")}
        </span>
        <span>{t("ui.373e2ebc8d13")}</span>
      </div>
      {!games.length ? (
        <Empty
          title={
            state.games.length ? t("ui.5f3cf2a9b0f0") : t("ui.01ff37ee32e0")
          }
        >
          {state.mode === "preview"
            ? t("ui.da783618ebbf")
            : t("ui.2b5d1484d8dc")}
        </Empty>
      ) : (
        <div className={view !== "Focused" ? "library grid-mode" : "library"}>
          {view === "Carousel" ? (
            <GameCarousel
              openActions={(_game, source, x, y) =>
                setGameMenu({ source, x, y, token: Date.now() })
              }
              games={games}
              selected={game?.id}
              select={select}
              wrap={state.settings?.["library/wrapNavigation"] !== false}
            />
          ) : (
            <GameCollection
              wrap={state.settings?.["library/wrapNavigation"] !== false}
              openActions={(_game, source, x, y) =>
                setGameMenu({ source, x, y, token: Date.now() })
              }
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
            <article
              className="detail"
              onContextMenu={(event) => {
                event.preventDefault();
                setGameMenu({
                  source: event.target as HTMLElement,
                  x: event.clientX,
                  y: event.clientY,
                  token: Date.now(),
                });
              }}
            >
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
                <ActionMenu
                  label={t("menu.game")}
                  request={gameMenu}
                  items={[
                    {
                      id: "play",
                      label: t("ui.436e61016e26"),
                      disabled: !policy?.launch,
                      run: () =>
                        void action("session.launch", { gameId: game.id }),
                    },
                    {
                      id: "favorite",
                      label: game.favorite
                        ? t("ui.4a045e8cae1b")
                        : t("ui.e39b24993c31"),
                      disabled: !policy?.favorite,
                      run: () =>
                        void action("library.favorite", {
                          id: game.id,
                          favorite: !game.favorite,
                        }),
                    },
                    {
                      id: "properties",
                      label: t("ui.ae43692b2a31"),
                      run: () => setProperties(game),
                    },
                    {
                      id: "module",
                      label: t("ui.f547b8100033"),
                      disabled: !policy?.moduleSettings,
                      run: () =>
                        setModuleSettings(
                          state.modules?.find(
                            (module) => module.id === game.moduleId,
                          ),
                        ),
                    },
                    {
                      id: "open",
                      label: t("ui.6a9084025c4b"),
                      disabled: !policy?.openFiles,
                      run: () =>
                        void action("system.openPath", {
                          target: "game",
                          id: game.id,
                        }),
                    },
                    {
                      id: "remove",
                      label: t("ui.cbeda6a6baa8"),
                      disabled: !policy?.remove,
                      run: () => {
                        if (
                          window.confirm(
                            t("ui.bf226efd7a72", { arg0: game.title }),
                          )
                        )
                          void action("library.remove", { id: game.id });
                      },
                    },
                  ]}
                />
                <button
                  className="primary"
                  disabled={!policy?.launch}
                  onClick={() =>
                    void action("session.launch", { gameId: game.id })
                  }
                >
                  {state.session?.fixture
                    ? t("ui.e8ac9fd53af2")
                    : t("ui.436e61016e26")}
                </button>
                <button
                  disabled={!policy?.favorite}
                  data-controller-secondary="true"
                  onClick={() =>
                    void action("library.favorite", {
                      id: game.id,
                      favorite: !game.favorite,
                    })
                  }
                >
                  {game.favorite ? t("ui.4a045e8cae1b") : t("ui.e39b24993c31")}
                </button>
                <button onClick={() => setProperties(game)}>
                  {t("ui.ae43692b2a31")}
                </button>
                {game.moduleId &&
                  state.modules?.some((m) => m.id === game.moduleId) && (
                    <button
                      onClick={() =>
                        setModuleSettings(
                          state.modules?.find((m) => m.id === game.moduleId),
                        )
                      }
                    >
                      {t("ui.f547b8100033")}
                    </button>
                  )}
              </div>
              {launchProblem && <p role="alert">{tText(launchProblem)}</p>}
              <p className="status">
                {game.verified ? t("ui.add979b827f6") : t("ui.1c3e01a731db")}{" "}
                {t("ui.787b27403fcd")}{" "}
                {state.capabilities.runtime
                  ? t("ui.ddd9818abacf")
                  : t("ui.98b1024757db")}
              </p>
              <dl>
                <dt>{t("ui.0e570ca6fabe")}</dt>
                <dd className="path">
                  {game.contentPath || t("ui.ca374c23ed92")}
                </dd>
                <dt>{t("ui.f148224ae44f")}</dt>
                <dd>{game.module}</dd>
              </dl>
              <GameAddons game={game} />
            </article>
          )}
        </div>
      )}
      {importPaths.length > 0 && (
        <Modal title={t("ui.28fd126a6360")} close={() => setImportPaths([])}>
          <p>{tCount("library.sourcesSelected", importPaths.length)}</p>
          <p>{t("ui.68753bdacbce")}</p>
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
              {t("ui.afa4fa63b6f3")}
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
              {t("ui.0e965b9b8ada")}
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
      title={t("ui.164931685fcc")}
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
          {t("ui.7e8cd2056da7")}
          <input
            required
            value={title}
            onChange={(e) => setTitle(e.target.value)}
          />
        </label>
        <label>
          {t("ui.f148224ae44f")}
          <select value={moduleId} onChange={(e) => setModule(e.target.value)}>
            <option value="">{t("ui.340956e89cd2")}</option>
            {state.modules?.map((m) => (
              <option key={m.id} value={m.id}>
                {m.name} · {m.version}
              </option>
            ))}
          </select>
        </label>
        <p>
          {t("ui.659b29bd027a")}
          {playtime(game.totalPlayTimeMs)} · {game.playCount ?? 0}
          {t("ui.39392078f6cf")}
        </p>
        <GameMedia game={game} reportDirty={setMediaDirty} />
        <p className="path">{game.contentPath}</p>
        <p>
          {game.managedCopies?.length
            ? t("ui.e29e38106f31", { arg0: game.managedCopies.length })
            : t("ui.869d345092b6")}
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
            {t("ui.b8039d1d54bf")}
            {copy.previous ? t("ui.6da0633528de") : t("ui.7fdfda5f50a4")}{" "}
            {copy.type === "game" ? t("ui.6ca5cab77e70") : t("ui.d3387a15055f")}
            {t("ui.d50fc111aeb5")}
          </button>
        ))}
        <div className="actions">
          <button
            className="primary"
            disabled={busy || state.mode !== "native" || mediaDirty}
          >
            {t("ui.1509f561f241")}
          </button>
          <button
            type="button"
            disabled={busy || state.mode !== "native"}
            onClick={() => void action("library.verify", { id: game.id })}
          >
            {t("ui.8b01a9909440")}
          </button>
          <button
            type="button"
            disabled={
              busy ||
              state.mode !== "native" ||
              !game.moduleId ||
              mediaDirty ||
              title !== game.title ||
              moduleId !== (game.moduleId ?? "")
            }
            onClick={async () => {
              if (await action("library.refreshMetadata", { id: game.id }))
                close();
            }}
          >
            {t("ui.44cc9aff727c")}
          </button>
          <button
            type="button"
            disabled={busy || !state.runtimeServices?.capabilities.content}
            onClick={() => void action("content.identify", { gameId: game.id })}
          >
            {t("ui.7edad939bd08")}
          </button>
          <button
            type="button"
            disabled={busy || !state.runtimeServices?.capabilities.content}
            onClick={() => void action("content.verify", { gameId: game.id })}
          >
            {t("ui.a767a1f76ec6")}
          </button>
          <button
            type="button"
            disabled={busy || Boolean(game.managedCopies?.length)}
            onClick={async () => {
              if (
                window.confirm(t("ui.bf226efd7a72", { arg0: game.title })) &&
                (await action("library.remove", { id: game.id }))
              )
                close();
            }}
          >
            {t("ui.cbeda6a6baa8")}
          </button>
          {!game.managedCopies?.some((copy) => copy.type === "game") &&
            game.contentPath && (
              <button
                type="button"
                disabled={busy || state.mode !== "native"}
                onClick={async () => {
                  if (
                    window.confirm(
                      t("ui.64a512729b94", { arg0: game.title }),
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
                {t("ui.0e965b9b8ada")}
              </button>
            )}
          {Boolean(game.managedCopies?.length) && (
            <button
              type="button"
              disabled={busy || state.mode !== "native"}
              onClick={async () => {
                if (
                  window.confirm(t("ui.1f8c81ef4d48", { arg0: game.title })) &&
                  (await action("library.deleteManaged", { id: game.id }))
                )
                  close();
              }}
            >
              {t("ui.14eec78e7fe2")}
            </button>
          )}
        </div>
        {game.contentReport && (
          <p role="status">
            {game.contentReport.fixture ? t("ui.8fbcafed382a") : ""}
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
  const [profileMenu, setProfileMenu] = useState<{
    id: string;
    request: MenuRequest;
  }>();
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
          {t("ui.61d30d997d13")}
        </button>
        <button
          disabled={busy || state.mode !== "native"}
          onClick={async () => {
            const [path] = await pick("open");
            if (path) await action("profiles.import", { path });
          }}
        >
          {t("ui.dafe05124d75")}
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
              onContextMenu={(event) => {
                event.preventDefault();
                setProfileMenu({
                  id: p.id,
                  request: {
                    source: event.currentTarget,
                    x: event.clientX,
                    y: event.clientY,
                    token: Date.now(),
                  },
                });
              }}
              tabIndex={-1}
            >
              <Avatar profile={p} />
              <h2>{p.name}</h2>
              <p>{p.description || t("ui.11dc260e1d20")}</p>
              <p className="status">
                {p.id === state.activeProfileId
                  ? t("ui.5608b89a8112")
                  : (p.region ?? t("ui.e01773363699"))}
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
                  {t("ui.777794058c73")}
                </button>
                <ActionMenu
                  label={t("menu.profile", { name: p.name })}
                  request={
                    profileMenu?.id === p.id ? profileMenu.request : undefined
                  }
                  items={[
                    {
                      id: "activate",
                      label: t("ui.24433c70eba5"),
                      disabled: !profilePolicy(state, p, busy).activate,
                      run: () => void action("profiles.activate", { id: p.id }),
                    },
                    {
                      id: "edit",
                      label: t("ui.464c4ffd019e"),
                      disabled: !profilePolicy(state, p, busy).edit,
                      run: () => setEditor(p),
                    },
                    {
                      id: "duplicate",
                      label: t("ui.02cdaabfca80"),
                      disabled: !profilePolicy(state, p, busy).duplicate,
                      run: () =>
                        void action("profiles.duplicate", { id: p.id }),
                    },
                    {
                      id: "export",
                      label: t("ui.3664895579f0"),
                      disabled: !profilePolicy(state, p, busy).export,
                      run: () => {
                        void (async () => {
                          const [path] = await pick("save");
                          if (path)
                            await action("profiles.export", { id: p.id, path });
                        })();
                      },
                    },
                    {
                      id: "remove",
                      label: t("ui.c3812fc4acb8"),
                      disabled: !profilePolicy(state, p, busy).remove,
                      run: () => {
                        if (
                          window.confirm(t("ui.3ae2260ff612", { arg0: p.name }))
                        )
                          void action("profiles.remove", { id: p.id });
                      },
                    },
                  ]}
                />
                {p.avatarKey && (
                  <button
                    disabled={busy}
                    onClick={() =>
                      void action("profiles.avatarRemove", { id: p.id })
                    }
                  >
                    {t("ui.5ae2a862d687")}
                  </button>
                )}
                <button
                  disabled={busy || p.id === state.activeProfileId}
                  onClick={() => void action("profiles.activate", { id: p.id })}
                >
                  {t("ui.24433c70eba5")}
                </button>
                <button onClick={() => setEditor(p)}>
                  {t("ui.464c4ffd019e")}
                </button>
                <button
                  disabled={busy || state.mode !== "native"}
                  onClick={() =>
                    void action("system.openPath", {
                      target: "profile",
                      id: p.id,
                    })
                  }
                >
                  {t("ui.4646ea775420")}
                </button>
                <button
                  disabled={busy}
                  onClick={() =>
                    void action("profiles.duplicate", { id: p.id })
                  }
                >
                  {t("ui.02cdaabfca80")}
                </button>
                <button
                  disabled={busy || state.mode !== "native"}
                  onClick={async () => {
                    const [path] = await pick("save");
                    if (path)
                      await action("profiles.export", { id: p.id, path });
                  }}
                >
                  {t("ui.3664895579f0")}
                </button>
                <button
                  disabled={busy || p.id === state.activeProfileId}
                  onClick={() => {
                    if (window.confirm(t("ui.3ae2260ff612", { arg0: p.name })))
                      void action("profiles.remove", { id: p.id });
                  }}
                >
                  {t("ui.c3812fc4acb8")}
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
  const [nameError, setNameError] = useState("");
  function field(key: keyof Profile, value: string | boolean | number) {
    if (key === "name") setNameError("");
    setDraft({ ...draft, [key]: value });
    change(true);
  }
  return (
    <Modal
      title={profile === "new" ? t("ui.61d30d997d13") : t("ui.15c4aa13037e")}
      close={close}
      dirty={changed}
    >
      <form
        className="form"
        onSubmit={async (e) => {
          e.preventDefault();
          let name: string;
          try {
            name = trimProfileName(draft.name);
            if (
              !name ||
              (!(profile !== "new" && name === profile.name) &&
                state.profiles.some(
                  (p) =>
                    p.id !== draft.id &&
                    profileNameKey(p.name) === profileNameKey(name),
                ))
            ) {
              setNameError(
                name
                  ? "That profile name is already in use."
                  : "Enter a profile name.",
              );
              return;
            }
          } catch (error) {
            setNameError(
              error instanceof Error ? error.message : String(error),
            );
            return;
          }
          const ok = await action(
            profile === "new" ? "profiles.create" : "profiles.update",
            { ...draft, name },
          );
          if (ok) close();
        }}
      >
        <label>
          {t("ui.dcd1d5223f73")}
          <input
            required
            maxLength={profileDefinitions.nameLimit}
            value={draft.name}
            aria-invalid={Boolean(nameError)}
            onChange={(e) => field("name", e.target.value)}
          />
        </label>
        {nameError && <p role="alert">{tText(nameError)}</p>}
        <label>
          {t("ui.526e0087cc3f")}
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
                    ? t("ui.a004fe1365ec")
                    : key === "avatarFocalY"
                      ? t("ui.40a3878f39f3")
                      : t("ui.509c517ede79")}
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
          {t("ui.d3a008ef1335")}
          <select
            value={draft.region ?? "Auto (Global)"}
            onChange={(e) => field("region", e.target.value)}
          >
            {profileDefinitions.regions.map((r) => (
              <option key={r} value={r}>
                {tText(r)}
              </option>
            ))}
          </select>
        </label>
        <label className="row">
          {t("ui.001c39f7b509")}
          <input
            type="checkbox"
            checked={draft.offline ?? true}
            onChange={(e) => field("offline", e.target.checked)}
          />
        </label>
        <label className="row">
          {t("ui.8e09ffec6847")}
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
          {t("ui.8b5c408cae99")}
          <select
            value={draft.startupPage ?? "Launcher default"}
            onChange={(event) => field("startupPage", event.target.value)}
          >
            {profileDefinitions.startupPages.map((page) => (
              <option key={page} value={page}>
                {tText(page)}
              </option>
            ))}
          </select>
        </label>
        <fieldset
          className="profile-runtime"
          disabled={busy || !draft.isolatedSettings}
        >
          <legend>{t("ui.810ba50189d9")}</legend>
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
                  {tText(definition.label)}
                  <small>{tText(definition.description)}</small>
                </span>
                {definition.type === "bool" ? (
                  <input
                    aria-label={tText(definition.label)}
                    type="checkbox"
                    checked={Boolean(value)}
                    onChange={(event) => update(event.target.checked)}
                  />
                ) : definition.type === "enum" ? (
                  <select
                    aria-label={tText(definition.label)}
                    value={String(value)}
                    onChange={(event) => update(event.target.value)}
                  >
                    {definition.options.map((option) => (
                      <option key={String(option.value)} value={option.value}>
                        {tText(option.label)}
                      </option>
                    ))}
                  </select>
                ) : (
                  <>
                    <input
                      aria-label={tText(definition.label)}
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
          <summary>{t("ui.aa1c4e565765")}</summary>
          {(["gamePath", "savePath", "screenshotPath"] as const).map((key) => (
            <label key={key}>
              {tText(key.replace("Path", " folder"))}
              <input
                value={draft[key] ?? ""}
                placeholder={t("ui.8b56b34ea69b")}
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
                {t("ui.17152dd6e093")}
                {key.replace("Path", " folder")}
              </button>
            </label>
          ))}
        </details>
        <button className="primary" disabled={busy}>
          {t("ui.0c8209e72ec8")}
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
          {t("ui.b0efd7732bb2")}
        </button>
        <button
          disabled={busy || !state.capabilities.modules}
          onClick={async () => {
            const [path] = await pick("open");
            if (path) await action("modules.installPackage", { path });
          }}
        >
          {t("ui.ad63cc994d2a")}
        </button>
        <button onClick={() => setCatalog(true)}>{t("ui.7d9e69313fa6")}</button>
        <button
          disabled={busy || !state.capabilities.modules}
          onClick={() => void action("modules.rescan")}
        >
          {t("ui.7d58b57ddbb4")}
        </button>
      </div>
      {!modules.length && (
        <Empty title={t("ui.d9be89b9d31f")}>{t("ui.48f2ec6c8b9b")}</Empty>
      )}
      <div className="cards">
        {modules.map((m) => (
          <article
            key={m.id}
            className="card"
            id={`module-${m.id}`}
            tabIndex={-1}
          >
            <p className="eyebrow">
              {m.enabled ? t("ui.616f73fbace9") : t("ui.0c7ef33e451e")}
            </p>
            <h2>{m.name}</h2>
            <p>
              {t("ui.92b2c95e99f0")}
              {m.version}
            </p>
            <p>{m.compatibility?.message ?? t("ui.ab91d9f426aa")}</p>
            {!!m.compatibility?.issues.length && (
              <ul
                className="error"
                aria-label={t("ui.4a15c536d09c", { arg0: m.name })}
              >
                {m.compatibility.issues.map((issue) => (
                  <li key={issue}>{issue}</li>
                ))}
              </ul>
            )}
            {!!m.compatibility?.warnings.length && (
              <ul aria-label={t("ui.65ad0df8d356", { arg0: m.name })}>
                {m.compatibility.warnings.map((warning) => (
                  <li key={warning}>{warning}</li>
                ))}
              </ul>
            )}
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
                {m.enabled ? t("ui.b7e3e4aa4257") : t("ui.5342e09f2729")}
              </button>
              <button onClick={() => setSettings(m)}>
                {t("ui.74a883a037bc")}
              </button>
              <button
                disabled={
                  busy || !state.games.some((game) => game.moduleId === m.id)
                }
                onClick={() => {
                  if (window.confirm(t("ui.f81beced084d", { arg0: m.name })))
                    void action("modules.unlinkGames", { id: m.id });
                }}
              >
                {t("ui.ea0d4531e400")}
              </button>
              <button onClick={() => setUpdateHistory(m.id)}>
                {t("ui.c665338a9287")}
              </button>
              <button
                disabled={busy || !m.previousVersions?.length}
                onClick={() => {
                  if (window.confirm(t("ui.9a807c1fd26c", { arg0: m.name })))
                    void action("modules.rollback", { id: m.id });
                }}
              >
                {t("ui.931f703d1509")}
              </button>
              <button
                disabled={busy}
                onClick={() =>
                  void action("system.openPath", { target: "module", id: m.id })
                }
              >
                {t("ui.6a9084025c4b")}
              </button>
              <button
                disabled={busy}
                onClick={() => void action("modules.verify", { id: m.id })}
              >
                {t("ui.39d3c64a7367")}
              </button>
              <button
                disabled={busy}
                onClick={() => {
                  if (window.confirm(t("ui.4d8395a1f0ba", { arg0: m.name })))
                    void action("modules.remove", { id: m.id });
                }}
              >
                {t("ui.be01aa28a44c")}
              </button>
              {m.canUninstall && (
                <button
                  disabled={busy}
                  onClick={() => {
                    if (window.confirm(t("ui.3d53caa7ae49", { arg0: m.name })))
                      void action("modules.uninstall", { id: m.id });
                  }}
                >
                  {t("ui.af1191f5a6d3")}
                </button>
              )}
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
    <Modal title={t("ui.bd8c863d3c5c", { arg0: module.name })} close={close}>
      {!module.schema.length && <p>{t("ui.5d460ccc6218")}</p>}
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
          if (window.confirm(t("ui.a08f5f00c702")))
            void action("modules.resetSettings", { id: module.id });
        }}
      >
        {t("ui.3c6581a82e38")}
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
    navigate,
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
      d.key !== "general/language" &&
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
        ? `${tText(d.label)} ${d.key}`
            .toLowerCase()
            .includes(query.toLowerCase())
        : d.category === category),
  );
  return (
    <div className="settings-layout">
      <nav aria-label={t("ui.b9ed95822f83")} className="category-nav">
        {categories.map((c) => (
          <button
            key={c}
            aria-current={category === c ? "page" : undefined}
            onClick={() => setCategory(c)}
          >
            {tText(c[0].toUpperCase() + c.slice(1))}
          </button>
        ))}
      </nav>
      <section className="card settings">
        <div className="heading">
          <h2>
            {query
              ? t("ui.e978b00de465")
              : tText(category[0].toUpperCase() + category.slice(1))}
          </h2>
          <button
            disabled={busy}
            onClick={() => {
              if (window.confirm(t("ui.b34a49d849fb")))
                void action("settings.reset", {
                  category: query ? "" : category,
                });
            }}
          >
            {t("ui.daee7606b339")}
          </button>
        </div>
        {category === "appearance" && !query && <AppearanceSettings />}
        {(!query
          ? category === "general"
          : [tText("Language"), tText("Display language")].some((label) =>
              label.toLowerCase().includes(query.toLowerCase()),
            )) && <LanguageSettings />}
        {category === "general" && !query && <LegacyMigration />}
        {category === "input" && !query && <InputSettings />}
        {visible.some((d) => d.category === "network") &&
          !featureEnabled(state, "settings.network") && (
            <p role="status">{t("ui.c8d770e8c061")}</p>
          )}
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
                  {tText(d.label)}
                  <small>{d.key}</small>
                </span>
                {d.type === "bool" ? (
                  <input
                    type="checkbox"
                    disabled={
                      busy ||
                      (d.key === "community/discordRichPresence" &&
                        (!state.community?.presence.providerAvailable ||
                          !!state.recovery?.safeMode)) ||
                      (d.category === "network" &&
                        !featureEnabled(state, "settings.network"))
                    }
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
                    disabled={
                      busy ||
                      (d.category === "network" &&
                        !featureEnabled(state, "settings.network"))
                    }
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
                        {tText(o.label)}
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
                    disabled={
                      busy ||
                      (d.category === "network" &&
                        !featureEnabled(state, "settings.network"))
                    }
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
                aria-label={t("ui.8d56d4d79c6e", { arg0: d.label })}
                disabled={busy || state.settings?.[d.key] === undefined}
                onClick={() => void action("settings.resetKey", { key: d.key })}
              >
                {t("ui.daee7606b339")}
              </button>
            </div>
          );
        })}
        {category === "paths" && !query && <StorageSettings />}
        {category === "filesystem" && !query && (
          <div>
            <p>{t("ui.b780fed2bf7e")}</p>
            <button onClick={() => navigate("Filesystem")}>
              {t("ui.55420f34f540")}
            </button>
          </div>
        )}
        {category === "network" && !query && (
          <button onClick={() => navigate("Network")}>
            {t("ui.ff2d496df641")}
          </button>
        )}
        {category === "developer" &&
          !query &&
          Boolean(state.settings?.["developer/modeEnabled"]) && (
            <button onClick={() => navigate("Developer")}>
              {t("ui.220cb26fc025")}
            </button>
          )}
        {category === "about" && (
          <div>
            <p>{t("ui.f585638e0406")}</p>
            {state.application ? (
              <p>
                {t("ui.92b2c95e99f0")}
                {state.application.version} / {state.application.platform} (
                {state.application.architecture})
              </p>
            ) : (
              <p>{t("ui.1944b776d709")}</p>
            )}
          </div>
        )}
        {category === "updates" && <UpdatesPanel />}
        {category === "community" && (
          <>
            <p>{t("presence.privacy")}</p>
            {state.community && (
              <>
                <p role="status">
                  {tText(state.community.presence.providerStatus)}
                </p>
                {state.community.presence.enabled && (
                  <>
                    <p>
                      {t("presence.preview", {
                        details: state.community.presence.activity.details,
                      })}
                    </p>
                    <button
                      disabled={busy}
                      onClick={() => void action("community.refresh")}
                    >
                      {t("presence.refresh")}
                    </button>
                  </>
                )}
                {!!state.recovery?.safeMode && <p>{t("presence.safeMode")}</p>}
              </>
            )}
            <div className="actions">
              <button
                disabled={busy || state.mode !== "native"}
                onClick={() =>
                  void action("system.openUrl", {
                    url: "https://discord.gg/zVGw3HADgA",
                  })
                }
              >
                {t("ui.c8cd344d05c9")}
              </button>
              <button
                disabled={busy || state.mode !== "native"}
                onClick={() =>
                  void action("system.openUrl", {
                    url: "https://github.com/nimauria/Xenon-Recomp",
                  })
                }
              >
                {t("ui.c3efa818b64b")}
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
            aria-label={t("ui.1c0d337c73ef", { arg0: game.title })}
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
    ? tText("Time unavailable")
    : date.toLocaleString(language());
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
        <p className="eyebrow">{t("ui.d599547e30aa")}</p>
        <h2>
          {state.profiles.find((p) => p.id === state.activeProfileId)?.name}
        </h2>
        <p>
          {state.games.length}
          {t("ui.6eaa379fc351")}
          {state.modules?.length ?? 0} {t("ui.d987c944efef")}
        </p>
        <button className="primary" onClick={() => navigate("Library")}>
          {t("ui.fef7a7574831")}
        </button>
      </section>
      <div className="home-stats" aria-label={t("ui.488f8fd876f1")}>
        <button onClick={() => navigate("Library")}>
          <strong>{state.games.length}</strong>
          <span>{t("ui.d9dae78103ad")}</span>
        </button>
        <button onClick={() => navigate("Modules")}>
          <strong>
            {state.modules?.filter((module) => module.enabled).length ?? 0} /{" "}
            {state.modules?.length ?? 0}
          </strong>
          <span>{t("ui.6290987e224c")}</span>
        </button>
        <button onClick={() => navigate("Captures")}>
          <strong>{state.captures?.length ?? 0}</strong>
          <span>{t("ui.502d75c486cf")}</span>
        </button>
        <div>
          <strong>{state.history?.length ?? 0}</strong>
          <span>{t("ui.53682f6b1ec0")}</span>
        </div>
        <div>
          <strong>{playtime(state.activitySummary?.totalPlayTimeMs)}</strong>
          <span>{t("ui.d6ae27bdce58")}</span>
        </div>
        <div>
          <strong>{state.activitySummary?.playCount ?? 0}</strong>
          <span>{t("ui.c6bae554de5a")}</span>
        </div>
      </div>
      <HomeShelf
        title={t("ui.d97d51d37092")}
        games={games.filter((game) => game.favorite)}
      />
      {(state.activitySummary?.fixturePlayCount ?? 0) > 0 && (
        <p>
          {t("ui.e00c279e9ad1")}
          {state.activitySummary?.fixturePlayCount}
          {t("ui.35f9cf5b49b3")}{" "}
          {playtime(state.activitySummary?.fixturePlayTimeMs)}
          {t("ui.fe3ab6a068f4")}
        </p>
      )}
      <HomeShelf
        title={t("ui.452ec9f553a3")}
        games={[...games]
          .filter((game) => Boolean(game.lastPlayed))
          .sort(
            (a, b) => activityTime(b.lastPlayed) - activityTime(a.lastPlayed),
          )}
      />
      <HomeShelf title={t("ui.96ec5d338ae3")} games={recent} />
      <div className="heading">
        <h2>{t("ui.8c4f3d130762")}</h2>
        <button
          disabled={busy || !state.history?.length}
          onClick={() => {
            if (window.confirm(t("ui.74efbc0d7e37")))
              void action("history.clear");
          }}
        >
          {t("ui.496121494e4a")}
        </button>
      </div>
      {!sessions.length ? (
        <Empty title={t("ui.f502267deff4")}>
          {query ? t("ui.9a046b61c29f") : t("ui.9fc9a9766f3f")}
        </Empty>
      ) : (
        sessions.map((h) => (
          <article className="card" key={h.id}>
            <h3>
              {h.title}
              {h.fixture && (
                <span className="badge">{t("ui.4a53f8e990f6")}</span>
              )}
            </h3>
            <p>
              {tText(h.state)} · {timestamp(h.time)} · {playtime(h.durationMs)}
            </p>
            {h.gameId && state.games.some((game) => game.id === h.gameId) && (
              <button onClick={() => openGame(h.gameId!)}>
                {t("ui.2af017ecdd5f")}
              </button>
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
          {t("ui.0df6f1cad36c")}
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
                {tText(f[0].toUpperCase() + f.slice(1))}
              </option>
            ))}
          </select>
        </label>
        <button
          disabled={
            busy || !state.jobs?.some((j) => !isActiveJob(j) && !j.cleaningUp)
          }
          onClick={() => {
            if (window.confirm(t("ui.6fe37e9c8182")))
              void action("jobs.clearFinished");
          }}
        >
          {t("ui.2a16b4f86f58")}
        </button>
      </div>
      {!jobs.length ? (
        <Empty title={t("ui.09ef53392a44")}>{t("ui.28371a884422")}</Empty>
      ) : (
        jobs.map((j) => (
          <article
            className="card operation"
            key={j.id}
            data-job-id={j.id}
            data-state={tText(j.state)}
            aria-label={j.title}
          >
            <h3>{j.title}</h3>
            <progress
              aria-label={t("ui.19d06be137b1", { arg0: j.title })}
              max={100}
              value={j.progress}
            />
            <p>
              <span className="operation-state">
                {j.state === "committing"
                  ? t("ui.7afcf133ecfa")
                  : tText(j.state)}
              </span>
              {j.state === "running" &&
                j.phase &&
                j.phase.toLowerCase() !== j.state.toLowerCase() && (
                  <> · {tText(j.phase)}</>
                )}{" "}
              · {Math.round(j.progress)}%
            </p>
            {isActiveJob(j) && Boolean(j.bytesTotal) && (
              <small>
                {((j.bytesDone ?? 0) / 1048576).toFixed(1)} /{" "}
                {((j.bytesTotal ?? 0) / 1048576).toFixed(1)}
                {t("ui.eca2d5dc7575")}
              </small>
            )}
            {j.error && <p>{tText(j.error)}</p>}
            {j.warning && <p role="status">{j.warning}</p>}
            {j.cleaningUp && <p role="status">{t("ui.67827ba0c9e4")}</p>}
            {j.cleanupError && (
              <p>
                {t("ui.1e6df6f1c077")}
                {j.cleanupError}
              </p>
            )}
            <div className="actions">
              {j.canCancel && (
                <button
                  disabled={busy}
                  onClick={() => void action("jobs.cancel", { id: j.id })}
                >
                  {t("ui.19766ed6ccb2")}
                </button>
              )}
              {j.state === "cancelling" && (
                <span role="status">{t("ui.91b104db05da")}</span>
              )}
              {j.canRetry && (
                <button
                  disabled={busy}
                  onClick={() => void action("jobs.retry", { id: j.id })}
                >
                  {t("ui.942087cc2d41")}
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
    application: state.application,
    runtimeConnected: state.capabilities.runtime,
    counts: {
      games: state.games.length,
      profiles: state.profiles.length,
      modules: state.modules?.length ?? 0,
    },
  };
  return (
    <section className="card">
      <h2>{t("ui.9e714a0d8605")}</h2>
      <p>{t("ui.3fe716b83bdb")}</p>
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
          {copied ? t("ui.8d525e5f158b") : t("ui.46fa69e51b74")}
        </button>
        <button onClick={() => downloadJson("xenon-diagnostics.json", report)}>
          {t("ui.bf7373be1cb1")}
        </button>
        <button
          disabled={busy || state.mode !== "native"}
          onClick={() => void action("support.bundle")}
        >
          {t("ui.d9bf4a9055a6")}
        </button>
      </div>
      <div className="actions">
        <button
          disabled={busy || state.mode !== "native"}
          onClick={() => void action("system.openPath", { target: "support" })}
        >
          {t("ui.28d41b81c690")}
        </button>
        <button
          disabled={busy || state.mode !== "native"}
          onClick={() => void action("system.openPath", { target: "storage" })}
        >
          {t("ui.62c95537c69d")}
        </button>
        <button
          disabled={busy || state.mode !== "native"}
          onClick={() => void action("application.restart", { safeMode: true })}
        >
          {t("ui.0a34df8a178f")}
        </button>
        {state.recovery?.safeMode && (
          <button
            disabled={busy}
            onClick={() =>
              void action("application.restart", { safeMode: false })
            }
          >
            {t("ui.51326d4f4e7f")}
          </button>
        )}
      </div>
      <p>{t("ui.f51a45b0137b")}</p>
    </section>
  );
}
export { RuntimeServices as RuntimeTools } from "./RuntimeServices";

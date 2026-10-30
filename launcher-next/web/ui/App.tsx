import { useControllerNavigation, controllerGlyphs } from "../controller";
import { ActionMenu } from "./ActionMenu";
import {
  t,
  tText,
  useLocalization,
  loadLocalization,
  setLanguage,
  language,
} from "../localization";
import { useEffect, useRef, useState } from "react";
import type {
  Page,
  Snapshot,
  Transport,
  LauncherIntent,
  Notification,
} from "../../contracts/launcher";
import { connect } from "../backend";
import { isActiveJob } from "../jobs";
import { navigationGroups } from "../navigation";
import { applyAppearance, resolveAppearance } from "../appearance";
import { ThemeBackdrop } from "./ThemeBackdrop";
import { SessionPanel } from "./SessionPanel";
import { Icon } from "./Icon";
import { Context, Empty, Modal, type LibraryBrowse } from "./context";
import {
  Library,
  Modules,
  Profiles,
  Settings,
  Activity,
  Captures,
  Downloads,
  Support,
  RuntimeTools,
} from "./pages";
import { CommandPalette } from "./CommandPalette";
export function App() {
  useLocalization();
  useEffect(() => {
    void loadLocalization();
  }, []);
  const [narrow, setNarrow] = useState(
    matchMedia("(max-width: 1000px)").matches,
  );
  const [systemDark, setSystemDark] = useState(
    matchMedia("(prefers-color-scheme: dark)").matches,
  );
  const [systemContrast, setSystemContrast] = useState(
    matchMedia("(forced-colors: active)").matches,
  );
  useEffect(() => {
    const dark = matchMedia("(prefers-color-scheme: dark)"),
      contrast = matchMedia("(forced-colors: active)"),
      narrowMedia = matchMedia("(max-width: 1000px)");
    const update = () => {
      setSystemDark(dark.matches);
      setSystemContrast(contrast.matches);
      setNarrow(narrowMedia.matches);
    };
    dark.addEventListener("change", update);
    contrast.addEventListener("change", update);
    narrowMedia.addEventListener("change", update);
    return () => {
      dark.removeEventListener("change", update);
      contrast.removeEventListener("change", update);
      narrowMedia.removeEventListener("change", update);
    };
  }, []);
  const backend = useRef<Transport | null>(null);
  const inFlight = useRef(false);
  const [state, storeSnapshot] = useState<Snapshot>();
  function setState(snapshot: Snapshot) {
    storeSnapshot((previous) =>
      previous && previous.revision > snapshot.revision ? previous : snapshot,
    );
  }
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  useEffect(() => {
    setLanguage(String(state?.settings?.["general/language"] ?? "auto"));
  }, [state?.settings?.["general/language"]]);
  const [page, setPage] = useState<Page>("Library");
  const pageRef = useRef<Page>(page);
  pageRef.current = page;
  const pageHistory = useRef<Page[]>([]);
  const [queries, setQueries] = useState<Partial<Record<Page, string>>>({});
  const query = queries[page] ?? "";
  const setQuery = (value: string) =>
    setQueries((previous) => ({ ...previous, [page]: value }));
  const [libraryBrowse, setLibraryBrowse] = useState<LibraryBrowse>({
    selected: "",
    filter: "All games",
    sort: "A–Z",
    scroll: 0,
  });
  const [palette, setPalette] = useState(false);
  const [navigationIntent, setNavigationIntent] =
    useState<Extract<LauncherIntent, { type: "navigate" }>>();
  const [toast, setToast] = useState<Notification>();
  const notificationSeen = useRef<string | null>(null);
  const [notifications, setNotifications] = useState(false);
  const [help, setHelp] = useState(false);
  async function refresh() {
    if (backend.current)
      setState(await backend.current.request("application.snapshot", {}));
  }
  function navigate(
    destination: Page,
    targetId?: string,
    section?: string,
    record = true,
  ) {
    const dialog = document.querySelector<HTMLDialogElement>("dialog[open]");
    if (
      dialog?.dataset.dirty === "true" &&
      !window.confirm(t("ui.470e7ee2a579"))
    )
      return false;
    dialog?.dispatchEvent(new Event("launcher-dismiss"));
    if (record && destination !== pageRef.current) {
      pageHistory.current.push(pageRef.current);
      if (pageHistory.current.length > 32) pageHistory.current.shift();
    }
    pageRef.current = destination;
    setPage(destination);
    setNavigationIntent({
      type: "navigate",
      page: destination,
      targetId,
      section,
    });
    if (targetId || section)
      setQueries((previous) => ({ ...previous, [destination]: "" }));
    setPalette(false);
    if (!targetId && !section)
      requestAnimationFrame(() => document.getElementById("content")?.focus());
    return true;
  }
  function goBack() {
    const previous = pageHistory.current.at(-1);
    if (previous && navigate(previous, undefined, undefined, false))
      pageHistory.current.pop();
  }
  function openGame(id: string) {
    if (!navigate("Library")) return;
    setQueries((previous) => ({ ...previous, Library: "" }));
    setLibraryBrowse((previous) => ({
      ...previous,
      selected: id,
      filter: "All games",
    }));
  }
  async function executeIntent(intent: LauncherIntent) {
    if (intent.type === "navigate") {
      if (intent.page === "Library" && intent.targetId)
        openGame(intent.targetId);
      else navigate(intent.page, intent.targetId, intent.section);
    } else {
      setPalette(false);
      await action(intent.name, intent.data ?? {});
    }
  }
  async function action(name: string, data: Record<string, unknown> = {}) {
    if (!backend.current || inFlight.current) return false;
    inFlight.current = true;
    setBusy(true);
    setError("");
    try {
      if (name === "profiles.create")
        setState(
          await backend.current.request("profiles.create", {
            ...data,
            name: String(data.name),
          }),
        );
      else if (name === "profiles.activate" || name === "profiles.remove")
        setState(await backend.current.request(name, { id: String(data.id) }));
      else if (name === "settings.update")
        setState(await backend.current.request(name, data));
      else if (name === "library.favorite")
        setState(
          await backend.current.request(name, {
            id: String(data.id),
            favorite: Boolean(data.favorite),
          }),
        );
      else {
        await backend.current.request("launcher.action", {
          action: name,
          data,
        });
        await refresh();
      }
      return true;
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
      return false;
    } finally {
      inFlight.current = false;
      setBusy(false);
    }
  }
  async function pick(mode: "open" | "multiple" | "folder" | "save") {
    if (!backend.current) return [];
    try {
      return (await backend.current.request("native.dialog", { mode })).paths;
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
      return [];
    }
  }
  useEffect(() => {
    if (!state) return;
    const latest = state.notifications?.at(-1);
    const fingerprint = latest
      ? latest.id + ":" + (latest.repeatCount ?? 1)
      : "";
    if (
      notificationSeen.current !== null &&
      fingerprint &&
      fingerprint !== notificationSeen.current &&
      latest &&
      !latest.read
    )
      setToast(latest);
    notificationSeen.current = fingerprint;
  }, [state?.notifications]);
  useEffect(() => {
    if (!toast) return;
    const timer = window.setTimeout(() => setToast(undefined), 7000);
    return () => clearTimeout(timer);
  }, [toast]);
  useEffect(() => {
    if (!state || !backend.current) return;
    const timer = window.setTimeout(() => {
      void backend.current
        ?.request("launcher.action", {
          action: "navigation.remember",
          data: { page },
        })
        .then(setState)
        .catch((error) => setError(String(error)));
    }, 250);
    return () => clearTimeout(timer);
  }, [page, state?.mode]);
  const readySent = useRef(false);
  useEffect(() => {
    if (state?.mode !== "native" || readySent.current || !backend.current)
      return;
    readySent.current = true;
    void backend.current
      .request("launcher.action", { action: "application.ready", data: {} })
      .catch((error) => setError(String(error)));
  }, [state?.mode]);
  const hasActiveJobs = state?.jobs?.some(isActiveJob) ?? false;
  useEffect(() => {
    if (!state?.community?.presence.requestedEnabled) return;
    let active = true,
      pending = false;
    const timer = window.setInterval(async () => {
      if (pending || inFlight.current || !backend.current) return;
      pending = true;
      try {
        const community = await backend.current.request("community.state", {});
        if (active)
          storeSnapshot((previous) =>
            previous &&
            JSON.stringify(previous.community) !== JSON.stringify(community)
              ? { ...previous, community }
              : previous,
          );
      } catch {
        /* The main request path reports bridge failures. */
      } finally {
        pending = false;
      }
    }, 1000);
    return () => {
      active = false;
      clearInterval(timer);
    };
  }, [state?.community?.presence.requestedEnabled]);
  const hasPendingJobs =
    state?.jobs?.some((job) => isActiveJob(job) || job.cleaningUp) ?? false;
  useEffect(() => {
    if (!state?.capabilities.runtime && !hasPendingJobs && page !== "Captures")
      return;
    let active = true,
      pending = false;
    const timer = window.setInterval(async () => {
      if (pending || inFlight.current || !backend.current) return;
      pending = true;
      try {
        const snapshot = await backend.current.request(
          "application.snapshot",
          {},
        );
        if (active) setState(snapshot);
      } catch (error) {
        if (active)
          setError(error instanceof Error ? error.message : String(error));
      } finally {
        pending = false;
      }
    }, 500);
    return () => {
      active = false;
      clearInterval(timer);
    };
  }, [state?.capabilities.runtime, hasPendingJobs, page]);
  useEffect(() => {
    let active = true;
    let unsubscribe: (() => void) | undefined;
    try {
      backend.current = connect();
      let requested = -1,
        received = -1,
        fetching = false;
      unsubscribe = backend.current.subscribe?.((revision) => {
        requested = Math.max(requested, revision);
        if (fetching || requested <= received) return;
        fetching = true;
        void (async () => {
          try {
            let attempts = 0;
            do {
              const snapshot = await backend.current!.request(
                "application.snapshot",
                {},
              );
              received = snapshot.revision;
              if (active) setState(snapshot);
            } while (active && received < requested && ++attempts < 2);
          } catch (error) {
            if (active) setError(String(error));
          } finally {
            fetching = false;
          }
        })();
      });
      backend.current
        .request("application.snapshot", {})
        .then((s) => {
          if (active) {
            setState(s);
            setPage(s.preferences.startPage);
            setLibraryBrowse((previous) => ({
              ...previous,
              selected:
                s.settings?.["library/rememberSelection"] !== false
                  ? String(s.settings?.["library/lastSelectedGameId"] ?? "")
                  : "",
            }));
            if (s.mode === "native" && s.capabilities.online)
              backend.current
                ?.request("launcher.action", {
                  action: "updates.initialize",
                  data: {},
                })
                .then((snapshot) => {
                  if (active) setState(snapshot);
                })
                .catch((error) => {
                  if (active) setError(String(error));
                });
          }
        })
        .catch((e) => {
          if (active) setError(String(e.message));
        });
    } catch (e) {
      setError(String(e));
    }
    return () => {
      active = false;
      unsubscribe?.();
    };
  }, []);
  useEffect(() => {
    if (!state) return;
    applyAppearance(resolveAppearance(state, systemDark, systemContrast));
    document.documentElement.style.fontSize = `${16 * Number(state.settings?.["accessibility/textScale"] ?? 1)}px`;
  }, [state, systemDark, systemContrast]);
  const navigation = useRef({ navigate, goBack });
  navigation.current = { navigate, goBack };
  useEffect(() => {
    const cycle = (event: Event) => {
      if (document.querySelector("dialog[open],[role=menu]:popover-open"))
        return;
      const pages = Array.from(
        document.querySelectorAll<HTMLButtonElement>("nav button[data-page]"),
      ).map((button) => button.dataset.page as Page);
      if (!pages.length) return;
      const index = Math.max(0, pages.indexOf(pageRef.current)),
        delta = (event as CustomEvent).detail === -1 ? -1 : 1;
      navigation.current.navigate(
        pages[(index + delta + pages.length) % pages.length],
      );
    };
    window.addEventListener("launcher-cycle-page", cycle);
    return () => window.removeEventListener("launcher-cycle-page", cycle);
  }, []);
  useEffect(() => {
    const listener = (event: KeyboardEvent) => {
      if (event.defaultPrevented) return;
      if (
        (event.altKey && event.key === "ArrowLeft") ||
        (event.key === "Escape" &&
          !document.querySelector("dialog[open],[role=menu]:popover-open") &&
          !(event.target as HTMLElement)?.matches("input,textarea,select"))
      ) {
        event.preventDefault();
        navigation.current.goBack();
        return;
      }
      if (
        event.key === "ContextMenu" ||
        (event.shiftKey && event.key === "F10")
      ) {
        event.preventDefault();
        const active = document.activeElement as HTMLElement;
        const rect = active?.getBoundingClientRect();
        active?.dispatchEvent(
          new MouseEvent("contextmenu", {
            bubbles: true,
            cancelable: true,
            clientX: rect?.left,
            clientY: rect?.bottom,
          }),
        );
        return;
      }
      if (event.ctrlKey || event.metaKey) {
        const destination = (
          {
            "1": "Library",
            "2": "Modules",
            "3": "Profiles",
            ",": "Settings",
            h: "Home",
          } as Record<string, Page>
        )[event.key];
        if (destination) {
          event.preventDefault();
          navigation.current.navigate(destination);
        }
        if (event.key === "k" || event.key === "f") {
          event.preventDefault();
          setPalette((v) => !v);
        }
      }
      if (event.key === "F1") {
        event.preventDefault();
        setHelp((v) => !v);
      }
    };
    window.addEventListener("keydown", listener);
    return () => window.removeEventListener("keydown", listener);
  }, []);
  const controller = useControllerNavigation(goBack);
  const glyphs = controllerGlyphs(controller.family);
  if (!state)
    return (
      <main className="startup">
        <h1>{t("ui.e1e2d099ffa0")}</h1>
        <p role={error ? "alert" : "status"}>
          {(error ? tText(error) : "") || t("ui.2d9a4d61d2b0")}
        </p>
        {error && <p>{t("ui.de128e71c411")}</p>}
      </main>
    );
  const unread = state.notifications?.filter((n) => !n.read).length ?? 0;
  const developer = Boolean(state.settings?.["developer/modeEnabled"]);
  const sidebarMode = String(state.settings?.["general/sidebarMode"] ?? "Auto");
  const compactSidebar =
    sidebarMode === "Compact" || (sidebarMode === "Auto" && narrow);
  return (
    <Context.Provider
      value={{
        state,
        busy,
        query,
        navigate,
        action,
        pick,
        refresh,
        openGame,
        libraryBrowse,
        setLibraryBrowse,
        navigationIntent,
        clearNavigationIntent: () => setNavigationIntent(undefined),
      }}
    >
      <div
        className="app"
        data-sidebar={compactSidebar ? "compact" : "expanded"}
      >
        <a className="skip" href="#content">
          {t("ui.ac576a66d456")}
        </a>
        <aside className="rail">
          <div className="brand">
            <span className="brand-mark" aria-hidden="true" />
            <strong>{t("ui.ca8152fba865")}</strong>
          </div>
          <p className="eyebrow">{t("ui.da36ef5ed6e5")}</p>
          <button
            className="sidebar-toggle"
            disabled={busy}
            aria-label={
              compactSidebar ? t("ui.4a984a4ea6d0") : t("ui.1afa67a9d3e7")
            }
            aria-expanded={!compactSidebar}
            onClick={() =>
              void action("settings.set", {
                key: "general/sidebarMode",
                value: compactSidebar ? "Expanded" : "Compact",
              })
            }
          >
            <Icon name="Menu" />
            {!compactSidebar && <span>{t("ui.3db65f8c2a7d")}</span>}
          </button>
          <nav aria-label={t("ui.eb355944b92d")}>
            {navigationGroups
              .filter((group) => !group.developer || developer)
              .map((group) => (
                <div
                  key={group.label}
                  className={`nav-group${group.bottom ? " nav-group-bottom" : ""}`}
                  role="group"
                  aria-label={tText(group.label)}
                >
                  <span className="nav-group-label">{tText(group.label)}</span>
                  {group.pages.map((p) => (
                    <button
                      key={p}
                      data-page={p}
                      aria-label={tText(p)}
                      title={tText(p)}
                      aria-current={page === p ? "page" : undefined}
                      onClick={() => navigate(p)}
                    >
                      <Icon name={p} />
                      <span className="nav-label">{tText(p)}</span>
                      {p === "Downloads" && hasActiveJobs && (
                        <span className="badge">
                          {state.jobs?.filter(isActiveJob).length}
                        </span>
                      )}
                    </button>
                  ))}
                </div>
              ))}
          </nav>
          <div className="rail-bottom">
            <span className="dot" />
            <span className="rail-status-copy">
              {state.mode === "preview"
                ? t("ui.352da55ba74a")
                : t("ui.9abee42d2dd8")}
            </span>
            <small>
              {t("ui.109311589787")}{" "}
              {state.capabilities.runtime
                ? state.session?.fixture
                  ? t("ui.9246b3d83911")
                  : t("ui.ddd9818abacf")
                : t("ui.98b1024757db")}
            </small>
            {state.capabilities.networkFixture && (
              <small>{t("ui.23f08cd93ed1")}</small>
            )}
          </div>
        </aside>
        <section className="workspace">
          <ThemeBackdrop
            appearance={resolveAppearance(state, systemDark, systemContrast)}
          />
          <header>
            <ActionMenu
              quick
              label={t("menu.quick")}
              items={[
                {
                  id: "library",
                  label: tText("Library"),
                  run: () => navigate("Library"),
                },
                {
                  id: "profile",
                  label: t("ui.61d30d997d13"),
                  run: () => navigate("Profiles", undefined, "create"),
                },
                {
                  id: "downloads",
                  label: tText("Downloads"),
                  run: () => navigate("Downloads"),
                },
                {
                  id: "settings",
                  label: tText("Settings"),
                  run: () => navigate("Settings"),
                },
                {
                  id: "commands",
                  label: t("ui.b269dc4e81a5"),
                  run: () => setPalette(true),
                },
                {
                  id: "notifications",
                  label: t("ui.788011833a5a"),
                  run: () => setNotifications(true),
                },
                {
                  id: "help",
                  label: t("ui.559cd2d4cc76"),
                  run: () => setHelp(true),
                },
              ]}
            />
            <label className="search">
              <span className="sr-only">{t("ui.e55511f8d7f5")}</span>
              <input
                type="search"
                placeholder={t("ui.8e1c2173dbf4", {
                  arg0: tText(page).toLocaleLowerCase(),
                })}
                value={query}
                onChange={(e) => setQuery(e.target.value)}
              />
            </label>
            <button
              onClick={() => setPalette(true)}
              title={t("ui.199f6f88d1fc")}
            >
              {t("ui.b269dc4e81a5")}
            </button>
            <button
              onClick={() => setNotifications(true)}
              aria-label={t("ui.522e065ca17f", { arg0: unread })}
            >
              {t("ui.4af28ba48e90")}
              {unread > 0 && <span className="badge">{unread}</span>}
            </button>
            <label>
              <span className="sr-only">{t("ui.5608b89a8112")}</span>
              <select
                disabled={busy}
                value={state.activeProfileId}
                onChange={(e) =>
                  void action("profiles.activate", { id: e.target.value })
                }
              >
                {state.profiles.map((p) => (
                  <option value={p.id} key={p.id}>
                    {p.name}
                  </option>
                ))}
              </select>
            </label>
          </header>
          <main id="content" tabIndex={-1} aria-busy={busy}>
            <SessionPanel />
            <div className="heading">
              <div>
                <p className="eyebrow">{t("ui.7f001e843613")}</p>
                <h1>{tText(page)}</h1>
              </div>
              <button
                onClick={() => void refresh().catch((e) => setError(String(e)))}
                disabled={busy}
              >
                {t("ui.0e9161011702")}
              </button>
            </div>
            {error && (
              <div className="error" role="alert">
                {tText(error)}
                <button
                  onClick={() => setError("")}
                  aria-label={t("ui.2db046678b9c")}
                >
                  ×
                </button>
              </div>
            )}
            {page === "Library" && <Library />}
            {page === "Modules" && <Modules />}
            {page === "Profiles" && <Profiles />}
            {page === "Settings" && <Settings />}
            {page === "Home" && <Activity />}
            {page === "Captures" && <Captures />}
            {page === "Downloads" && <Downloads />}
            {page === "Support" && <Support />}
            {(["Developer", "Network", "Filesystem"] as Page[]).includes(
              page,
            ) && <RuntimeTools page={page} />}
          </main>
          <footer>
            <span>{t("ui.c3b26bd9718b")}</span>
            {hasActiveJobs && (
              <button onClick={() => navigate("Downloads")}>
                {t("ui.882559b2cc39")}
                {state.jobs?.filter(isActiveJob).length})
              </button>
            )}
            <span>
              {busy
                ? t("ui.5474eef8d0f1")
                : state.mode === "preview"
                  ? t("ui.cc3518f7a88c")
                  : t("ui.2e67ef168705")}
            </span>
            <button onClick={() => setHelp(true)}>
              {t("ui.e0f2cf0da6ab")}
            </button>
          </footer>
        </section>
        {palette && (
          <CommandPalette
            close={() => setPalette(false)}
            execute={executeIntent}
          />
        )}
        {notifications && (
          <Modal
            title={t("ui.788011833a5a")}
            close={() => setNotifications(false)}
          >
            <button
              disabled={busy || !unread}
              onClick={() => void action("notifications.readAll")}
            >
              {t("ui.3bc62a9e6a39")}
            </button>
            <button
              disabled={busy}
              onClick={() => void action("notifications.clear")}
            >
              {t("ui.496121494e4a")}
            </button>
            {!state.notifications?.length && (
              <Empty title={t("ui.df7d1cc1aa81")}>{t("ui.c5e80a5efa49")}</Empty>
            )}
            {state.notifications
              ?.slice()
              .reverse()
              .map((n) => (
                <article className="card" key={n.id}>
                  <h3>{n.programTitle ? tText(n.title) : n.title}</h3>
                  <p>{n.message}</p>
                  <small>
                    {new Date(Number(n.time)).toLocaleString(language())}
                    {(n.repeatCount ?? 1) > 1
                      ? t("ui.92d15f16adb2", { arg0: n.repeatCount ?? 1 })
                      : ""}
                  </small>
                  {n.action && (
                    <button
                      disabled={busy}
                      onClick={async () => {
                        await action("notifications.read", {
                          id: n.id,
                          read: true,
                        });
                        setNotifications(false);
                        await executeIntent(n.action!);
                      }}
                    >
                      {t("ui.4bf3f8ddd7bf")}
                    </button>
                  )}
                  <button
                    disabled={busy}
                    onClick={() =>
                      void action("notifications.dismiss", { id: n.id })
                    }
                  >
                    {t("ui.b7bb3f342402")}
                  </button>
                  <button
                    disabled={busy}
                    onClick={() =>
                      void action("notifications.read", {
                        id: n.id,
                        read: !n.read,
                      })
                    }
                  >
                    {n.read ? t("ui.54b4e3f28dc5") : t("ui.b49c9b6cdf43")}
                  </button>
                </article>
              ))}
          </Modal>
        )}
        {toast && (
          <section
            className="notification-toast card"
            role="status"
            aria-live="polite"
          >
            <strong>
              {toast.programTitle ? tText(toast.title) : toast.title}
            </strong>
            <p>{toast.message}</p>
            <div className="actions">
              {toast.action && (
                <button
                  onClick={() => {
                    void executeIntent(toast.action!);
                    setToast(undefined);
                  }}
                >
                  {t("ui.4bf3f8ddd7bf")}
                </button>
              )}
              <button
                aria-label={t("ui.dfc6e194ec78")}
                onClick={() => setToast(undefined)}
              >
                {t("ui.48845bff334a")}
              </button>
            </div>
          </section>
        )}
        {help && (
          <Modal title={t("ui.559cd2d4cc76")} close={() => setHelp(false)}>
            <dl>
              <dt>{t("ui.22395070ef87")}</dt>
              <dd>{t("ui.8be0feb36a8a")}</dd>
              <dt>{t("ui.fba9e6c21f1d")}</dt>
              <dd>{t("ui.0466c6703eb0")}</dd>
              <dt>{t("ui.44fdb5a83aa6")}</dt>
              <dd>{t("ui.74a883a037bc")}</dd>
              <dt>{t("ui.015e6e9220f6")}</dt>
              <dd>{t("ui.3a78695388b3")}</dd>
              <dt>{t("ui.31df66dc9f06")}</dt>
              <dd>{t("ui.3c0237a355d1")}</dd>
              {controller.connected && (
                <dd>
                  {t(
                    controller.standard
                      ? "controller.help"
                      : "controller.unmapped",
                    {
                      activate: glyphs.activate,
                      back: glyphs.back,
                      context: glyphs.context,
                    },
                  )}
                </dd>
              )}
            </dl>
          </Modal>
        )}
        {state.recovery?.previousUncleanExit && (
          <Modal
            title={t("ui.d978235e451c")}
            close={() => void action("recovery.acknowledge")}
          >
            <p>{t("ui.172207fa1b30")}</p>
            {state.recovery.automaticSafeMode && (
              <p>
                {t(
                  state.recovery.automaticSafeModeReason === "startup-failure"
                    ? "recovery.startupFailure"
                    : "recovery.repeated",
                  { count: state.recovery.consecutiveUncleanStarts ?? 0 },
                )}
              </p>
            )}
            <div className="actions">
              <button
                disabled={busy}
                onClick={() => void action("recovery.acknowledge")}
              >
                {state.recovery.safeMode
                  ? t("recovery.continueSafe")
                  : t("ui.f6e80e3f8fbe")}
              </button>
              <button
                disabled={busy}
                onClick={() =>
                  void action("application.restart", { safeMode: true })
                }
              >
                {t("ui.0a34df8a178f")}
              </button>
            </div>
          </Modal>
        )}
      </div>
    </Context.Provider>
  );
}

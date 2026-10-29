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
  const [page, setPage] = useState<Page>("Library");
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
  function navigate(destination: Page, targetId?: string, section?: string) {
    const dialog = document.querySelector<HTMLDialogElement>("dialog[open]");
    if (
      dialog?.dataset.dirty === "true" &&
      !window.confirm("Discard unsaved changes?")
    )
      return false;
    dialog?.dispatchEvent(new Event("launcher-dismiss"));
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
    if(state?.mode !== "native" || readySent.current || !backend.current)return;
    readySent.current = true;
    void backend.current.request("launcher.action", {action:"application.ready",data:{}}).catch(error => setError(String(error)));
  },[state?.mode]);
  const hasActiveJobs = state?.jobs?.some(isActiveJob) ?? false;
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
  useEffect(() => {
    const listener = (event: KeyboardEvent) => {
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
          navigate(destination);
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
  useEffect(() => {
    let frame = 0;
    let last = 0;
    let held = false;
    function poll(time: number) {
      const pad = navigator.getGamepads?.().find((p) => p?.connected);
      const active =
        document.visibilityState === "visible" && document.hasFocus();
      const pressed =
        pad &&
        active &&
        (pad.buttons.some((b) => b.pressed) ||
          pad.axes.some((a) => Math.abs(a) > 0.6));
      if (pad && active && pressed && (!held || time - last > 220)) {
        last = time;
        const dialog = document.querySelector("dialog[open]");
        const scope = dialog ?? document;
        const elements = [
          ...scope.querySelectorAll<HTMLElement>(
            "button:not(:disabled),input:not(:disabled),select:not(:disabled),a[href]",
          ),
        ].filter((e) => e.getClientRects().length > 0);
        const index = elements.indexOf(document.activeElement as HTMLElement);
        if (pad.buttons[0]?.pressed)
          (document.activeElement as HTMLElement)?.click();
        else if (pad.buttons[1]?.pressed) {
          if (dialog)
            dialog.dispatchEvent(new Event("cancel", { cancelable: true }));
          else navigate("Library");
        } else {
          const backwards =
            pad.buttons[12]?.pressed ||
            pad.buttons[14]?.pressed ||
            pad.axes[0] < -0.6 ||
            pad.axes[1] < -0.6;
          elements[
            (index + (backwards ? -1 : 1) + elements.length) % elements.length
          ]?.focus();
        }
      }
      held = Boolean(pressed);
      frame = requestAnimationFrame(poll);
    }
    frame = requestAnimationFrame(poll);
    return () => cancelAnimationFrame(frame);
  }, []);
  if (!state)
    return (
      <main className="startup">
        <h1>Xenon</h1>
        <p role={error ? "alert" : "status"}>
          {error || "Opening your library…"}
        </p>
        {error && (
          <p>
            Existing data is preserved. Inspect the startup error before
            retrying.
          </p>
        )}
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
          Skip to content
        </a>
        <aside className="rail">
          <div className="brand">
            <span className="brand-mark" aria-hidden="true" />
            <strong>XENON</strong>
          </div>
          <p className="eyebrow">YOUR GAMES. YOUR SPACE.</p>
          <button
            className="sidebar-toggle"
            disabled={busy}
            aria-label={
              compactSidebar ? "Expand navigation" : "Collapse navigation"
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
            {!compactSidebar && <span>Navigation</span>}
          </button>
          <nav aria-label="Main navigation">
            {navigationGroups
              .filter((group) => !group.developer || developer)
              .map((group) => (
                <div
                  key={group.label}
                  className={`nav-group${group.bottom ? " nav-group-bottom" : ""}`}
                  role="group"
                  aria-label={group.label}
                >
                  <span className="nav-group-label">{group.label}</span>
                  {group.pages.map((p) => (
                    <button
                      key={p}
                      aria-label={p}
                      title={p}
                      aria-current={page === p ? "page" : undefined}
                      onClick={() => navigate(p)}
                    >
                      <Icon name={p} />
                      <span className="nav-label">{p}</span>
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
                ? "Browser preview"
                : "Desktop launcher"}
            </span>
            <small>
              Runtime{" "}
              {state.capabilities.runtime
                ? state.session?.fixture
                  ? "fixture provider"
                  : "available"
                : "not connected"}
            </small>
            {state.capabilities.networkFixture && (
              <small>Network fixture provider</small>
            )}
          </div>
        </aside>
        <section className="workspace">
          <ThemeBackdrop
            appearance={resolveAppearance(state, systemDark, systemContrast)}
          />
          <header>
            <label className="search">
              <span className="sr-only">Search current page</span>
              <input
                type="search"
                placeholder={`Search ${page.toLowerCase()}`}
                value={query}
                onChange={(e) => setQuery(e.target.value)}
              />
            </label>
            <button onClick={() => setPalette(true)} title="Ctrl+K">
              Commands
            </button>
            <button
              onClick={() => setNotifications(true)}
              aria-label={`Notifications, ${unread} unread`}
            >
              Inbox {unread > 0 && <span className="badge">{unread}</span>}
            </button>
            <label>
              <span className="sr-only">Active profile</span>
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
                <p className="eyebrow">XENON LAUNCHER</p>
                <h1>{page}</h1>
              </div>
              <button
                onClick={() => void refresh().catch((e) => setError(String(e)))}
                disabled={busy}
              >
                Refresh
              </button>
            </div>
            {error && (
              <div className="error" role="alert">
                {error}
                <button onClick={() => setError("")} aria-label="Dismiss error">
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
            <span>Xenon · Launcher Next</span>
            {hasActiveJobs && (
              <button onClick={() => navigate("Downloads")}>
                View operations ({state.jobs?.filter(isActiveJob).length})
              </button>
            )}
            <span>
              {busy
                ? "Working…"
                : state.mode === "preview"
                  ? "Preview storage · No native file access"
                  : "Native services · Local storage"}
            </span>
            <button onClick={() => setHelp(true)}>Help · F1</button>
          </footer>
        </section>
        {palette && (
          <CommandPalette
            close={() => setPalette(false)}
            execute={executeIntent}
          />
        )}
        {notifications && (
          <Modal title="Notifications" close={() => setNotifications(false)}>
            <button
              disabled={busy || !unread}
              onClick={() => void action("notifications.readAll")}
            >
              Mark all read
            </button>
            <button
              disabled={busy}
              onClick={() => void action("notifications.clear")}
            >
              Clear history
            </button>
            {!state.notifications?.length && (
              <Empty title="You're up to date">
                Launcher activity will appear here.
              </Empty>
            )}
            {state.notifications
              ?.slice()
              .reverse()
              .map((n) => (
                <article className="card" key={n.id}>
                  <h3>{n.title}</h3>
                  <p>{n.message}</p>
                  <small>
                    {new Date(Number(n.time)).toLocaleString()}
                    {(n.repeatCount ?? 1) > 1
                      ? ` ? ${n.repeatCount} occurrences`
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
                      View activity
                    </button>
                  )}
                  <button
                    disabled={busy}
                    onClick={() =>
                      void action("notifications.dismiss", { id: n.id })
                    }
                  >
                    Dismiss notification
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
                    {n.read ? "Mark unread" : "Mark read"}
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
            <strong>{toast.title}</strong>
            <p>{toast.message}</p>
            <div className="actions">
              {toast.action && (
                <button
                  onClick={() => {
                    void executeIntent(toast.action!);
                    setToast(undefined);
                  }}
                >
                  View activity
                </button>
              )}
              <button
                aria-label="Dismiss toast"
                onClick={() => setToast(undefined)}
              >
                Dismiss
              </button>
            </div>
          </section>
        )}
        {help && (
          <Modal title="Launcher shortcuts" close={() => setHelp(false)}>
            <dl>
              <dt>Ctrl+K / Ctrl+F</dt>
              <dd>Global commands</dd>
              <dt>Ctrl+1 / 2 / 3</dt>
              <dd>Library / Modules / Profiles</dd>
              <dt>Ctrl+,</dt>
              <dd>Settings</dd>
              <dt>Ctrl+H</dt>
              <dd>Home</dd>
              <dt>Controller</dt>
              <dd>
                D-pad or stick moves focus; A/Cross activates; B/Circle goes
                back. Input is limited to the foreground window.
              </dd>
            </dl>
          </Modal>
        )}
        {state.recovery?.previousUncleanExit && (
          <Modal
            title="Launcher recovery"
            close={() => void action("recovery.acknowledge")}
          >
            <p>
              The previous launcher session did not close cleanly. Your saved
              data has been preserved.
            </p>
            <div className="actions">
              <button
                disabled={busy}
                onClick={() => void action("recovery.acknowledge")}
              >
                Continue normally
              </button>
              <button
                disabled={busy}
                onClick={() =>
                  void action("application.restart", { safeMode: true })
                }
              >
                Restart in safe mode
              </button>
            </div>
          </Modal>
        )}
      </div>
    </Context.Provider>
  );
}

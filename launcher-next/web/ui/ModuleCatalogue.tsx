import { useState } from "react";
import { Modal, Empty, useLauncher } from "./context";
import { CoverImage } from "./CoverImage";
const descriptions: Record<string, string> = {
  idle: "Check GitHub for a published release.",
  checking: "Checking releases…",
  available: "A release is available.",
  "update-available": "A newer version is available.",
  "up-to-date": "The installed version is current.",
  "no-release": "No published semantic-version release is available.",
  "package-unavailable":
    "The latest release has no matching package for this launcher and platform.",
  "digest-unavailable":
    "The release package has no SHA-256 digest. Download is unavailable.",
  downloading: "Downloading and verifying the package…",
  preparing: "Verifying and preparing the replacement launcher…",
  restarting: "Restarting into the replacement launcher…",
  "ready-to-install":
    "The downloaded package is verified and ready to install.",
  error: "The operation could not finish.",
};
export function UpdateControls({
  id,
  name,
  launcher = false,
}: {
  id: string;
  name: string;
  launcher?: boolean;
}) {
  const { state, busy, action } = useLauncher();
  const value = launcher ? state.launcherUpdate : state.moduleUpdates?.[id];
  const release = value?.release;
  return (
    <section className="update-controls" aria-label={`${name} updates`}>
      <p role={value?.busy ? "status" : undefined}>
        {value
          ? (descriptions[value.state] ?? value.state)
          : "This module is not listed in the saved registry."}
      </p>
      {release && (
        <p>
          Available: <strong>{release.version}</strong>
          {release.prerelease ? " · Prerelease" : ""}
          {value?.installedVersion
            ? ` · Installed: ${value.installedVersion}`
            : ""}
        </p>
      )}
      {value?.lastError && <p className="update-error">{value.lastError}</p>}
      <div className="actions">
        <button
          disabled={busy || !value?.canCheck}
          onClick={() =>
            void action(launcher ? "updates.check" : "modules.checkUpdate", {
              id,
            })
          }
        >
          Check for updates
        </button>
        {value?.canDownload && (
          <button
            disabled={busy}
            onClick={() =>
              void action(
                launcher ? "updates.download" : "modules.downloadUpdate",
                { id },
              )
            }
          >
            Download release
          </button>
        )}
        {value?.canInstall && (
          <button
            className="primary"
            disabled={busy}
            onClick={() => {
              if (
                window.confirm(
                  launcher
                    ? `Install launcher ${release?.version} and restart? The previous installation is retained for rollback if startup fails.`
                    : `Install ${name} ${release?.version}? The current module is retained for rollback.`,
                )
              )
                void action(
                  launcher ? "updates.install" : "modules.installDownloaded",
                  { id },
                );
            }}
          >
            Install verified package
          </button>
        )}
        {release && (
          <button
            disabled={busy}
            onClick={() => void action("system.openUrl", { url: release.url })}
          >
            Release page
          </button>
        )}
      </div>
      {launcher && value?.state === "ready-to-install" && !value.canInstall && (
        <p>
          The verified CEF package is staged. Automatic replacement requires a
          packaged Windows installation. Development builds and other platforms
          use manual installation.
        </p>
      )}
      {release?.notes && (
        <details className="release-notes">
          <summary>Release notes</summary>
          <p>{release.notes}</p>
        </details>
      )}
    </section>
  );
}
export function UpdateHistory({
  id,
  close,
}: {
  id?: string;
  close: () => void;
}) {
  const { state, busy, action } = useLauncher();
  const rows = (state.updateHistory ?? [])
    .filter((row) => !id || row.moduleId === id)
    .slice()
    .reverse();
  return (
    <Modal title="Update history" close={close}>
      <button
        disabled={busy || !rows.length}
        onClick={() => {
          if (window.confirm("Clear these update-history records?"))
            void action("updates.clearHistory", { id: id ?? "" });
        }}
      >
        Clear update history
      </button>
      {!rows.length && <p>No update activity recorded.</p>}
      {rows.map((row) => (
        <article className="card" key={row.id}>
          <strong>
            {row.moduleId} · {row.action}
          </strong>
          <p>
            {row.message} · {row.outcome}
          </p>
          <small>{new Date(Number(row.time)).toLocaleString()}</small>
        </article>
      ))}
    </Modal>
  );
}
export function UpdatesPanel() {
  const { state, busy, action } = useLauncher();
  const [history, setHistory] = useState(false);
  return (
    <section className="card">
      <h3>Launcher updates</h3>
      <UpdateControls id="launcher" name="Xenon Launcher" launcher />
      <div className="actions">
        <button
          disabled={busy || !state.capabilities.online}
          onClick={() => void action("modules.checkAllUpdates")}
        >
          Check all module updates
        </button>
        <button onClick={() => setHistory(true)}>Update history</button>
      </div>
      {!state.capabilities.online && (
        <p>Online services are disabled in this launcher session.</p>
      )}
      {history && <UpdateHistory close={() => setHistory(false)} />}
    </section>
  );
}
export function ModuleCatalogue({ close }: { close: () => void }) {
  const { state, busy, action } = useLauncher();
  const [query, setQuery] = useState("");
  const [history, setHistory] = useState<string>();
  const catalog = state.catalog;
  const entries = (catalog?.entries ?? []).filter((entry) =>
    `${entry.name} ${entry.description} ${entry.publisher}`
      .toLowerCase()
      .includes(query.toLowerCase()),
  );
  return (
    <Modal title="Module catalogue" close={close}>
      <div className="actions">
        <input
          type="search"
          aria-label="Search module catalogue"
          placeholder="Search modules"
          value={query}
          onChange={(event) => setQuery(event.target.value)}
        />
        <button
          disabled={
            busy || !state.capabilities.online || catalog?.state === "loading"
          }
          onClick={() => void action("catalog.refresh")}
        >
          Refresh catalogue
        </button>
      </div>
      <p role="status">
        {catalog?.state === "loading"
          ? "Refreshing the official registry…"
          : catalog?.state === "bundled"
            ? "Showing the bundled registry snapshot. Refresh for current information."
            : catalog?.state === "offline"
              ? "Showing saved registry information."
              : catalog?.state === "partial"
                ? "Some registry entries could not be refreshed."
                : `${entries.length} modules · ${state.capabilities.networkFixture ? "Network fixture" : "Official Xenon Modules registry"}`}
      </p>
      {catalog?.lastError && (
        <p className="update-error">{catalog.lastError}</p>
      )}
      {!entries.length && (
        <Empty title="No matching modules">
          Refresh the catalogue or change the search.
        </Empty>
      )}
      {entries.map((entry) => (
        <article
          className="card catalog-entry"
          key={entry.id}
          aria-label={entry.name}
        >
          <CoverImage
            source={
              entry.launcher?.tileArt?.startsWith("xenon://")
                ? entry.launcher.tileArt
                : undefined
            }
            label={entry.name}
            className="catalog-cover"
          />
          <div>
            <h3>{entry.name}</h3>
            <p>{entry.description}</p>
            <small>
              {entry.publisher}
              {entry.verified ? " · Registry verified" : ""}
            </small>
            {entry.assetName ? (
              <UpdateControls id={entry.id} name={entry.name} />
            ) : (
              <p>No package is declared for {entry.hostKey}.</p>
            )}
            <div className="actions">
              <button
                disabled={busy}
                onClick={() =>
                  void action("system.openUrl", { url: entry.repositoryUrl })
                }
              >
                Project page
              </button>
              <button onClick={() => setHistory(entry.id)}>
                Update history
              </button>
            </div>
          </div>
        </article>
      ))}
      {history && (
        <UpdateHistory id={history} close={() => setHistory(undefined)} />
      )}
    </Modal>
  );
}

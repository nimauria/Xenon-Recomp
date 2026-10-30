import { t, tText, language } from "../localization";
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
    <section
      className="update-controls"
      aria-label={t("ui.52d568bf50bb", { arg0: name })}
    >
      <p role={value?.busy ? "status" : undefined}>
        {value
          ? tText(descriptions[value.state] ?? value.state)
          : t("ui.cae41b68a298")}
      </p>
      {release && (
        <p>
          {t("ui.333771bad487")}
          <strong>{release.version}</strong>
          {release.prerelease ? t("ui.81511a8a149c") : ""}
          {value?.installedVersion
            ? t("ui.b9d90b0a2a53", { arg0: value.installedVersion })
            : ""}
        </p>
      )}
      {value?.lastError && <p className="update-error">{tText(value.lastError)}</p>}
      <div className="actions">
        <button
          disabled={busy || !value?.canCheck}
          onClick={() =>
            void action(launcher ? "updates.check" : "modules.checkUpdate", {
              id,
            })
          }
        >
          {t("ui.f26f3275e735")}
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
            {t("ui.f808eebbf4fb")}
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
                    ? t("ui.b3544960eb55", { arg0: release?.version })
                    : t("ui.8989db5eff99", {
                        arg0: name,
                        arg1: release?.version,
                      }),
                )
              )
                void action(
                  launcher ? "updates.install" : "modules.installDownloaded",
                  { id },
                );
            }}
          >
            {t("ui.7d10ba698d22")}
          </button>
        )}
        {release && (
          <button
            disabled={busy}
            onClick={() => void action("system.openUrl", { url: release.url })}
          >
            {t("ui.6ba4c7ebdff7")}
          </button>
        )}
      </div>
      {launcher && value?.state === "ready-to-install" && !value.canInstall && (
        <p>{t("ui.6fd04b3dc33e")}</p>
      )}
      {release?.notes && (
        <details className="release-notes">
          <summary>{t("ui.1254829bd999")}</summary>
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
    <Modal title={t("ui.c665338a9287")} close={close}>
      <button
        disabled={busy || !rows.length}
        onClick={() => {
          if (window.confirm(t("ui.c93ca52ffd5d")))
            void action("updates.clearHistory", { id: id ?? "" });
        }}
      >
        {t("ui.9ddc0b95903a")}
      </button>
      {!rows.length && <p>{t("ui.62d510a3dd71")}</p>}
      {rows.map((row) => (
        <article className="card" key={row.id}>
          <strong>
            {row.moduleId} · {row.action}
          </strong>
          <p>
            {row.message} · {row.outcome}
          </p>
          <small>{new Date(Number(row.time)).toLocaleString(language())}</small>
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
      <h3>{t("ui.f614e9e1ecaf")}</h3>
      <UpdateControls id="launcher" name="Xenon Launcher" launcher />
      <div className="actions">
        <button
          disabled={busy || !state.capabilities.online}
          onClick={() => void action("modules.checkAllUpdates")}
        >
          {t("ui.40e3ee18b30a")}
        </button>
        <button onClick={() => setHistory(true)}>{t("ui.c665338a9287")}</button>
      </div>
      {!state.capabilities.online && <p>{t("ui.cbe84cdaa82e")}</p>}
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
    <Modal title={t("ui.7d9e69313fa6")} close={close}>
      <div className="actions">
        <input
          type="search"
          aria-label={t("ui.123997597a9c")}
          placeholder={t("ui.a67f6147d439")}
          value={query}
          onChange={(event) => setQuery(event.target.value)}
        />
        <button
          disabled={
            busy || !state.capabilities.online || catalog?.state === "loading"
          }
          onClick={() => void action("catalog.refresh")}
        >
          {t("ui.4bc465d2f66c")}
        </button>
      </div>
      <p role="status">
        {catalog?.state === "loading"
          ? t("ui.602169ad67e9")
          : catalog?.state === "bundled"
            ? t("ui.9ef0fe728df1")
            : catalog?.state === "offline"
              ? t("ui.fd60015b17a7")
              : catalog?.state === "partial"
                ? t("ui.c9e73e26cbf6")
                : t("ui.8840b2d1dc40", {
                    arg0: entries.length,
                    arg1: state.capabilities.networkFixture
                      ? "Network fixture"
                      : "Official Xenon Modules registry",
                  })}
      </p>
      {catalog?.lastError && (
        <p className="update-error">{tText(catalog.lastError)}</p>
      )}
      {!entries.length && (
        <Empty title={t("ui.ecee6d9c672d")}>{t("ui.82250b05afa4")}</Empty>
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
              {entry.verified ? t("ui.637fe328ab16") : ""}
            </small>
            {entry.assetName ? (
              <UpdateControls id={entry.id} name={entry.name} />
            ) : (
              <p>
                {t("ui.85a63e8b5ea3")}
                {entry.hostKey}.
              </p>
            )}
            <div className="actions">
              <button
                disabled={busy}
                onClick={() =>
                  void action("system.openUrl", { url: entry.repositoryUrl })
                }
              >
                {t("ui.d6e6cdd8433d")}
              </button>
              <button onClick={() => setHistory(entry.id)}>
                {t("ui.c665338a9287")}
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

import { useState } from "react";
import { isActiveJob } from "../jobs";
import { useLauncher } from "./context";
export function LegacyMigration() {
  const { state, busy, action, pick } = useLauncher();
  const [replaceSettings, replace] = useState(false);
  const [activateProfile, activate] = useState(true);
  const [path, setPath] = useState("");
  const review = state.migration;
  const operation = state.jobs
    ?.slice()
    .reverse()
    .find((job) => job.kind?.startsWith("migration."));
  const running = Boolean(operation && isActiveJob(operation));
  return (
    <section className="legacy-migration" aria-label="Previous launcher data">
      <h3>Bring your previous launcher data across</h3>
      <p>
        Review profiles, avatars, themes, settings and library/module references
        before importing. The previous launcher's files stay in place. A backup
        of this launcher's state is saved before import.
      </p>
      <label>
        Previous data folder
        <input
          readOnly
          aria-label="Previous data folder"
          value={path}
          placeholder="Discover the previous launcher's folders automatically"
        />
      </label>
      <div className="actions">
        <button
          disabled={busy || running || state.mode !== "native"}
          onClick={async () => {
            const [folder] = await pick("folder");
            if (folder) setPath(folder);
          }}
        >
          Choose copied data folder
        </button>
        {path && (
          <button disabled={running} onClick={() => setPath("")}>
            Use automatic discovery
          </button>
        )}
      </div>
      <label className="row">
        Replace settings already saved in this launcher
        <input
          type="checkbox"
          checked={replaceSettings}
          disabled={running}
          onChange={(event) => replace(event.target.checked)}
        />
      </label>
      <label className="row">
        Use the previous active profile after import
        <input
          type="checkbox"
          checked={activateProfile}
          disabled={running}
          onChange={(event) => activate(event.target.checked)}
        />
      </label>
      <button
        disabled={busy || running || state.mode !== "native"}
        onClick={() =>
          void action("migration.inspect", {
            path,
            replaceSettings,
            activateProfile,
          })
        }
      >
        Review previous launcher data
      </button>
      {operation && (
        <p role="status">
          {operation.title}: {operation.phase ?? operation.state}
          {operation.error ? ` — ${operation.error}` : ""}
        </p>
      )}
      {review && (
        <div className="card">
          <h3>
            {review.state === "completed"
              ? "Previous data imported"
              : "Import review"}
          </h3>
          <p className="path">Source: {review.source}</p>
          <p>
            {review.counts.profiles} profiles · {review.counts.avatars} avatars
            · {review.counts.games} games · {review.counts.modules} modules ·{" "}
            {review.counts.settings} settings · {review.counts.skipped} existing
            entries/settings kept
          </p>
          <p>
            {review.replaceSettings
              ? "Reviewed settings replace current values."
              : "Current settings are kept where a value is already saved."}{" "}
            {review.activateProfile
              ? "The previous active profile will be selected."
              : "Your current active profile will be kept."}
          </p>
          {review.changes &&
            Object.entries(review.changes).map(
              ([kind, values]) =>
                values.length > 0 && (
                  <details key={kind}>
                    <summary>
                      {kind[0].toUpperCase() + kind.slice(1)} ({values.length})
                    </summary>
                    <ul>
                      {values.map((value, index) => (
                        <li key={index}>{value}</li>
                      ))}
                    </ul>
                  </details>
                ),
            )}
          {review.warnings.length > 0 && (
            <details open>
              <summary>
                Items needing attention ({review.warnings.length})
              </summary>
              <ul>
                {review.warnings.map((warning, index) => (
                  <li key={index}>{warning}</li>
                ))}
              </ul>
            </details>
          )}
          {review.state === "review" && (
            <button
              disabled={busy || running}
              onClick={() => {
                if (
                  window.confirm(
                    "Import the reviewed data? A backup of the new launcher state will be saved first. Original files will be retained.",
                  )
                )
                  void action("migration.apply", { id: review.id });
              }}
            >
              Import reviewed data
            </button>
          )}
          {review.backupPath && (
            <p className="path">State backup: {review.backupPath}</p>
          )}
        </div>
      )}
    </section>
  );
}

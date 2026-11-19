import { t } from "../localization";
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
    <section className="legacy-migration" aria-label={t("ui.2b36f4e9e378")}>
      <h3>{t("ui.6e2c3ade539e")}</h3>
      <p>{t("ui.ddf90d127ae5")}</p>
      <label>
        {t("ui.4f5809e124df")}
        <input
          readOnly
          aria-label={t("ui.4f5809e124df")}
          value={path}
          placeholder={t("ui.a49daf7801a9")}
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
          {t("ui.92bdd5d642da")}
        </button>
        {path && (
          <button disabled={running} onClick={() => setPath("")}>
            {t("ui.eebd4ff210fc")}
          </button>
        )}
      </div>
      <label className="row">
        {t("ui.e52a68807f3c")}
        <input
          type="checkbox"
          checked={replaceSettings}
          disabled={running}
          onChange={(event) => replace(event.target.checked)}
        />
      </label>
      <label className="row">
        {t("ui.4598213400ff")}
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
        {t("ui.3eaba41e5aed")}
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
              ? t("ui.e21ebbdc943f")
              : t("ui.4f7928797414")}
          </h3>
          <p className="path">
            {t("ui.1a5ac0bd0b6b")}
            {review.source}
          </p>
          <p>
            {review.counts.profiles}
            {t("ui.8c2485f7611b")}
            {review.counts.avatars}
            {t("ui.61616fcf6289")}
            {review.counts.games}
            {t("ui.9a11553bae04")}
            {review.counts.modules}
            {t("ui.d4ec5524cd37")} {review.counts.settings}
            {t("ui.508d23934638")}
            {review.counts.skipped}
            {t("ui.31e4ef835989")}
          </p>
          <p>
            {review.replaceSettings
              ? t("ui.1b9f34d7e56f")
              : t("ui.86a0285e4831")}{" "}
            {review.activateProfile
              ? t("ui.8b0fda52673c")
              : t("ui.8ac15bc76025")}
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
                {t("ui.945f85c9e672")}
                {review.warnings.length})
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
                if (window.confirm(t("ui.f823ce143012")))
                  void action("migration.apply", { id: review.id });
              }}
            >
              {t("ui.0f307bec8476")}
            </button>
          )}
          {review.backupPath && (
            <p className="path">
              {t("ui.f9abb1b089f6")}
              {review.backupPath}
            </p>
          )}
        </div>
      )}
    </section>
  );
}

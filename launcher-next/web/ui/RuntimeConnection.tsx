import { useEffect, useState } from "react";
import { t } from "../localization";
import { sessionActive } from "../session";
import { useLauncher } from "./context";
export function RuntimeConnection() {
  const { state, busy, action, pick } = useLauncher();
  const connection = state.runtimeConnection;
  const [path, setPath] = useState(connection?.configuredPath ?? "");
  useEffect(
    () => setPath(connection?.configuredPath ?? ""),
    [connection?.configuredPath],
  );
  const locked =
    busy ||
    !connection ||
    connection.locked ||
    sessionActive(state.session?.state);
  return (
    <section
      className="card runtime-connection"
      aria-label={t("runtime.connection")}
    >
      <h3>{t("runtime.connection")}</h3>
      <p>{t("runtime.description")}</p>
      <p role="status">
        {t(
          connection?.available
            ? connection.fixture
              ? "runtime.fixture"
              : "runtime.available"
            : "runtime.missing",
        )}
      </p>
      {connection?.effectivePath && (
        <p className="path">{connection.effectivePath}</p>
      )}
      <label>
        {t("runtime.executable")}
        <input
          value={path}
          disabled={locked}
          onChange={(event) => setPath(event.target.value)}
          placeholder={t("runtime.auto")}
        />
      </label>
      <div className="actions">
        <button
          disabled={locked}
          onClick={async () => {
            const [selected] = await pick("open");
            if (selected) setPath(selected);
          }}
        >
          {t("runtime.browse")}
        </button>
        <button
          disabled={locked || path === connection?.configuredPath}
          onClick={() => void action("runtime.configure", { path })}
        >
          {t("runtime.save")}
        </button>
        <button
          disabled={locked || !connection?.configuredPath}
          onClick={() => void action("runtime.configure", { path: "" })}
        >
          {t("runtime.reset")}
        </button>
      </div>
      {connection?.locked && <p>{t("runtime.locked")}</p>}
      {sessionActive(state.session?.state) && <p>{t("runtime.active")}</p>}
      <small>{t("runtime.services")}</small>
    </section>
  );
}

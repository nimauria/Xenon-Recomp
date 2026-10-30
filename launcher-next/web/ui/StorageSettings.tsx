import { t, tText } from "../localization";
import { useLauncher } from "./context";
export function StorageSettings() {
  const { state, busy, action, pick } = useLauncher();
  if (!state.locations) return <p>{t("ui.99f50b5ec22a")}</p>;
  return (
    <section aria-label={t("ui.f8e839d0b175")}>
      <p>{t("ui.47e5ce1a83a5")}</p>
      {state.locations.destinations.map((location) => (
        <article
          className="card"
          key={location.id}
          aria-label={t("storage.destination", { name: tText(location.label) })}
        >
          <h3>{tText(location.label)}</h3>
          {location.description && <p>{tText(location.description)}</p>}
          {location.id === "cache" && state.cache?.pending && (
            <p role="status">{t("ui.04ebeb9889a7")}</p>
          )}
          {location.id === "cache" &&
            Boolean(state.cache?.previousLocations.length) && (
              <details>
                <summary>
                  {t("ui.cdfab0b95864")}
                  {state.cache?.previousLocations.length})
                </summary>
                <p>{t("ui.a50ab1ef5df9")}</p>
                {state.cache?.previousLocations.map((path) => {
                  const separator = path.includes("\\") ? "\\" : "/";
                  const base = path.replace(/[\\/]+$/, "");
                  return (
                    <div key={path}>
                      <p className="path">
                        {base}
                        {separator}catalog-art
                      </p>
                      <p className="path">
                        {base}
                        {separator}downloads
                      </p>
                    </div>
                  );
                })}
              </details>
            )}
          <p className="path">{location.effectivePath}</p>
          <small>
            {location.configuredPath && !location.invalid
              ? t("ui.6b2cfac810c4")
              : t("ui.83a1853b9f77")}
          </small>
          {location.configuredPath && (
            <p className="path">
              {t("ui.bc05325a8849")}
              {location.configuredPath}
            </p>
          )}
          {location.invalid && <p role="alert">{t("ui.41c950c19419")}</p>}
          <details>
            <summary>{t("ui.40d8d52d9e35")}</summary>
            <p className="path">{location.defaultPath}</p>
          </details>
          <div className="actions">
            <button
              disabled={
                busy ||
                state.mode !== "native" ||
                (location.id === "cache" && state.cache?.pending)
              }
              onClick={async () => {
                const [path] = await pick("folder");
                if (path)
                  await action("settings.set", {
                    key: location.key,
                    value: path,
                  });
              }}
            >
              {t("storage.choose", {
                name: tText(location.label).toLocaleLowerCase(),
              })}
            </button>
            <button
              disabled={
                busy ||
                state.mode !== "native" ||
                !location.configuredPath ||
                (location.id === "cache" && state.cache?.pending)
              }
              onClick={() =>
                void action("settings.resetKey", { key: location.key })
              }
            >
              {t("storage.default", {
                name: tText(location.label).toLocaleLowerCase(),
              })}
            </button>
            <button
              disabled={busy || state.mode !== "native"}
              onClick={() =>
                void action("system.openPath", {
                  target: "location",
                  id: location.id,
                })
              }
            >
              {t("storage.open", {
                name: tText(location.label).toLocaleLowerCase(),
              })}
            </button>
          </div>
        </article>
      ))}
      <h3>{t("ui.a9ae7a6fbf7b")}</h3>
      {state.locations.fixed.map((location) => (
        <article
          className="card"
          key={location.id}
          aria-label={t("storage.location", { name: tText(location.label) })}
        >
          <h4>{tText(location.label)}</h4>
          <p className="path">{location.path}</p>
          <p>{tText(location.description)}</p>
          {location.legacyConfiguredPath && (
            <p className="path">
              {t("ui.d5ebdd2de6f3")} {location.legacyConfiguredPath}
              {t("ui.00ef34a4c87f")}
            </p>
          )}
          <div className="actions">
            <button
              disabled={busy || state.mode !== "native"}
              onClick={() =>
                void action("system.openPath", {
                  target: "location",
                  id: location.id,
                })
              }
            >
              {t("storage.openLocation", {
                name: tText(location.label).toLocaleLowerCase(),
              })}
            </button>
            {location.legacyConfiguredPath && location.legacyKey && (
              <button
                disabled={busy || state.mode !== "native"}
                onClick={() =>
                  void action("settings.resetKey", { key: location.legacyKey })
                }
              >
                {t("storage.clear", {
                  name: tText(location.label).toLocaleLowerCase(),
                })}
              </button>
            )}
          </div>
        </article>
      ))}
    </section>
  );
}

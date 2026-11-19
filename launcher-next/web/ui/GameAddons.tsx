import { t } from "../localization";
import { useEffect, useState } from "react";
import type { Dlc, Game } from "../../contracts/launcher";
import { useLauncher } from "./context";
export function GameAddons({ game }: { game: Game }) {
  const { state, busy, action, pick } = useLauncher();
  const [query, setQuery] = useState(""),
    [limit, setLimit] = useState(32);
  useEffect(() => {
    setQuery("");
    setLimit(32);
  }, [game.id]);
  const definitions = state.dlcCatalog?.[game.moduleId ?? ""] ?? [];
  const local = game.dlc ?? [];
  const declared = new Set(definitions.map((d) => d.dlcId));
  const rows = [
    ...definitions.map((definition) => ({
      key: `catalog-${definition.dlcId}`,
      name: definition.name,
      description: definition.description,
      group: definition.group,
      category: definition.category,
      catalogId: definition.dlcId,
      local: local.find((d) => d.catalogId === definition.dlcId),
    })),
    ...local
      .filter((d) => !d.catalogId || !declared.has(d.catalogId))
      .map((d) => ({
        key: d.id,
        name: d.name,
        description: "Local add-on content",
        group: "",
        category: "",
        catalogId: undefined,
        local: d,
      })),
  ].filter(
    (row) =>
      (row.local ||
        state.settings?.["library/missingContent"] !==
          "Hide missing content") &&
      `${row.name} ${row.description} ${row.group}`
        .toLowerCase()
        .includes(query.toLowerCase()),
  );
  async function importContent(
    catalogId?: string,
    folder = false,
    replacing = false,
  ) {
    if (replacing && !window.confirm(t("ui.be947aefcca2"))) return;
    const paths = await pick(folder ? "folder" : "multiple");
    if (paths.length)
      await action("dlc.import", {
        id: game.id,
        paths,
        ...(catalogId ? { catalogId } : {}),
      });
  }
  function remove(dlc: Dlc) {
    if (
      window.confirm(
        dlc.managed
          ? t("ui.7ce0ef93e717", { arg0: dlc.name })
          : t("ui.543ca167c932", { arg0: dlc.name }),
      )
    )
      void action("dlc.remove", { id: game.id, dlcId: dlc.id });
  }
  return (
    <section aria-label={t("ui.6b478486ef01")}>
      <h3>{t("ui.ccfc53fcbd49")}</h3>
      <p>{t("ui.4746843e3ae2")}</p>
      <label>
        {t("ui.eb005f7b45f6")}
        <input
          type="search"
          value={query}
          onChange={(e) => {
            setQuery(e.target.value);
            setLimit(32);
          }}
        />
      </label>
      <div className="dlc-list">
        {!rows.length && <p>{t("ui.04c3d0e1cc24")}</p>}
        {rows.slice(0, limit).map((row) => (
          <article
            className="addon-row"
            key={row.key}
            data-addon-id={row.catalogId ?? row.local?.id}
          >
            <h4>{row.name}</h4>
            {(row.group || row.category) && (
              <small>
                {[row.group, row.category].filter(Boolean).join(" · ")}
              </small>
            )}
            {row.description && <p>{row.description}</p>}
            <small>
              {row.local
                ? row.local.receiptVerified
                  ? t("ui.808fdc6c3383")
                  : t("ui.c4833139620e")
                : t("ui.a37f7e6bfe00")}
            </small>
            <div className="actions">
              {row.catalogId && (
                <>
                  <button
                    disabled={busy || state.mode !== "native"}
                    onClick={() =>
                      void importContent(
                        row.catalogId,
                        false,
                        Boolean(row.local),
                      )
                    }
                  >
                    {row.local ? t("ui.69d8c38cfb86") : t("ui.824ca255db35")}
                  </button>
                  <button
                    disabled={busy || state.mode !== "native"}
                    onClick={() =>
                      void importContent(
                        row.catalogId,
                        true,
                        Boolean(row.local),
                      )
                    }
                  >
                    {row.local ? t("ui.499d403c8a3a") : t("ui.b849e38fdb56")}
                  </button>
                </>
              )}
              {row.local && (
                <>
                  <button
                    disabled={busy || state.mode !== "native"}
                    onClick={() =>
                      void action("system.openPath", {
                        target: "dlc",
                        id: game.id,
                        dlcId: row.local!.id,
                      })
                    }
                  >
                    {t("ui.6a9084025c4b")}
                  </button>
                  <button
                    disabled={busy || state.mode !== "native"}
                    onClick={() =>
                      void action("dlc.verify", {
                        id: game.id,
                        dlcId: row.local!.id,
                      })
                    }
                  >
                    {row.local.managed
                      ? t("ui.530ed4fd16b8")
                      : t("ui.9256cd68ef7e")}
                  </button>
                  <button
                    disabled={busy || state.mode !== "native"}
                    onClick={() => remove(row.local!)}
                  >
                    {row.local.managed
                      ? t("ui.dfe8945029b7")
                      : t("ui.cbeda6a6baa8")}
                  </button>
                </>
              )}
            </div>
          </article>
        ))}
      </div>
      {rows.length > limit && (
        <button onClick={() => setLimit((value) => value + 32)}>
          {t("ui.7bdd6b182da3")}
        </button>
      )}
      <div className="actions">
        <button
          disabled={busy || state.mode !== "native"}
          onClick={() => void importContent()}
        >
          {t("ui.eef80523f9c0")}
        </button>
        <button
          disabled={busy || state.mode !== "native"}
          onClick={() => void importContent(undefined, true)}
        >
          {t("ui.3dd48064b639")}
        </button>
      </div>
    </section>
  );
}

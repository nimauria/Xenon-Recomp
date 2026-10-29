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
    if (
      replacing &&
      !window.confirm(
        "Replace this add-on's managed copy? Originals are kept, and the previous managed copy is retained until you delete this add-on's managed files.",
      )
    )
      return;
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
          ? `Permanently delete all managed copies of “${dlc.name}”? Original source files are kept.`
          : `Remove the reference to “${dlc.name}”? Original files are kept.`,
      )
    )
      void action("dlc.remove", { id: game.id, dlcId: dlc.id });
  }
  return (
    <section aria-label="Game add-ons">
      <h3>Add-ons</h3>
      <p>
        Supply your own content. Selecting a catalogue entry records your
        choice; content identity and compatibility still require the Xenon
        adapter.
      </p>
      <label>
        Search add-ons
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
        {!rows.length && <p>No add-ons match this view.</p>}
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
                  ? "Import receipt matched at last check"
                  : "Imported · content identity not verified"
                : "Not imported"}
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
                    {row.local ? "Replace files" : "Import files"}
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
                    {row.local ? "Replace folder" : "Import folder"}
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
                    Open folder
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
                    {row.local.managed ? "Check copy" : "Check source"}
                  </button>
                  <button
                    disabled={busy || state.mode !== "native"}
                    onClick={() => remove(row.local!)}
                  >
                    {row.local.managed
                      ? "Delete managed copy"
                      : "Remove reference"}
                  </button>
                </>
              )}
            </div>
          </article>
        ))}
      </div>
      {rows.length > limit && (
        <button onClick={() => setLimit((value) => value + 32)}>
          Show more add-ons
        </button>
      )}
      <div className="actions">
        <button
          disabled={busy || state.mode !== "native"}
          onClick={() => void importContent()}
        >
          Copy local DLC
        </button>
        <button
          disabled={busy || state.mode !== "native"}
          onClick={() => void importContent(undefined, true)}
        >
          Copy local DLC folder
        </button>
      </div>
    </section>
  );
}

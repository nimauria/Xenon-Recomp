import { t } from "../localization";
import { useEffect, useState } from "react";
import type { Game, GameMedia as Media } from "../../contracts/launcher";
import { useLauncher } from "./context";
function fields(media: Media) {
  return {
    name: media.name,
    discNumber: media.discNumber,
    discCount: media.discCount,
    contentMediaId: media.mediaId,
    region: media.region,
  };
}
function MediaEditor({
  game,
  medium,
  reportDirty,
  locked,
}: {
  game: Game;
  medium: Media;
  locked: boolean;
  reportDirty: (dirty: boolean) => void;
}) {
  const { busy, state, action } = useLauncher();
  const [draft, setDraft] = useState(() => fields(medium)),
    [saved, setSaved] = useState(() => fields(medium));
  const dirty = JSON.stringify(draft) !== JSON.stringify(saved);
  useEffect(() => {
    reportDirty(dirty);
  }, [dirty, reportDirty]);
  useEffect(() => () => reportDirty(false), [reportDirty]);
  return (
    <fieldset disabled={locked || busy || state.mode !== "native"}>
      <legend>{t("ui.cbbda500412f")}</legend>
      <label>
        {t("ui.83541e75ab8f")}
        <input
          value={draft.name}
          maxLength={512}
          onChange={(e) => setDraft({ ...draft, name: e.target.value })}
        />
      </label>
      <label>
        {t("ui.0fd0aeff1065")}
        <input
          type="number"
          min={0}
          max={99}
          value={draft.discNumber}
          onChange={(e) =>
            setDraft({ ...draft, discNumber: Number(e.target.value) })
          }
        />
      </label>
      <label>
        {t("ui.7a5d46c31911")}
        <input
          type="number"
          min={0}
          max={99}
          value={draft.discCount}
          onChange={(e) =>
            setDraft({ ...draft, discCount: Number(e.target.value) })
          }
        />
      </label>
      <label>
        {t("ui.35d2da46d091")}
        <input
          value={draft.contentMediaId}
          maxLength={512}
          onChange={(e) =>
            setDraft({ ...draft, contentMediaId: e.target.value })
          }
        />
      </label>
      <label>
        {t("ui.cbf17390a421")}
        <input
          value={draft.region}
          maxLength={512}
          onChange={(e) => setDraft({ ...draft, region: e.target.value })}
        />
      </label>
      <small>{t("ui.9b34627ab4ea")}</small>
      <button
        type="button"
        disabled={
          !dirty ||
          !Number.isInteger(draft.discNumber) ||
          !Number.isInteger(draft.discCount) ||
          draft.discNumber < 0 ||
          draft.discNumber > 99 ||
          draft.discCount < 0 ||
          draft.discCount > 99 ||
          (draft.discCount > 0 && draft.discNumber > draft.discCount)
        }
        onClick={async () => {
          if (
            await action("library.media.update", {
              id: game.id,
              mediaId: medium.id,
              ...draft,
            })
          )
            setSaved(draft);
        }}
      >
        {t("ui.42d9c4ea755c")}
      </button>
    </fieldset>
  );
}
export function GameMedia({
  game,
  reportDirty,
}: {
  game: Game;
  reportDirty: (dirty: boolean) => void;
}) {
  const { state, busy, action, pick } = useLauncher();
  const [selected, setSelected] = useState(
      game.media?.find((m) => m.isPrimary)?.id ?? "",
    ),
    [dirty, setDirty] = useState(false),
    [mode, setMode] = useState("keep");
  const running = Boolean(
    state.session?.gameId === game.id &&
    !["idle", "unavailable", "failed", "stopped"].includes(state.session.state),
  );
  const locked = busy || state.mode !== "native" || running;
  const medium = game.media?.find((m) => m.id === selected) ?? game.media?.[0];
  useEffect(() => reportDirty(dirty), [dirty, reportDirty]);
  async function add(folder: boolean) {
    const paths = await pick(folder ? "folder" : "multiple");
    if (paths.length)
      await action("library.media.add", { id: game.id, paths, mode });
  }
  return (
    <section aria-label={t("ui.dc3a53a0ac3f")}>
      <h3>{t("ui.58bd8158205b")}</h3>
      <p>{t("ui.b9614dcf4a88")}</p>
      {running && <p>{t("ui.f5dfaef9d539")}</p>}
      <div className="media-list">
        {game.media?.map((m) => (
          <article key={m.id} data-media-id={m.id}>
            <strong>
              {m.name || t("ui.a66c94c699e9")}
              {m.isPrimary ? t("ui.b88564376634") : ""}
            </strong>
            <p className="path">{m.sourcePath}</p>
            {m.missing && <p role="status">{t("ui.7a100009759b")}</p>}
            <small>
              {m.discNumber
                ? t("ui.2edc7f46758d", {
                    arg0: m.discNumber,
                    arg1: m.discCount ? ` of ${m.discCount}` : "",
                  })
                : t("ui.50238108414b")}{" "}
              · {m.importMode} · {m.region || t("ui.d572c90ff329")}
            </small>
            <div className="actions">
              <button
                type="button"
                disabled={locked || m.isPrimary}
                onClick={() =>
                  void action("library.media.select", {
                    id: game.id,
                    mediaId: m.id,
                  })
                }
              >
                {t("ui.317ec528404e")}
              </button>
              <button
                type="button"
                disabled={locked}
                onClick={() =>
                  void action("system.openPath", {
                    target: "media",
                    id: game.id,
                    mediaId: m.id,
                  })
                }
              >
                {t("ui.e966201f0397")}
              </button>
              <button
                type="button"
                disabled={busy || state.mode !== "native"}
                onClick={() =>
                  void action("library.media.check", {
                    id: game.id,
                    mediaId: m.id,
                  })
                }
              >
                {t("ui.359adde4f7ec")}
              </button>
              {!game.managedCopies?.some(
                (copy) => copy.type === "game" && copy.entryId === m.id,
              ) ? (
                <>
                  <button
                    type="button"
                    disabled={busy || state.mode !== "native"}
                    onClick={() => {
                      if (
                        window.confirm(t("ui.75796215682f", { arg0: m.name }))
                      )
                        void action("library.transfer", {
                          id: game.id,
                          mediaId: m.id,
                          path: m.sourcePath,
                          type: "game",
                          mode: "move",
                        });
                    }}
                  >
                    {t("ui.9275387c04a3")}
                  </button>
                  <button
                    type="button"
                    disabled={locked || (game.media?.length ?? 0) < 2}
                    onClick={() => {
                      if (
                        window.confirm(t("ui.1c4eacfe586e", { arg0: m.name }))
                      )
                        void action("library.media.remove", {
                          id: game.id,
                          mediaId: m.id,
                        });
                    }}
                  >
                    {t("ui.fcfa948617c5")}
                  </button>
                </>
              ) : (
                <button
                  type="button"
                  disabled={locked}
                  onClick={() => {
                    if (window.confirm(t("ui.ff52ec74e500", { arg0: m.name })))
                      void action("library.deleteManaged", {
                        id: game.id,
                        mediaId: m.id,
                      });
                  }}
                >
                  {t("ui.a344eb83c40a")}
                </button>
              )}
            </div>
          </article>
        ))}
      </div>
      {medium && (
        <>
          <label>
            {t("ui.d4838f73ce43")}
            <select
              value={medium.id}
              onChange={(e) => {
                if (!dirty || window.confirm(t("ui.342b9357badc")))
                  setSelected(e.target.value);
              }}
            >
              {game.media?.map((m) => (
                <option key={m.id} value={m.id}>
                  {m.name || m.sourcePath}
                </option>
              ))}
            </select>
          </label>
          <MediaEditor
            locked={locked}
            key={medium.id}
            game={game}
            medium={medium}
            reportDirty={setDirty}
          />
        </>
      )}
      <label>
        {t("ui.fb292d47c073")}
        <select value={mode} onChange={(e) => setMode(e.target.value)}>
          <option value="keep">{t("ui.afa4fa63b6f3")}</option>
          <option value="move">{t("ui.0e965b9b8ada")}</option>
        </select>
      </label>
      <div className="actions">
        <button
          type="button"
          disabled={busy || state.mode !== "native"}
          onClick={() => void add(false)}
        >
          {t("ui.30ba51ead609")}
        </button>
        <button
          type="button"
          disabled={busy || state.mode !== "native"}
          onClick={() => void add(true)}
        >
          {t("ui.509b414451bd")}
        </button>
      </div>
    </section>
  );
}

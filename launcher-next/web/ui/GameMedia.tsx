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
      <legend>Disc details</legend>
      <label>
        Media name
        <input
          value={draft.name}
          maxLength={512}
          onChange={(e) => setDraft({ ...draft, name: e.target.value })}
        />
      </label>
      <label>
        Disc number
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
        Disc count
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
        Media identifier
        <input
          value={draft.contentMediaId}
          maxLength={512}
          onChange={(e) =>
            setDraft({ ...draft, contentMediaId: e.target.value })
          }
        />
      </label>
      <label>
        Media region
        <input
          value={draft.region}
          maxLength={512}
          onChange={(e) => setDraft({ ...draft, region: e.target.value })}
        />
      </label>
      <small>
        Use zero for an unspecified disc number/count. These details are
        manually recorded until a content adapter verifies them.
      </small>
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
        Save disc details
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
  const running=Boolean(state.session?.gameId===game.id && !["idle","unavailable","failed","stopped"].includes(state.session.state));
  const locked=busy||state.mode!=="native"||running;
  const medium = game.media?.find((m) => m.id === selected) ?? game.media?.[0];
  useEffect(() => reportDirty(dirty), [dirty, reportDirty]);
  async function add(folder: boolean) {
    const paths = await pick(folder ? "folder" : "multiple");
    if (paths.length)
      await action("library.media.add", { id: game.id, paths, mode });
  }
  return (
    <section aria-label="Game discs and media">
      <h3>Discs and media</h3>
      <p>
        The primary source is used when you press Play. Attach additional discs
        or regional copies to this same game entry.
      </p>
      {running&&<p>Stop the game before changing its discs or media.</p>}
      <div className="media-list">
        {game.media?.map((m) => (
          <article key={m.id} data-media-id={m.id}>
            <strong>
              {m.name || "Unnamed media"}
              {m.isPrimary ? " · Primary" : ""}
            </strong>
            <p className="path">{m.sourcePath}</p>
            {m.missing&&<p role="status">Source missing; reference kept.</p>}
            <small>
              {m.discNumber
                ? `Disc ${m.discNumber}${m.discCount ? ` of ${m.discCount}` : ""}`
                : "Disc unspecified"}{" "}
              · {m.importMode} · {m.region || "Region unspecified"}
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
                Use as primary
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
                Open media folder
              </button>
              <button type="button" disabled={busy||state.mode!=="native"} onClick={()=>void action("library.media.check",{id:game.id,mediaId:m.id})}>Check media source</button>
              {!game.managedCopies?.some(
                (copy) => copy.type === "game" && copy.entryId === m.id,
              ) ? (
                <>
                  <button
                    type="button"
                    disabled={busy || state.mode !== "native"}
                    onClick={() => {
                      if (
                        window.confirm(
                          `Move “${m.name}” into the managed library? Originals are removed only after the copy is verified.`,
                        )
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
                    Move this media
                  </button>
                  <button
                    type="button"
                    disabled={locked || (game.media?.length ?? 0) < 2}
                    onClick={() => {
                      if (
                        window.confirm(
                          `Remove the reference to “${m.name}”? Files are kept.`,
                        )
                      )
                        void action("library.media.remove", {
                          id: game.id,
                          mediaId: m.id,
                        });
                    }}
                  >
                    Remove media reference
                  </button>
                </>
              ) : (
                <button
                  type="button"
                  disabled={locked}
                  onClick={() => {
                    if (
                      window.confirm(
                        `Permanently delete managed files for “${m.name}”? Other discs and original sources are kept.`,
                      )
                    )
                      void action("library.deleteManaged", {
                        id: game.id,
                        mediaId: m.id,
                      });
                  }}
                >
                  Delete this media's managed files
                </button>
              )}
            </div>
          </article>
        ))}
      </div>
      {medium && (
        <>
          <label>
            Edit media details
            <select
              value={medium.id}
              onChange={(e) => {
                if (!dirty || window.confirm("Discard unsaved disc details?"))
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
        New media location
        <select value={mode} onChange={(e) => setMode(e.target.value)}>
          <option value="keep">Keep current location</option>
          <option value="move">Move into Xenon Library</option>
        </select>
      </label>
      <div className="actions">
        <button
          type="button"
          disabled={busy || state.mode !== "native"}
          onClick={() => void add(false)}
        >
          Add disc files
        </button>
        <button
          type="button"
          disabled={busy || state.mode !== "native"}
          onClick={() => void add(true)}
        >
          Add media folder
        </button>
      </div>
    </section>
  );
}

import { t, tText, tCount, language } from "../localization";
import { useEffect, useRef, useState } from "react";
import type { Capture } from "../../contracts/launcher";
import { Empty, Modal, useLauncher } from "./context";

function source(capture: Capture) {
  return `xenon://app/captures/${encodeURIComponent(capture.id)}?v=${encodeURIComponent(capture.version ?? "")}`;
}
export function Captures() {
  const { state, busy, query, action } = useLauncher();
  const [selected, select] = useState("");
  const [zoom, setZoom] = useState(1);
  const [error, setError] = useState("");
  const [scroll, setScroll] = useState(0);
  const viewport = useRef<HTMLDivElement>(null);
  const [width, setWidth] = useState(600);
  const [height, setHeight] = useState(580);
  const [sort, setSort] = useState("Newest first");
  const captures = (state.captures ?? [])
    .filter((capture) =>
      capture.name.toLowerCase().includes(query.trim().toLowerCase()),
    )
    .slice()
    .sort((a, b) =>
      sort === "Name"
        ? a.name.localeCompare(b.name)
        : sort === "Oldest first"
          ? (a.modified ?? 0) - (b.modified ?? 0)
          : (b.modified ?? 0) - (a.modified ?? 0),
    );
  const current = captures.find((capture) => capture.id === selected);
  const index = captures.findIndex((capture) => capture.id === selected);
  const scale = Math.max(
    1,
    Number(state.settings?.["accessibility/textScale"] ?? 1),
  );
  const columns = Math.max(1, Math.floor(width / (300 * scale)));
  const rowHeight = Math.ceil(310 * scale);
  const rows = Math.ceil(captures.length / columns);
  const start = Math.max(0, Math.floor(scroll / rowHeight) - 1);
  const end = Math.min(rows, start + Math.ceil(height / rowHeight) + 3);
  useEffect(() => {
    const node = viewport.current;
    if (!node) return;
    const observer = new ResizeObserver(() => {
      setWidth(node.clientWidth);
      setHeight(node.clientHeight);
    });
    observer.observe(node);
    return () => observer.disconnect();
  }, []);
  useEffect(() => {
    if (viewport.current) viewport.current.scrollTop = 0;
    setScroll(0);
  }, [query, sort]);
  useEffect(() => {
    const node = viewport.current;
    if (
      node &&
      node.scrollTop > Math.max(0, rows * rowHeight - node.clientHeight)
    ) {
      node.scrollTop = Math.max(0, rows * rowHeight - node.clientHeight);
      setScroll(node.scrollTop);
    }
    if (selected && !current) select("");
  }, [rows, rowHeight, selected, current]);
  function open(capture: Capture) {
    setZoom(1);
    setError("");
    select(capture.id);
  }
  function step(delta: number) {
    const capture = captures[index + delta];
    if (capture) open(capture);
  }
  function adjust(delta: number) {
    setZoom((value) => Math.min(4, Math.max(1, value + delta)));
  }
  async function remove(capture: Capture) {
    if (window.confirm(t("ui.a83bcbd031c1", { arg0: capture.name }))) {
      const ok = await action("captures.delete", { id: capture.id });
      if (ok && selected === capture.id) select("");
    }
  }
  return (
    <>
      <p>{t("ui.1006b2666c1a")}</p>
      <div className="actions">
        <label>
          {t("ui.6be090825c99")}
          <select
            aria-label={t("ui.2222e7905656")}
            value={sort}
            onChange={(event) => setSort(event.target.value)}
          >
            {["Newest first", "Oldest first", "Name"].map((value) => (
              <option key={value} value={value}>
                {tText(value)}
              </option>
            ))}
          </select>
        </label>
        <button
          disabled={busy || state.mode !== "native"}
          onClick={() => void action("captures.refresh")}
        >
          {t("ui.f33b13ab00d4")}
        </button>
        <button
          disabled={busy || state.mode !== "native"}
          onClick={() => void action("system.openPath", { target: "captures" })}
        >
          {t("ui.0e5ca87f6320")}
        </button>
        <span role="status">
          {state.captureScan?.state === "scanning"
            ? t("ui.96f218d3fa60")
            : tCount("captures.count", captures.length)}
        </span>
      </div>
      {state.captureScan?.error && (
        <p role="alert">{tText(state.captureScan.error)}</p>
      )}
      {state.captureScan?.truncated && <p>{t("ui.c3a8fbecfdfd")}</p>}
      {error && <p role="alert">{tText(error)}</p>}
      {!captures.length && state.captureScan?.state !== "scanning" && (
        <Empty title={t("ui.f499a243c542")}>{t("ui.960413011340")}</Empty>
      )}
      <div
        className="capture-grid"
        aria-label={t("ui.3ca82f15e59c")}
        ref={viewport}
        onScroll={(event) => setScroll(event.currentTarget.scrollTop)}
        style={{ display: captures.length ? undefined : "none" }}
      >
        <div style={{ height: rows * rowHeight, position: "relative" }}>
          {captures
            .slice(start * columns, end * columns)
            .map((capture, offset) => {
              const position = start * columns + offset;
              return (
                <article
                  className="card capture-card"
                  key={capture.id}
                  aria-label={capture.name}
                  style={{
                    position: "absolute",
                    top: Math.floor(position / columns) * rowHeight,
                    left: `${((position % columns) * 100) / columns}%`,
                    width: `calc(${100 / columns}% - 12px)`,
                    height: rowHeight - 12,
                  }}
                >
                  {capture.viewable !== false ? (
                    <button
                      className="capture-thumbnail"
                      aria-label={t("ui.17c73ea32664", { arg0: capture.name })}
                      onClick={() => open(capture)}
                    >
                      <img
                        src={source(capture)}
                        loading="lazy"
                        decoding="async"
                        alt={capture.name}
                      />
                    </button>
                  ) : (
                    <p>{t("ui.6e2d74fe4a38")}</p>
                  )}
                  <h3 title={capture.name}>{capture.name}</h3>
                  <small>
                    {Math.ceil(capture.size / 1024)}
                    {t("ui.ad94f7d950c4")}
                    {capture.modified
                      ? ` · ${new Date(capture.modified).toLocaleDateString(language())}`
                      : ""}
                  </small>
                  <div className="actions">
                    <button
                      disabled={capture.viewable === false}
                      onClick={() => open(capture)}
                    >
                      {t("ui.dcc839a4015c")}
                    </button>
                    <button
                      disabled={busy}
                      onClick={() => void remove(capture)}
                    >
                      {t("ui.e2d0a54968ea")}
                    </button>
                  </div>
                </article>
              );
            })}
        </div>
      </div>
      {current && (
        <Modal title={current.name} close={() => select("")}>
          <div className="actions capture-viewer-controls">
            <button
              aria-label={t("ui.69e116a2f315")}
              disabled={index <= 0}
              onClick={() => step(-1)}
            >
              {t("ui.a57b08a480b8")}
            </button>
            <span role="status">
              {index + 1}
              {t("ui.a4282e4b2298")}
              {captures.length} · {Math.round(zoom * 100)}%
            </span>
            <button
              aria-label={t("ui.f45eedac87e9")}
              disabled={index + 1 >= captures.length}
              onClick={() => step(1)}
            >
              {t("ui.1ff57a29d7c9")}
            </button>
            <button
              aria-label={t("ui.bc7b631a689b")}
              disabled={zoom <= 1}
              onClick={() => adjust(-0.25)}
            >
              −
            </button>
            <button onClick={() => setZoom(1)}>{t("ui.9f872ed43d00")}</button>
            <button
              aria-label={t("ui.0e47f09a748f")}
              disabled={zoom >= 4}
              onClick={() => adjust(0.25)}
            >
              +
            </button>
          </div>
          <div
            className="capture-stage"
            tabIndex={0}
            role="region"
            aria-label={t("ui.b9b1cf51915e")}
            onWheel={(event) => {
              if (event.ctrlKey) {
                event.preventDefault();
                adjust(event.deltaY < 0 ? 0.25 : -0.25);
              }
            }}
            onKeyDown={(event) => {
              if (event.key === "ArrowLeft") step(-1);
              else if (event.key === "ArrowRight") step(1);
              else if (event.key === "+" || event.key === "=") adjust(0.25);
              else if (event.key === "-") adjust(-0.25);
              else if (event.key === "0" || event.key === "Home") setZoom(1);
              else return;
              event.preventDefault();
              event.stopPropagation();
            }}
          >
            <img
              key={current.version + current.id}
              className="capture-image"
              src={source(current)}
              alt={current.name}
              style={{
                width: `${zoom * 100}%`,
                maxWidth: "none",
                maxHeight: "none",
              }}
              onError={() =>
                setError(
                  "This image could not be decoded. Try opening it in your image app.",
                )
              }
            />
          </div>
          <div className="actions">
            <button
              disabled={busy}
              onClick={() => void action("captures.open", { id: current.id })}
            >
              {t("ui.9887c8e8d51d")}
            </button>
            <button
              onClick={async () => {
                try {
                  await navigator.clipboard.writeText(
                    `${state.captureFolderPath ?? ""}/${current.id}`,
                  );
                } catch {
                  setError("The capture path could not be copied.");
                }
              }}
            >
              {t("ui.008fe647bbbc")}
            </button>
            <button disabled={busy} onClick={() => void remove(current)}>
              {t("ui.0b2c96558a2e")}
            </button>
          </div>
          {error && <p role="alert">{error}</p>}
        </Modal>
      )}
    </>
  );
}

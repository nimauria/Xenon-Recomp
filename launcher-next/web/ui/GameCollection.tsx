import { useEffect, useLayoutEffect, useRef, useState } from "react";
import type { Game } from "../../contracts/launcher";
import { CoverImage } from "./CoverImage";
/** Window the visible rows so a large library does not create thousands of buttons. */
export function GameCollection({
  games,
  selected,
  grid,
  select,
  scale = 1,
  density = "Auto",
  scroll,
  setScroll,
  wrap = true,
  openActions,
}: {
  games: Game[];
  selected?: string;
  grid: boolean;
  select: (id: string) => void;
  scale?: number;
  density?: string;
  scroll: number;
  setScroll: (value: number) => void;
  wrap?: boolean;
  openActions?: (game: Game, source: HTMLElement, x: number, y: number) => void;
}) {
  const viewport = useRef<HTMLDivElement>(null);
  const pendingFocus = useRef<number | undefined>(undefined);
  const [width, setWidth] = useState(300);
  useEffect(() => {
    if (viewport.current && viewport.current.scrollTop !== scroll)
      viewport.current.scrollTop = scroll;
  }, [scroll]);
  useEffect(() => {
    const node = viewport.current!;
    const observer = new ResizeObserver(() => setWidth(node.clientWidth));
    observer.observe(node);
    return () => observer.disconnect();
  }, []);
  const capacity = Math.max(
    1,
    Math.floor(width / ((density === "Auto" ? 180 : 140) * scale)),
  );
  const columns = grid ? Math.min(capacity, density === "3" ? 3 : 6) : 1;
  const rowHeight = (grid ? 300 : 130) + 100 * (scale - 1);
  const rows = Math.ceil(games.length / columns);
  const start = Math.min(
    Math.max(0, rows - 1),
    Math.max(0, Math.floor(scroll / rowHeight) - 2),
  );
  const end = Math.min(rows, start + Math.ceil(560 / rowHeight) + 5);
  useEffect(() => {
    if (viewport.current && viewport.current.scrollTop > rows * rowHeight)
      viewport.current.scrollTop = 0;
  }, [rows, rowHeight]);
  useLayoutEffect(() => {
    if (pendingFocus.current === undefined) return;
    const node = viewport.current?.querySelector<HTMLButtonElement>(
      `[data-game-index="${pendingFocus.current}"]`,
    );
    if (node) {
      node.focus({ preventScroll: true });
      pendingFocus.current = undefined;
    }
  });
  return (
    <div
      className="virtual-games"
      ref={viewport}
      onKeyDown={(event) => {
        const button = (event.target as HTMLElement).closest<HTMLButtonElement>(
          "button[data-game-index]",
        );
        if (!button) return;
        const index = Number(button.dataset.gameIndex);
        const offset = (
          {
            ArrowLeft: -1,
            ArrowRight: 1,
            ArrowUp: -columns,
            ArrowDown: columns,
          } as Record<string, number>
        )[event.key];
        if (offset === undefined && !["Home", "End"].includes(event.key))
          return;
        let next =
          event.key === "Home"
            ? 0
            : event.key === "End"
              ? games.length - 1
              : index + offset;
        if (next < 0 || next >= games.length) {
          if (!wrap) return;
          next = (next + games.length) % games.length;
        }
        event.preventDefault();
        pendingFocus.current = next;
        const top = Math.floor(next / columns) * rowHeight,
          height = viewport.current?.clientHeight ?? 560;
        if (top < scroll) setScroll(top);
        else if (top + rowHeight > scroll + height)
          setScroll(top + rowHeight - height);
        select(games[next].id);
      }}
      onScroll={(e) => setScroll(e.currentTarget.scrollTop)}
      style={{ height: Math.min(560, rows * rowHeight), overflowY: "auto" }}
    >
      <div style={{ height: rows * rowHeight, position: "relative" }}>
        {games.slice(start * columns, end * columns).map((game, offset) => {
          const index = start * columns + offset;
          return (
            <button
              key={game.id}
              data-game-index={index}
              aria-label={game.title}
              aria-pressed={selected === game.id}
              style={{
                position: "absolute",
                top: Math.floor(index / columns) * rowHeight,
                left: `${((index % columns) * 100) / columns}%`,
                width: `calc(${100 / columns}% - 10px)`,
                height: rowHeight - 10,
              }}
              onClick={() => select(game.id)}
              onContextMenu={(event) => {
                if (openActions) {
                  event.preventDefault();
                  select(game.id);
                  openActions(
                    game,
                    event.currentTarget,
                    event.clientX,
                    event.clientY,
                  );
                }
              }}
            >
              <CoverImage
                source={game.tileArt}
                label={game.title}
                className={grid ? "grid-cover" : "list-cover"}
                x={game.tileArtFocalX}
                y={game.tileArtFocalY}
              />
              <strong>
                {game.favorite ? "★ " : ""}
                {game.title}
              </strong>
              <small>{game.module}</small>
            </button>
          );
        })}
      </div>
    </div>
  );
}

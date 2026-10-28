import { useEffect, useRef, useState } from "react";
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
}: {
  games: Game[];
  selected?: string;
  grid: boolean;
  select: (id: string) => void;
  scale?: number;
  density?: string;
}) {
  const viewport = useRef<HTMLDivElement>(null);
  const [width, setWidth] = useState(300);
  const [scroll, setScroll] = useState(0);
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
  return (
    <div
      className="virtual-games"
      ref={viewport}
      onScroll={(e) => setScroll(e.currentTarget.scrollTop)}
      style={{ height: Math.min(560, rows * rowHeight), overflowY: "auto" }}
    >
      <div style={{ height: rows * rowHeight, position: "relative" }}>
        {games.slice(start * columns, end * columns).map((game, offset) => {
          const index = start * columns + offset;
          return (
            <button
              key={game.id}
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

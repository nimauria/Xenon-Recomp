import { useEffect, useRef } from "react";
import type { Game } from "../../contracts/launcher";
import { CoverImage } from "./CoverImage";
export function GameCarousel({
  games,
  selected,
  select,
  wrap = true,
}: {
  games: Game[];
  selected?: string;
  select: (id: string) => void;
  wrap?: boolean;
}) {
  const index = Math.max(
    0,
    games.findIndex((game) => game.id === selected),
  );
  const windowed = games.slice(
    Math.max(0, index - 2),
    Math.min(games.length, index + 3),
  );
  const chosen = useRef<HTMLButtonElement>(null);
  useEffect(() => {
    chosen.current?.scrollIntoView({
      block: "nearest",
      inline: "center",
      behavior: "instant",
    });
  }, [selected]);
  const move = (direction: number) => {
    if (!games.length) return;
    const next = wrap
      ? (index + direction + games.length) % games.length
      : Math.min(games.length - 1, Math.max(0, index + direction));
    const target = games[next];
    if (target) select(target.id);
  };
  return (
    <section
      className="game-carousel"
      aria-label="Game carousel"
      onKeyDown={(event) => {
        if (event.key === "ArrowLeft" || event.key === "ArrowRight") {
          event.preventDefault();
          move(event.key === "ArrowLeft" ? -1 : 1);
        }
      }}
    >
      <div className="carousel-controls">
        <button
          aria-label="Previous game"
          disabled={games.length < 2 || (!wrap && index === 0)}
          onClick={() => move(-1)}
        >
          ← Previous
        </button>
        <span aria-live="polite">
          {index + 1} / {games.length}
        </span>
        <button
          aria-label="Next game"
          disabled={games.length < 2 || (!wrap && index === games.length - 1)}
          onClick={() => move(1)}
        >
          Next →
        </button>
      </div>
      <div className="carousel-shelf">
        {windowed.map((game) => (
          <button
            ref={game.id === selected ? chosen : undefined}
            key={game.id}
            aria-label={game.title}
            aria-pressed={game.id === selected}
            onClick={() => select(game.id)}
          >
            <CoverImage
              source={game.tileArt}
              label={game.title}
              className="carousel-cover"
              x={game.tileArtFocalX}
              y={game.tileArtFocalY}
            />
            <strong>{game.title}</strong>
            <small>{game.module}</small>
          </button>
        ))}
      </div>
      <p className="carousel-hint">
        Arrow keys browse games · Choose a game to see its actions below
      </p>
    </section>
  );
}

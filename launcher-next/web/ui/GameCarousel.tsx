import { t } from "../localization";
import { useEffect, useRef } from "react";
import type { Game } from "../../contracts/launcher";
import { CoverImage } from "./CoverImage";
export function GameCarousel({
  games,
  selected,
  select,
  wrap = true,
  openActions,
}: {
  games: Game[];
  selected?: string;
  select: (id: string) => void;
  wrap?: boolean;
  openActions?: (game: Game, source: HTMLElement, x: number, y: number) => void;
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
  const keyboardNavigation = useRef(false);
  useEffect(() => {
    chosen.current?.scrollIntoView({
      block: "nearest",
      inline: "center",
      behavior: "instant",
    });
    if (keyboardNavigation.current) {
      chosen.current?.focus({ preventScroll: true });
      keyboardNavigation.current = false;
    }
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
      aria-label={t("ui.0d9c23f7834b")}
      onKeyDown={(event) => {
        if (event.key === "ArrowLeft" || event.key === "ArrowRight") {
          event.preventDefault();
          keyboardNavigation.current = true;
          move(event.key === "ArrowLeft" ? -1 : 1);
        }
      }}
    >
      <div className="carousel-controls">
        <button
          aria-label={t("ui.0faf3673c304")}
          disabled={games.length < 2 || (!wrap && index === 0)}
          onClick={() => move(-1)}
        >
          {t("ui.435567b14289")}
        </button>
        <span aria-live="polite">
          {index + 1} / {games.length}
        </span>
        <button
          aria-label={t("ui.be88fbf0cb7b")}
          disabled={games.length < 2 || (!wrap && index === games.length - 1)}
          onClick={() => move(1)}
        >
          {t("ui.bcf0489a7b45")}
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
              className="carousel-cover"
              x={game.tileArtFocalX}
              y={game.tileArtFocalY}
            />
            <strong>{game.title}</strong>
            <small>{game.module}</small>
          </button>
        ))}
      </div>
      <p className="carousel-hint">{t("ui.943b4f352911")}</p>
    </section>
  );
}

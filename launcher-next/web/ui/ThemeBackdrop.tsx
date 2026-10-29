import { useEffect, useState } from "react";
import type { Appearance } from "../appearance";
export function ThemeBackdrop({ appearance }: { appearance: Appearance }) {
  const [loaded, setLoaded] = useState("");
  useEffect(() => {
    let active = true;
    if (!appearance.image) {
      setLoaded("");
      return;
    }
    const image = new Image();
    image.src = appearance.image;
    image
      .decode()
      .then(() => {
        if (active) setLoaded(appearance.image);
      })
      .catch(() => {
        if (active) setLoaded("");
      });
    return () => {
      active = false;
    };
  }, [appearance.image]);
  if (!appearance.enabled) return null;
  const family = appearance.palette.decorFamily;
  const decor = (name: string) =>
    `/theme-art/decor/${family}/${name}_${family}.svg`;
  const decorated = appearance.decorLevel !== "Minimal" && family !== "neutral";
  const full = appearance.decorLevel === "Full" && decorated;
  return (
    <div
      className="theme-backdrop"
      aria-hidden="true"
      style={
        { "--backdrop-strength": appearance.intensity } as React.CSSProperties
      }
    >
      {loaded && <img className="backdrop-raster" src={loaded} alt="" />}
      <div className="backdrop-shade" />
      {decorated && (
        <>
          <img className="decor-strip" src={decor("side_strip")} alt="" />
          <img className="decor-corner" src={decor("corner_top_left")} alt="" />
          <img
            className="decor-corner-bottom"
            src={decor("corner_bottom_right")}
            alt=""
          />
        </>
      )}
      {full && (
        <>
          <img className="decor-rail" src={decor("rail_vertical")} alt="" />
          <img className="decor-arc" src={decor("hud_arc_left")} alt="" />
          <img className="decor-slashes" src={decor("slashes")} alt="" />
          <img className="decor-divider" src={decor("divider_long")} alt="" />
        </>
      )}
      <div
        className="decor-overlay"
        style={{
          backgroundImage: `url(/theme-art/decor/neutral/${appearance.id === "light" ? "hex_overlay" : "diagonal_lines_overlay"}.svg)`,
        }}
      />
      <div className="decor-glow" />
    </div>
  );
}

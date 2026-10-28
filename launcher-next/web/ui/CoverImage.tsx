import { useEffect, useState } from "react";
export function CoverImage({
  source,
  label,
  className = "",
  x = 0.5,
  y = 0.5,
}: {
  source?: string;
  label: string;
  className?: string;
  x?: number;
  y?: number;
}) {
  const [loaded, setLoaded] = useState("");
  useEffect(() => {
    let active = true;
    if (!source) {
      setLoaded("");
      return;
    }
    const image = new Image();
    image.src = source;
    image
      .decode()
      .then(() => {
        if (active) setLoaded(source);
      })
      .catch(() => {});
    return () => {
      active = false;
    };
  }, [source]);
  return (
    <span className={`cover-image ${className}`} aria-hidden="true">
      <span>{label.slice(0, 2).toUpperCase()}</span>
      {loaded && (
        <img
          src={loaded}
          alt=""
          decoding="async"
          style={{
            objectPosition: `${Math.max(0, Math.min(1, x)) * 100}% ${Math.max(0, Math.min(1, y)) * 100}%`,
          }}
        />
      )}
    </span>
  );
}

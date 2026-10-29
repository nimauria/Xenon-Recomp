import { useEffect, useState } from "react";
import {
  themes,
  accents,
  backgrounds,
  resolveAppearance,
  type ThemeId,
} from "../appearance";
import { useLauncher } from "./context";
function RangeSetting({
  label,
  value,
  min,
  disabled,
  commit,
}: {
  label: string;
  value: number;
  min: number;
  disabled: boolean;
  commit: (value: number) => void;
}) {
  const [draft, setDraft] = useState(value);
  useEffect(() => setDraft(value), [value]);
  const save = (next: number) => {
    if (next !== value) commit(next);
  };
  return (
    <label>
      {label}
      <output>{Math.round(draft * 100)}%</output>
      <input
        type="range"
        aria-label={label}
        min={min}
        max="1"
        step=".02"
        value={draft}
        disabled={disabled}
        onChange={(event) => setDraft(Number(event.target.value))}
        onPointerUp={(event) => save(Number(event.currentTarget.value))}
        onKeyUp={(event) => save(Number(event.currentTarget.value))}
        onBlur={(event) => save(Number(event.currentTarget.value))}
      />
    </label>
  );
}
export function AppearanceSettings() {
  const { state, busy, action } = useLauncher();
  const [dark, setDark] = useState(
    matchMedia("(prefers-color-scheme: dark)").matches,
  );
  useEffect(() => {
    const media = matchMedia("(prefers-color-scheme: dark)");
    const update = () => setDark(media.matches);
    media.addEventListener("change", update);
    return () => media.removeEventListener("change", update);
  }, []);
  const appearance = resolveAppearance(state, dark);
  const settings = state.settings ?? {};
  const save = (key: string, value: unknown) =>
    void action("settings.set", { key, value });
  return (
    <div className="appearance-settings">
      <h3>Theme</h3>
      <div className="theme-options">
        {themes.map((theme) => {
          const palette = resolveAppearance(
            { ...state, settings: { ...settings, "ui/theme": theme.id } },
            dark,
          ).palette;
          return (
            <button
              key={theme.id}
              disabled={busy}
              aria-pressed={appearance.selected === theme.id}
              onClick={() => save("ui/theme", theme.id)}
            >
              <span
                className="theme-swatch"
                style={{
                  background: palette.window,
                  borderColor: palette.border,
                }}
              >
                <i style={{ background: palette.sidebar }} />
                <i style={{ background: palette.surface }} />
                <i style={{ background: palette.accent }} />
              </span>
              {theme.label}
            </button>
          );
        })}
      </div>
      <h3>Accent colour</h3>
      <div className="accent-options">
        {accents.map((accent) => (
          <button
            key={accent.id}
            disabled={busy}
            aria-pressed={appearance.accentId === accent.id}
            onClick={() => save("ui/accent", accent.id)}
          >
            <i
              style={{
                background:
                  accent.color ||
                  (accent.id === "custom"
                    ? String(settings["ui/customAccent"] ?? "#35D7EA")
                    : appearance.palette.accent),
              }}
            />
            {accent.label}
          </button>
        ))}
      </div>
      {appearance.accentId === "custom" && (
        <label>
          Custom accent
          <input
            type="color"
            aria-label="Custom accent colour"
            disabled={busy}
            value={String(settings["ui/customAccent"] ?? "#35D7EA")}
            onChange={(e) => save("ui/customAccent", e.target.value)}
          />
        </label>
      )}
      <label>
        Corner style
        <select
          value={appearance.corner}
          disabled={busy}
          onChange={(e) => save("ui/corners", e.target.value)}
        >
          <option value="rounded">Rounded</option>
          <option value="soft">Soft</option>
          <option value="square">Square</option>
        </select>
      </label>
      <h3>Background</h3>
      <label>
        Show theme background
        <input
          type="checkbox"
          checked={settings["appearance/themeBackdrop"] !== false}
          disabled={busy}
          onChange={(e) => save("appearance/themeBackdrop", e.target.checked)}
        />
      </label>
      <div className="background-options">
        {backgrounds[appearance.id as ThemeId].map((variant) => (
          <button
            key={variant}
            disabled={busy}
            aria-pressed={appearance.variant === variant}
            onClick={() =>
              save(`appearance/backdropVariant/${appearance.id}`, variant)
            }
          >
            {appearance.id === "light" ? (
              <span className="background-minimal">Minimal</span>
            ) : (
              <img
                loading="lazy"
                src={`/theme-art/backgrounds/theme-${appearance.id}-${variant}.png`}
                alt=""
              />
            )}
            <span>
              {variant === "hud"
                ? "HUD"
                : variant[0].toUpperCase() + variant.slice(1)}
            </span>
          </button>
        ))}
      </div>
      <RangeSetting
        label="Background intensity"
        value={appearance.intensity}
        min={0.2}
        disabled={busy}
        commit={(value) => save("appearance/backdropIntensity", value)}
      />
      <label>
        Decorations
        <select
          value={appearance.decorLevel}
          disabled={busy}
          onChange={(e) => save("appearance/decorLevel", e.target.value)}
        >
          {["Minimal", "Balanced", "Full"].map((level) => (
            <option key={level}>{level}</option>
          ))}
        </select>
      </label>
      <RangeSetting
        label="Panel opacity"
        value={Number(settings["appearance/panelOpacity"] ?? 0.94)}
        min={0}
        disabled={busy}
        commit={(value) => save("appearance/panelOpacity", value)}
      />
      <p className="appearance-note">
        Background artwork stays specific to its theme. Accent colours recolour
        controls and Xenon branding. High contrast hides decorative backgrounds.
      </p>
    </div>
  );
}

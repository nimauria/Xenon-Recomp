import catalogue from "../contracts/appearance.json";
import type { Snapshot } from "../contracts/launcher";
export const { themes, accents, backgrounds, palettes } = catalogue;
export type ThemeId = keyof typeof backgrounds;
function rgb(hex: string) {
  return [1, 3, 5].map((offset) => parseInt(hex.slice(offset, offset + 2), 16));
}
function blend(a: string, b: string, weight: number) {
  const left = rgb(a),
    right = rgb(b);
  return (
    "#" +
    left
      .map((v, i) =>
        Math.round(v * weight + right[i] * (1 - weight))
          .toString(16)
          .padStart(2, "0"),
      )
      .join("")
  );
}
function luminance(hex: string) {
  const values = rgb(hex).map((v) => {
    const c = v / 255;
    return c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
  });
  return values[0] * 0.2126 + values[1] * 0.7152 + values[2] * 0.0722;
}
export function resolveAppearance(
  state: Snapshot,
  systemDark: boolean,
  forcedContrast = false,
) {
  const settings = state.recovery?.safeMode
    ? {
        "ui/theme": "xenon-dark",
        "ui/accent": "default",
        "appearance/themeBackdrop": false,
        "appearance/decorLevel": "Minimal",
        "appearance/panelOpacity": 1,
        "accessibility/reduceMotion": true,
      }
    : (state.settings ?? {});
  const selected = String(
    settings["ui/theme"] ??
      (state.preferences.theme === "light" ? "light" : "system"),
  );
  const id = (
    selected === "system" ? (systemDark ? "xenon-dark" : "light") : selected
  ) as ThemeId;
  const palette =
    palettes.find((p) => p.id === id) ??
    palettes.find((p) => p.id === "xenon-dark")!;
  const accentId = String(settings["ui/accent"] ?? "default");
  const custom = String(settings["ui/customAccent"] ?? "#35D7EA");
  const override =
    accentId === "custom"
      ? custom
      : accents.find((a) => a.id === accentId)?.color;
  const accent =
    override && /^#[0-9a-f]{6}$/i.test(override) ? override : palette.accent;
  const highContrast =
    forcedContrast || Boolean(settings["accessibility/highContrast"]);
  const corner = String(settings["ui/corners"] ?? "rounded");
  const variant = String(
    settings[`appearance/backdropVariant/${palette.id}`] ??
      palette.defaultBackdrop,
  );
  const allowed = backgrounds[palette.id as ThemeId];
  const activeVariant = allowed.includes(variant)
    ? variant
    : palette.defaultBackdrop;
  const enabled =
    settings["appearance/themeBackdrop"] !== false && !highContrast;
  return {
    selected,
    id: palette.id as ThemeId,
    palette,
    accent,
    accentId,
    corner,
    highContrast,
    reduceMotion: Boolean(
      state.preferences.reducedMotion || settings["accessibility/reduceMotion"],
    ),
    variant: activeVariant,
    enabled,
    intensity: Math.max(
      0.2,
      Math.min(1, Number(settings["appearance/backdropIntensity"] ?? 0.72)),
    ),
    decorLevel: String(settings["appearance/decorLevel"] ?? "Balanced"),
    image:
      palette.id === "light"
        ? ""
        : `/theme-art/backgrounds/theme-${palette.id}-${activeVariant}.png`,
    variables: {
      "--bg": palette.window,
      "--header": palette.header,
      "--sidebar": palette.sidebar,
      "--panel": palette.surface,
      "--surface-alt": palette.surfaceAlt,
      "--surface-hover": palette.surfaceHover,
      "--surface-raised": palette.surfaceRaised,
      "--line": highContrast
        ? palette.dark
          ? "#E8EEF1"
          : "#1A2630"
        : palette.border,
      "--divider": palette.divider,
      "--text": palette.text,
      "--muted": highContrast ? palette.text : palette.textMuted,
      "--accent": accent,
      "--accent-strong": override
        ? blend(accent, "#000000", 1 / 1.22)
        : palette.accentStrong,
      "--accent-soft": override
        ? blend(
            accent,
            palette.dark ? "#09141B" : "#FFFFFF",
            palette.dark ? 0.24 : 0.18,
          )
        : palette.accentSoft,
      "--accent-text": override
        ? luminance(accent) > 0.42
          ? "#061014"
          : "#FFFFFF"
        : palette.accentText,
      "--success": palette.success,
      "--warning": palette.warning,
      "--danger": palette.danger,
      "--input": palette.input,
      "--surface-rgb": rgb(palette.surface).join(" "),
      "--panel-opacity": String(
        highContrast
          ? 1
          : Math.max(
              0,
              Math.min(1, Number(settings["appearance/panelOpacity"] ?? 0.94)),
            ),
      ),
      "--control-radius":
        corner === "square" ? "4px" : corner === "soft" ? "7px" : "9px",
      "--panel-radius":
        corner === "square" ? "4px" : corner === "soft" ? "8px" : "12px",
      "--dialog-radius":
        corner === "square" ? "6px" : corner === "soft" ? "10px" : "14px",
      "--focus-width": highContrast
        ? "3px"
        : settings["accessibility/enhancedFocus"]
          ? "4px"
          : "2px",
    },
  };
}
export type Appearance = ReturnType<typeof resolveAppearance>;
export function applyAppearance(appearance: Appearance) {
  const root = document.documentElement;
  root.dataset.theme = appearance.palette.dark ? "dark" : "light";
  root.dataset.themeId = appearance.id;
  root.dataset.motion = appearance.reduceMotion ? "reduced" : "normal";
  root.dataset.contrast = appearance.highContrast ? "high" : "normal";
  for (const [key, value] of Object.entries(appearance.variables))
    root.style.setProperty(key, value);
  root.style.colorScheme = appearance.palette.dark ? "dark" : "light";
}

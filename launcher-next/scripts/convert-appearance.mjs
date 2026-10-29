import { readFileSync, writeFileSync, cpSync, mkdirSync } from "node:fs";
const root = new URL("../", import.meta.url);
const source = readFileSync(
  new URL(
    "../../launcher/src/frontend_backend/settings/appearance/appearance_feature.cpp",
    import.meta.url,
  ),
  "utf8",
);
const keys = [
  "id",
  "name",
  "dark",
  "window",
  "header",
  "sidebar",
  "surface",
  "surfaceAlt",
  "surfaceHover",
  "surfaceRaised",
  "border",
  "divider",
  "text",
  "textMuted",
  "accent",
  "accentStrong",
  "accentSoft",
  "accentText",
  "success",
  "warning",
  "danger",
  "input",
  "overlay",
  "decorFamily",
  "defaultBackdrop",
];
const ids = ["carbon", "industrial", "light", "xenon-dark"];
const calls = [...source.matchAll(/return palette\(([\s\S]+?)\);/g)];
const palettes = calls.map((call, i) => {
  const values = [
    ...call[1].matchAll(
      /QStringLiteral\("([^"]*)"\)|"([^"]*)"|\b(true|false)\b/g,
    ),
  ].map((m) => (m[3] ? m[3] === "true" : (m[1] ?? m[2])));
  if (i < 3) values.unshift(ids[i]);
  if (values.length !== keys.length)
    throw new Error(`Palette ${ids[i]} has ${values.length} values`);
  return Object.fromEntries(keys.map((key, n) => [key, values[n]]));
});
if (palettes.length !== 4) throw new Error("Expected all four legacy palettes");
const backgrounds = {
  "xenon-dark": ["orbit", "tech", "hud"],
  carbon: ["nebula", "tech"],
  industrial: ["orbit", "tech"],
  light: ["minimal"],
};
const result = {
  palettes,
  themes: [
    { id: "system", label: "System" },
    { id: "xenon-dark", label: "Xenon Dark" },
    { id: "carbon", label: "Carbon" },
    { id: "industrial", label: "Industrial" },
    { id: "light", label: "Light" },
  ],
  accents: [
    { id: "default", label: "Theme default", color: "" },
    { id: "cyan", label: "Cyan", color: "#35D7EA" },
    { id: "emerald", label: "Emerald", color: "#45D995" },
    { id: "amber", label: "Amber", color: "#EFB23B" },
    { id: "violet", label: "Violet", color: "#A78BFA" },
    { id: "rose", label: "Rose", color: "#F2799A" },
    { id: "custom", label: "Custom", color: "" },
  ],
  backgrounds,
};
writeFileSync(
  new URL("contracts/appearance.json", root),
  JSON.stringify(result, null, 2) + "\n",
);
mkdirSync(new URL("public/theme-art/", root), { recursive: true });
for (const directory of ["backgrounds", "decor"])
  cpSync(
    new URL(`../../launcher/resources/${directory}`, import.meta.url),
    new URL(`public/theme-art/${directory}`, root),
    { recursive: true },
  );
cpSync(
  new URL("../../launcher/resources/branding", import.meta.url),
  new URL("public/branding/", root),
  { recursive: true },
);
console.log(
  "Converted 4 palettes, 5 theme choices, 7 accents and all legacy background/decor/branding assets",
);

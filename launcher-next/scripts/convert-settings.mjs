// One-time/repeatable conversion of the legacy declarative catalogue; no Qt runtime.
import { readFileSync, writeFileSync } from "node:fs";
const source = readFileSync(
  new URL(
    "../../launcher/src/frontend_backend/settings/schema/settings_catalog.cpp",
    import.meta.url,
  ),
  "utf8",
);
const body = source.slice(
  source.indexOf("static const QList<Definition> values{"),
  source.indexOf("\n  };"),
);
const starts = [...body.matchAll(/\{"(\w+\/\w+)",\s*"(\w+)",\s*"(\w+)",/g)];
const parse = (s) => {
  s = s.trim();
  const q = s.match(/^QStringLiteral\("([^"]*)"\)/);
  if (q) return q[1];
  if (s.startsWith("QString{}")) return "";
  if (s.startsWith("true")) return true;
  if (s.startsWith("false")) return false;
  return Number.parseFloat(s);
};
const definitions = starts.map((match, i) => {
  const text = body.slice(
    match.index + match[0].length,
    starts[i + 1]?.index ?? body.length,
  );
  const options = text.match(/stringOptions\(\{([^}]+)\}\)/);
  const explicit = [
    ...text.matchAll(
      /option\(QStringLiteral\("([^"]+)"\),\s*(QStringLiteral\("[^"]*"\)|[^)]+)\)/g,
    ),
  ];
  const range = text.match(/\},\s*(-?[\d.]+),\s*(-?[\d.]+),\s*true/);
  return {
    key: match[1],
    category: match[2],
    type: match[3],
    label: match[1]
      .split("/")[1]
      .replace(/([A-Z])/g, " $1")
      .replace(/^./, (c) => c.toUpperCase()),
    default: parse(text),
    options: options
      ? [...options[1].matchAll(/"([^"]+)"/g)].map((m) => ({
          label: m[1],
          value: m[1],
        }))
      : explicit.map((m) => ({ label: m[1], value: parse(m[2]) })),
    ...(range ? { minimum: Number(range[1]), maximum: Number(range[2]) } : {}),
  };
});
if (
  definitions.some(
    (d) => typeof d.default === "number" && !Number.isFinite(d.default),
  )
)
  throw new Error("Unparsed default");
const appearance = JSON.parse(
  readFileSync(
    new URL("../contracts/appearance.json", import.meta.url),
    "utf8",
  ),
);
const choice = (key, label, defaultValue, options) => ({
  key,
  category: "appearance",
  label,
  type: "enum",
  default: defaultValue,
  options: options.map((o) =>
    typeof o === "string"
      ? { label: o, value: o }
      : { label: o.label, value: o.id },
  ),
});
definitions.push(
  choice("ui/theme", "Theme", "system", appearance.themes),
  choice("ui/accent", "Accent colour", "default", appearance.accents),
  choice("ui/corners", "Corner style", "rounded", [
    { id: "rounded", label: "Rounded" },
    { id: "soft", label: "Soft" },
    { id: "square", label: "Square" },
  ]),
);
definitions.push({
  key: "ui/customAccent",
  category: "appearance",
  label: "Custom accent colour",
  type: "string",
  default: "#35D7EA",
  options: [],
});
definitions.push({key:"general/language",category:"general",label:"Language",type:"string",default:"auto",options:[]});
for (const palette of appearance.palettes)
  definitions.push(
    choice(
      `appearance/backdropVariant/${palette.id}`,
      "Background variant",
      palette.defaultBackdrop,
      appearance.backgrounds[palette.id],
    ),
  );
writeFileSync(
  new URL("../contracts/settings.json", import.meta.url),
  JSON.stringify(definitions, null, 2) + "\n",
);
console.log(`Converted ${definitions.length} settings`);

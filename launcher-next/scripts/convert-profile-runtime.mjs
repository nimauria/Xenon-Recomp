import { readFileSync, writeFileSync } from "node:fs";
const source = readFileSync(
  new URL(
    "../../launcher/src/frontend_backend/profiles/runtime/profile_runtime_catalog.cpp",
    import.meta.url,
  ),
  "utf8",
);
const settings = JSON.parse(
  readFileSync(new URL("../contracts/settings.json", import.meta.url), "utf8"),
);
const definitions = [
  ...source.matchAll(/\{"([^"]+\/[^"]+)", "([^"]+)", "([^"]+)"\}/g),
].map((match) => {
  const definition = settings.find((definition) => definition.key === match[1]);
  if (!definition) throw new Error(`Unknown runtime setting ${match[1]}`);
  return { ...definition, label: match[2], description: match[3] };
});
if (definitions.length !== 9)
  throw new Error("Unexpected legacy profile-runtime definition count");
const feature = readFileSync(
  new URL(
    "../../launcher/src/frontend_backend/profiles/profiles_feature.cpp",
    import.meta.url,
  ),
  "utf8",
);
const service = readFileSync(
  new URL("../../launcher/src/services/profile_service.cpp", import.meta.url),
  "utf8",
);
const choices = (name) =>
  [
    ...feature
      .match(
        new RegExp(`static const QStringList ${name}\\{([\\s\\S]*?)\\};`),
      )[1]
      .matchAll(/QStringLiteral\("([^"]*)"\)/g),
  ].map((match) => match[1]);
const limit = (name) =>
  Number(
    service.match(
      new RegExp(`constexpr (?:int|double) ${name} = ([\\d.]+);`),
    )[1],
  );
writeFileSync(
  new URL("../contracts/profiles.json", import.meta.url),
  JSON.stringify(
    {
      nameLimit: limit("kProfileNameLimit"),
      descriptionLimit: limit("kDescriptionLimit"),
      avatarZoomMin: limit("kMinAvatarZoom"),
      avatarZoomMax: limit("kMaxAvatarZoom"),
      regions: choices("regions"),
      startupPages: choices("pages"),
    },
    null,
    2,
  ) + "\n",
);
writeFileSync(
  new URL("../contracts/profile-runtime.json", import.meta.url),
  JSON.stringify(definitions, null, 2) + "\n",
);
console.log(`Converted ${definitions.length} profile runtime settings`);

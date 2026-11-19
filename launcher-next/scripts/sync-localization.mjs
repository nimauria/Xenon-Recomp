import { readFile, writeFile, mkdir } from "node:fs/promises";
import { fileURLToPath } from "node:url";
process.chdir(fileURLToPath(new URL("../", import.meta.url)));
const bytes = await readFile("contracts/localization.json");
const master = JSON.parse(bytes.toString("utf8"));
await writeFile("public/localization.json", bytes);
// Keep source locations, native-message inventory and other languages out of the
// startup JavaScript. The editable runtime file supplies them after startup.
const keys = Object.keys(master.translations.en).filter(
  (key) => !key.startsWith("native."),
);
const english = Object.fromEntries(
  keys.map((key) => [key, master.translations.en[key]]),
);
const sources = Object.fromEntries(
  keys.map((key) => [
    key,
    { original: master.sources[key].original, locations: [] },
  ]),
);
await mkdir("web/generated", { recursive: true });
await writeFile(
  "web/generated/localization.json",
  JSON.stringify({
    ...master,
    translations: Object.fromEntries(
      Object.keys(master.languages).map((id) => [
        id,
        id === "en" ? english : {},
      ]),
    ),
    sources,
  }) + "\n",
);

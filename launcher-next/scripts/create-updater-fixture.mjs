import {
  cp,
  mkdir,
  readFile,
  copyFile,
  writeFile,
  stat,
} from "node:fs/promises";
import { createReadStream } from "node:fs";
import { createHash } from "node:crypto";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
process.chdir(fileURLToPath(new URL("../", import.meta.url)));
const oldPayload = process.argv[2],
  newPackage = process.argv[3];
if (!oldPayload || !newPackage)
  throw Error("Pass an old package payload directory and the new package ZIP");
const root = resolve(`build/updater-e2e-${Date.now()}`),
  installation = resolve(root, "installation"),
  storage = resolve(root, "user-data"),
  network = resolve(root, "network");
await mkdir(network, { recursive: true });
await mkdir(storage, { recursive: true });
await cp(resolve(oldPayload), installation, {
  recursive: true,
  errorOnExist: true,
  force: false,
});
await copyFile(resolve(newPackage), resolve(network, "launcher.zip"));
const manifest = JSON.parse(
  (
    await readFile(resolve(oldPayload, "launcher-package.json"), "utf8")
  ).replace(/^\uFEFF/, ""),
);
const version = process.env.UPDATE_FIXTURE_VERSION || "0.1.1";
const hash = createHash("sha256");
for await (const chunk of createReadStream(newPackage)) hash.update(chunk);
const digest = hash.digest("hex"),
  size = (await stat(newPackage)).size;
const api =
    "https://api.github.com/repos/nimauria/Xenon-Recomp/releases?per_page=30",
  url = `https://github.com/nimauria/Xenon-Recomp/releases/download/v${version}/xenon-launcher-next-windows-x64.zip`;
await writeFile(
  resolve(network, "releases.json"),
  JSON.stringify([
    {
      tag_name: `v${version}`,
      draft: false,
      prerelease: false,
      body: "Explicit local updater fixture; no release was published.",
      assets: [
        {
          name: "xenon-launcher-next-windows-x64.zip",
          browser_download_url: url,
          size,
          digest: `sha256:${digest}`,
        },
      ],
    },
  ]),
);
await writeFile(
  resolve(network, "catalog.json"),
  JSON.stringify({ schema: "xenon.module-catalog", version: 1, modules: [] }),
);
await writeFile(
  resolve(network, "responses.json"),
  JSON.stringify({
    [api]: { file: "releases.json" },
    [url]: { file: "launcher.zip" },
    "https://raw.githubusercontent.com/nimauria/Xenon-Modules/main/catalog.json":
      { file: "catalog.json" },
  }),
);
const fixture = {
  root,
  installation,
  storage,
  network,
  oldVersion: manifest.version,
  newVersion: version,
};
await writeFile(
  resolve("build/updater-fixture.json"),
  JSON.stringify(fixture, null, 2),
);
console.log(JSON.stringify(fixture, null, 2));

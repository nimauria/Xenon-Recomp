import { mkdir, writeFile, readFile, copyFile } from "node:fs/promises";
import { resolve } from "node:path";
import { createHash } from "node:crypto";
import { fileURLToPath } from "node:url";
import { writeFixtureZip } from "./zip-fixture.mjs";
export const fixtureModule = "test.catalogue.module";
export const registry =
  "https://raw.githubusercontent.com/nimauria/Xenon-Modules/main/";
export const releaseApi =
  "https://api.github.com/repos/fixture/catalogue-module/releases?per_page=30";
export async function createNetworkFixtures(folder) {
  await mkdir(folder, { recursive: true });
  const entry = {
    schema: "xenon.module-entry",
    version: 1,
    id: fixtureModule,
    type: "game",
    name: "Fixture catalogue module",
    description:
      "Explicit network-fixture module for launcher migration checks.",
    publisher: { name: "Fixture publisher", verified: false },
    repository: {
      provider: "github",
      owner: "fixture",
      name: "catalogue-module",
    },
    releaseAssets: {
      "windows-x64": "fixture-windows-x64.xenonmod.zip",
      "linux-x64": "fixture-linux-x64.xenonmod.zip",
      "macos-arm64": "fixture-macos-arm64.xenonmod.zip",
    },
    launcher: { tileArt: "assets/test.catalogue.module/cover.png" },
    dlc: [],
  };
  await writeFile(
    resolve(folder, "catalog.json"),
    JSON.stringify({
      schema: "xenon.module-catalog",
      version: 1,
      modules: [`modules/${fixtureModule}.json`],
    }),
  );
  await writeFile(resolve(folder, "entry.json"), JSON.stringify(entry));
  await writeFile(resolve(folder, "launcher-releases.json"), "[]");
  await copyFile(
    new URL(
      "../public/theme-art/backgrounds/theme-carbon-nebula.png",
      import.meta.url,
    ),
    resolve(folder, "cover.png"),
  );
  const mapping = {
    [registry + "catalog.json"]: { file: "catalog.json" },
    [registry + `modules/${fixtureModule}.json`]: { file: "entry.json" },
    [registry + "assets/test.catalogue.module/cover.png"]: {
      file: "cover.png",
      etag: "fixture-cover-v1",
    },
    [releaseApi]: { file: "releases.json" },
    "https://api.github.com/repos/nimauria/Xenon-Recomp/releases?per_page=30": {
      file: "launcher-releases.json",
    },
  };
  await writeFile(resolve(folder, "responses.json"), JSON.stringify(mapping));
  await publishFixture(folder, "1.0.0");
}
export async function publishFixture(folder, version, validDigest = true) {
  const mapping = JSON.parse(
    await readFile(resolve(folder, "responses.json"), "utf8"),
  );
  const asset = "fixture-windows-x64.xenonmod.zip";
  await writeFixtureZip(resolve(folder, "module.zip"), {
    "module/module.json": JSON.stringify({
      id: fixtureModule,
      name: "Fixture catalogue module",
      version,
      settings: [{ id: "example", type: "bool", defaultValue: true }],
    }),
    "module/data.txt": "test-only fixture",
  });
  const bytes = await readFile(resolve(folder, "module.zip"));
  const url = `https://github.com/fixture/catalogue-module/releases/download/v${version}/${asset}`;
  mapping[url] = { file: "module.zip" };
  await writeFile(
    resolve(folder, "releases.json"),
    JSON.stringify([
      {
        tag_name: `v${version}`,
        name: `Fixture ${version}`,
        body: "Fixture release notes.",
        draft: false,
        prerelease: false,
        assets: [
          {
            name: asset,
            browser_download_url: url,
            size: bytes.length,
            digest:
              "sha256:" +
              (validDigest
                ? createHash("sha256").update(bytes).digest("hex")
                : "0".repeat(64)),
          },
        ],
      },
    ]),
  );
  await writeFile(resolve(folder, "responses.json"), JSON.stringify(mapping));
}
if (
  process.argv[1] &&
  resolve(process.argv[1]) === fileURLToPath(import.meta.url)
) {
  const folder = resolve(
    process.argv[2] ?? "launcher-next/build/network-fixtures",
  );
  await createNetworkFixtures(folder);
  console.log(folder);
}

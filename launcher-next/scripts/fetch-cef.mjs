import { createHash } from "node:crypto";
import { createReadStream, createWriteStream } from "node:fs";
import { mkdir, readFile, stat, writeFile } from "node:fs/promises";
import { Readable } from "node:stream";
import { pipeline } from "node:stream/promises";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { resolve } from "node:path";
const root = fileURLToPath(new URL("../", import.meta.url));
const platforms = {
  win32: process.arch === "arm64" ? "windowsarm64" : "windows64",
  linux: process.arch === "arm64" ? "linuxarm64" : "linux64",
  darwin: process.arch === "arm64" ? "macosarm64" : "macosx64",
};
const platform = process.argv[2] ?? platforms[process.platform];
const manifest = JSON.parse(
  await readFile(resolve(root, "cef-distributions.json"), "utf8"),
);
const artifact = manifest.platforms[platform];
if (!artifact) throw new Error(`No pinned CEF archive for ${platform}`);
const tools = resolve(root, ".tools");
await mkdir(tools, { recursive: true });
const archive = resolve(tools, artifact.name);
if (!(await stat(archive).catch(() => null))) {
  console.log(`Downloading ${artifact.name}`);
  const response = await fetch(
    `https://cef-builds.spotifycdn.com/${encodeURIComponent(artifact.name)}`,
  );
  if (!response.ok || !response.body)
    throw new Error(`CEF download failed: ${response.status}`);
  await pipeline(Readable.fromWeb(response.body), createWriteStream(archive));
}
const hash = createHash("sha1");
for await (const chunk of createReadStream(archive)) hash.update(chunk);
if (
  hash.digest("hex") !== artifact.sha1 ||
  (await stat(archive)).size !== artifact.size
)
  throw new Error("CEF archive failed vendor checksum/size validation");
const directory = resolve(tools, artifact.name.replace(/\.tar\.bz2$/, ""));
if (!(await stat(resolve(directory, "cmake/FindCEF.cmake")).catch(() => null)))
  execFileSync("tar", ["-xf", archive, "-C", tools], { stdio: "inherit" });
await writeFile(resolve(tools, `cef-root-${platform}.txt`), directory + "\n");
if(platform===platforms[process.platform])await writeFile(resolve(tools, "cef-root.txt"), directory + "\n");
console.log(`Verified CEF ${manifest.version}: ${directory}`);

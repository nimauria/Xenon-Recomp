// Explicit, persistent developer samples. Never seeds normal launcher data.
import { mkdir, readFile, writeFile, copyFile, stat } from "node:fs/promises";
import { spawn } from "node:child_process";
import { createHash, randomUUID } from "node:crypto";
import { resolve, join, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { writeFixtureZip } from "./zip-fixture.mjs";
const root = fileURLToPath(new URL("../", import.meta.url));
const registry =
  "https://raw.githubusercontent.com/nimauria/Xenon-Modules/main/";
const platform = `${process.platform === "win32" ? "windows" : process.platform === "darwin" ? "macos" : "linux"}-${process.arch === "arm64" ? "arm64" : "x64"}`;
const examples = [
  ["orbital", "Orbital Frontier", "theme-carbon-nebula.png"],
  ["rally", "Neon Rally", "theme-industrial-tech.png"],
  ["horizon", "Iron Horizon", "theme-xenon-dark-orbit.png"],
  ["skyline", "Skyline Squadron", "theme-xenon-dark-hud.png"],
];
const schema = [
  {
    id: "difficulty",
    label: "Difficulty",
    description: "Example module option, passed to the test host only.",
    type: "choice",
    default: "Normal",
    options: ["Easy", "Normal", "Hard"],
  },
  {
    id: "fov",
    label: "Field of view",
    description: "Example per-game graphics option.",
    type: "number",
    default: 90,
    minimum: 60,
    maximum: 120,
  },
  {
    id: "vibration",
    label: "Vibration",
    description: "The fixture drives no physical device.",
    type: "bool",
    default: true,
  },
];
const json = (file, value) =>
  writeFile(file, JSON.stringify(value, null, 2) + "\n");
export async function createDemo(folder) {
  folder = resolve(folder);
  if (!folder.startsWith(resolve(root, "build") + sep))
    throw Error("Demo data must be isolated below launcher-next/build");
  const data = join(folder, "data"),
    network = join(folder, "network"),
    marker = join(folder, "demo-fixture.json");
  if (await stat(join(data, "state.json")).catch(() => null)) {
    const existing = JSON.parse(await readFile(marker, "utf8"));
    if (existing.schema !== "xenon.launcher-demo")
      throw Error("Refusing to modify non-demo data");
    return { folder, data, network };
  }
  for (const path of [
    data,
    network,
    join(data, "catalog-art"),
    join(data, "content"),
    join(data, "avatars"),
    join(data, "captures"),
  ])
    await mkdir(path, { recursive: true });
  const entries = [],
    modules = [],
    games = [],
    moduleUpdates = {},
    jobs = [],
    mapping = {};
  const now = Date.now();
  for (const [i, [key, title, art]] of examples.entries()) {
    const id = "demo." + key,
      moduleRoot = join(data, "demo-modules", key),
      source = join(data, "content", key + ".xex");
    await mkdir(moduleRoot, { recursive: true });
    const image = join(root, "public/theme-art/backgrounds", art);
    await copyFile(image, join(moduleRoot, "art.png"));
    await copyFile(image, join(data, "catalog-art", id + ".png"));
    await copyFile(image, join(data, "captures", title + " - demo.png"));
    await writeFile(
      source,
      "Developer demo fixture. Not an Xbox 360 executable.\n",
    );
    const manifest = {
      id,
      name: title + " module",
      version: "1.0.0",
      settings: schema,
      launcher: {
        gameTitle: title,
        description:
          "Fictional developer example; no commercial game files are included.",
        tileArt: "art.png",
        heroArt: "art.png",
      },
    };
    await json(join(moduleRoot, "module.json"), manifest);
    if (i < 3)
      modules.push({
        id,
        name: manifest.name,
        version: "1.0.0",
        path: moduleRoot,
        enabled: true,
        schema,
        settings: { difficulty: "Normal", fov: 90, vibration: true },
        manifest,
        managed: false,
      });
    const gameId = "demo-game-" + key;
    games.push({
      id: gameId,
      title,
      module: i < 3 ? manifest.name : "Demo module not installed",
      moduleId: id,
      description: manifest.launcher.description,
      contentPath: source,
      favorite: i < 2,
      lastPlayed: String(now - (i + 1) * 3600000),
      addedAt: String(now - (4 - i) * 86400000),
      verified: false,
      mediaModelVersion: 1,
      activityModelVersion: 1,
      playCount: 3 + i * 2,
      totalPlayTimeMs: (45 + i * 25) * 60000,
      tileArt: i < 3 ? "xenon://app/game-tile/" + gameId : undefined,
      heroArt: i < 3 ? "xenon://app/game-hero/" + gameId : undefined,
      dlc: [],
      fixtureActivity: { playCount: 0, totalPlayTimeMs: 0 },
      media: [
        {
          id: "demo-media-" + key,
          name: "Demo source",
          sourcePath: source,
          sourceType: ".xex",
          mediaId: "",
          discNumber: 1,
          discCount: 1,
          region: "Auto (Global)",
          importMode: "kept",
          isPrimary: true,
          identityVerified: false,
        },
      ],
      runtimeOverrides: i === 0 ? { "audio/masterVolume": 0.8 } : {},
    });
    const assetName = `${id}-${platform}.xenonmod.zip`,
      version = i === 0 ? "2.0.0" : i === 3 ? "1.1.0" : "1.0.0";
    const entry = {
      schema: "xenon.module-entry",
      version: 1,
      id,
      type: "game",
      name: manifest.name,
      description: "Developer demo package for " + title + ".",
      publisher: { name: "Xenon developer demo", verified: false },
      repository: { provider: "github", owner: "fixture", name: key },
      releaseAssets: { [platform]: assetName },
      launcher: { gameTitle: title, tileArt: `assets/${id}/cover.png` },
      dlc: [],
    };
    await json(join(network, id + ".json"), entry);
    entries.push({
      id,
      name: entry.name,
      description: entry.description,
      repository: "fixture/" + key,
      repositoryUrl: "https://github.com/fixture/" + key,
      publisher: entry.publisher.name,
      verified: false,
      hostKey: platform,
      assetName,
      launcher: {
        gameTitle: title,
        tileArt: entry.launcher.tileArt,
        _tileFile: id + ".png",
      },
      dlc: [],
    });
    const zip = join(network, id + ".zip");
    const payload = {
      "xenon-module.json": JSON.stringify({ ...manifest, version }),
      "art.png": await readFile(image),
      "demo.txt": "Fictional metadata/artwork sample; no executable module.",
    };
    await writeFixtureZip(zip, payload);
    const bytes = await readFile(zip);
    const archiveDigest = createHash("sha256").update(bytes).digest("hex");
    const files = [];
    for (const [path, content] of Object.entries(payload).sort(([a], [b]) =>
      a < b ? -1 : 1,
    )) {
      const data = Buffer.from(content);
      const file = id + "-source-" + path;
      await writeFile(join(network, file), data);
      files.push({
        path,
        size: data.length,
        sha256: createHash("sha256").update(data).digest("hex"),
      });
      mapping[registry + "games/fixture/" + key + "/" + path] = { file };
    }
    const digest = createHash("sha256")
      .update(
        files
          .map((f) => f.path + "\n" + f.size + "\n" + f.sha256 + "\n")
          .join(""),
      )
      .digest("hex");
    const release = {
      version,
      source: "repository",
      name: title + " demo " + version,
      notes: "Developer sample module files. Metadata and artwork only.",
      url:
        "https://github.com/nimauria/Xenon-Modules/tree/main/games/fixture/" +
        key,
      prerelease: false,
      _files: files,
      asset: {
        name: "repository-files",
        size: files.reduce((s, f) => s + f.size, 0),
        sha256: digest,
      },
    };
    const packageFile = id + "-package.json";
    await json(join(network, packageFile), {
      schema: "xenon.repository-package",
      version: 1,
      moduleId: id,
      moduleVersion: version,
      files,
    });
    mapping[registry + "games/fixture/" + key + "/package.json"] = {
      file: packageFile,
    };
    mapping[registry + "modules/" + id + ".json"] = { file: id + ".json" };
    mapping[registry + entry.launcher.tileArt] = { file: id + "-art.png" };
    await copyFile(image, join(network, id + "-art.png"));
    if (i === 1 || i === 2)
      modules.find((module) => module.id === id).repositoryDigest = digest;
    moduleUpdates[id] = {
      state:
        i === 3
          ? "ready-to-install"
          : i === 0
            ? "update-available"
            : "up-to-date",
      release,
      lastChecked: String(now),
      lastError: "",
    };
    if (i === 3) {
      const jobId = randomUUID(),
        download = join(data, "downloads", jobId);
      await mkdir(download, { recursive: true });
      await copyFile(zip, join(download, "package.zip"));
      await json(join(download, "owner.json"), {
        owner: "xenon-launcher-next-download",
        jobId,
      });
      Object.assign(moduleUpdates[id], {
        _packagePath: join(download, "package.zip"),
        _downloadId: jobId,
        downloadVersion: version,
        downloadDigest: digest,
        _archiveDigest: archiveDigest,
      });
      jobs.push({
        id: jobId,
        kind: "module.download",
        moduleId: id,
        title: "Download " + entry.name + " " + version,
        state: "completed",
        phase: "Completed",
        progress: 100,
        createdAt: String(now - 90000),
        endedAt: String(now - 60000),
        bytesDone: bytes.length,
        bytesTotal: bytes.length,
      });
    }
  }
  await json(join(network, "catalog.json"), {
    schema: "xenon.module-catalog",
    version: 1,
    modules: entries.map((e) => "modules/" + e.id + ".json"),
  });
  mapping[registry + "catalog.json"] = { file: "catalog.json" };
  await json(join(network, "launcher-releases.json"), []);
  mapping[
    "https://api.github.com/repos/nimauria/Xenon-Recomp/releases?per_page=30"
  ] = { file: "launcher-releases.json" };
  await json(join(network, "responses.json"), mapping);
  await copyFile(
    join(root, "public/branding/xenon-app-icon-256.png"),
    join(data, "avatars/demo-pilot.png"),
  );
  const profiles = [
    {
      id: "demo-pilot",
      name: "Demo Pilot",
      description: "Example desktop profile",
      region: "Auto (Global)",
      offline: true,
      startupPage: "Library",
      isolatedSettings: false,
      runtimeOverrides: {},
      avatarKey: "demo-pilot.png",
    },
    {
      id: "demo-couch",
      name: "Demo Couch",
      description: "Example living-room profile",
      region: "Auto (Global)",
      offline: true,
      startupPage: "Home",
      isolatedSettings: true,
      runtimeOverrides: {
        "audio/masterVolume": 0.6,
        "input/preferredDevice": "Controller",
      },
    },
  ];
  jobs.push({
    id: randomUUID(),
    kind: "module.check",
    moduleId: "demo.orbital",
    title: "Earlier demo update check",
    state: "failed",
    phase: "Failed",
    progress: 0,
    error: "Example interrupted check. Retry uses the local demo service.",
    createdAt: String(now - 120000),
    endedAt: String(now - 110000),
  });
  await json(join(data, "state.json"), {
    schemaVersion: 1,
    revision: 0,
    mode: "native",
    developmentDemo: true,
    profiles,
    activeProfileId: profiles[0].id,
    games,
    modules,
    preferences: { theme: "dark", reducedMotion: false, startPage: "Library" },
    settings: {
      "ui/theme": "xenon-dark",
      "library/viewMode": "Grid",
      "updates/automaticChecks": false,
      "developer/modeEnabled": true,
      "general/language": "en",
    },
    catalog: {
      state: "ready",
      entries,
      lastError: "",
      lastChecked: String(now),
    },
    moduleUpdates,
    launcherUpdate: { state: "idle" },
    updateHistory: [],
    jobs,
    notifications: [
      {
        id: randomUUID(),
        title: "Developer demo",
        message:
          "All games, profiles, history and packages in this window are fictional samples.",
        programTitle: false,
        severity: "info",
        time: String(now),
        read: false,
        action: { type: "navigate", page: "Home" },
      },
      {
        id: randomUUID(),
        title: "Sample package ready",
        message:
          "Try installing Skyline Squadron from Downloads or the module catalogue.",
        programTitle: false,
        severity: "success",
        time: String(now - 1000),
        read: false,
        action: { type: "navigate", page: "Downloads" },
      },
    ],
    history: games.slice(0, 3).map((g, i) => ({
      id: randomUUID(),
      gameId: g.id,
      title: g.title,
      time: String(now - (i + 1) * 3600000),
      state: "stopped",
      fixture: true,
      durationMs: 1200000,
      reachedRunning: true,
    })),
  });
  await json(marker, {
    schema: "xenon.launcher-demo",
    version: 1,
    createdAt: new Date().toISOString(),
    description:
      "Explicit fictional developer data. Not a production game library.",
  });
  return { folder, data, network };
}
if (
  process.argv[1] &&
  resolve(process.argv[1]) === fileURLToPath(import.meta.url)
) {
  process.chdir(root);
  const option = (name) =>
    process.argv.find((v) => v.startsWith(name + "="))?.slice(name.length + 1);
  const demo = await createDemo(
    resolve(
      option("--data-root") ??
        "build/demo/" +
          (process.argv.includes("--fresh") ? "run-" + Date.now() : "default"),
    ),
  );
  console.log("Developer demo data: " + demo.data);
  if (!process.argv.includes("--prepare-only")) {
    const build = resolve(process.env.XENON_TEST_BUILD || "build/native"),
      desktop = resolve(
        process.env.XENON_TEST_DESKTOP || join(build, "desktop"),
      ),
      executable = join(
        desktop,
        process.platform === "darwin"
          ? "XenonLauncher.app/Contents/MacOS/XenonLauncher"
          : "xenon_launcher_next" +
              (process.platform === "win32" ? ".exe" : ""),
      ),
      host = join(
        build,
        "xenon_launcher_fixture_host" +
          (process.platform === "win32" ? ".exe" : ""),
      );
    if (
      !(await stat(executable).catch(() => null)) ||
      !(await stat(host).catch(() => null))
    )
      throw Error("Build the desktop first: npm run desktop:build");
    const args = [
        "--data-dir=" + demo.data,
        "--fixture-network=" + demo.network,
        "--fixture-host=" + host,
      ],
      port = option("--debug-port");
    if (port) {
      if (!/^\d+$/.test(port) || +port < 1024 || +port > 65535)
        throw Error("Invalid debug port");
      args.push("--remote-debugging-port=" + port);
    }
    const app = spawn(executable, args, {
      // This command intentionally opens the reviewable developer demo.
      windowsHide: false,
      stdio: "inherit",
    });
    app.once("error", (e) => {
      throw e;
    });
    app.once("exit", (code) => {
      process.exitCode = code ?? 1;
    });
  }
}

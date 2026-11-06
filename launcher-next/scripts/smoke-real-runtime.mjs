// Uses a real existing Xenon host and isolated invalid content. A failure is the
// expected result; this must never be described as a successfully running game.
import { spawn } from "node:child_process";
import { createInterface } from "node:readline";
import { mkdir, writeFile, readFile, stat } from "node:fs/promises";
import { createHash } from "node:crypto";
import { resolve, join } from "node:path";
import { fileURLToPath } from "node:url";
import assert from "node:assert/strict";
process.chdir(fileURLToPath(new URL("../", import.meta.url)));
const host = resolve(process.argv[2] || process.env.XENON_RUNTIME_HOST || "");
assert.ok(
  process.argv[2] || process.env.XENON_RUNTIME_HOST,
  "Supply the real xenon_runtime_host executable",
);
assert.ok((await stat(host)).isFile());
const output = resolve("build/real-runtime-" + Date.now()),
  data = join(output, "data");
await mkdir(data, { recursive: true });
const source = join(output, "deliberately-invalid.xex");
await writeFile(
  source,
  "Invalid XEX for the real runtime integration failure test.",
);
const service = resolve(
  process.env.XENON_TEST_BUILD || "build/native",
  "xenon_launcher_service" + (process.platform === "win32" ? ".exe" : ""),
);
let app,
  lines,
  exited,
  session,
  sequence = 0;
async function bounded(promise, ms = 30000) {
  let timer;
  try {
    return await Promise.race([
      promise,
      new Promise((_resolve, reject) => {
        timer = setTimeout(() => reject(Error("Runtime smoke timeout")), ms);
      }),
    ]);
  } finally {
    clearTimeout(timer);
  }
}
function open() {
  app = spawn(service, [data], {
    windowsHide: true,
    stdio: ["pipe", "pipe", "pipe"],
  });
  exited = false;
  app.once("exit", () => {
    exited = true;
  });
  app.stderr.on("data", (bytes) => process.stderr.write(bytes));
  lines = createInterface({ input: app.stdout })[Symbol.asyncIterator]();
}
async function rpc(command, payload = {}) {
  const id = "real-" + ++sequence;
  app.stdin.write(JSON.stringify({ id, version: 1, command, payload }) + "\n");
  const { value, done } = await bounded(lines.next());
  assert.equal(done, false, "Service must reply");
  const reply = JSON.parse(value);
  assert.equal(reply.id, id);
  assert.equal(reply.ok, true, reply.error?.message);
  return reply.data;
}
const action = (action, data = {}) => rpc("launcher.action", { action, data });
async function close() {
  if (!app) return;
  app.stdin.end();
  for (let i = 0; i < 100 && !exited; ++i)
    await new Promise((r) => setTimeout(r, 20));
  if (!exited) app.kill();
  app = undefined;
}
try {
  open();
  assert.equal((await rpc("application.snapshot")).capabilities.runtime, false);
  let state = await action("runtime.configure", { path: host });
  assert.equal(state.runtimeConnection.source, "saved");
  assert.equal(state.capabilities.runtime, true);
  state = await action("library.import", { paths: [source], mode: "keep" });
  const game = state.games[0];
  state = await action("session.launch", { gameId: game.id });
  session = state.session;
  assert.equal(session.fixture, false);
  assert.equal(session.state, "starting");
  for (let i = 0; i < 300; ++i) {
    state = await rpc("application.snapshot");
    session = state.session;
    assert.notEqual(
      session.state,
      "running",
      "Invalid content must never report Running",
    );
    if (session.state === "failed") break;
    await new Promise((r) => setTimeout(r, 100));
  }
  assert.equal(session.state, "failed");
  assert.equal(session.fixture, false);
  assert.ok(session.lastError);
  assert.ok(session.errorCategory, "Propagate the real host error category");
  for (
    let i = 0;
    i < 40 && !session.logTail?.includes("[runtime_host] Starting session");
    ++i
  ) {
    await new Promise((resolve) => setTimeout(resolve, 50));
    state = await rpc("application.snapshot");
    session = state.session;
  }
  assert.ok(
    session.logTail.includes("[runtime_host] Starting session"),
    "Actual Xenon process must have consumed the launcher configuration",
  );
  assert.equal(state.games[0].playCount, 0);
  const config = JSON.parse(
    await readFile(join(session.sessionDir, "launch-config.json"), "utf8"),
  );
  assert.equal(config.fixture, false);
  assert.equal(config.headlessMode, false);
  assert.equal(config.configVersion, 1);
  assert.equal(config.contentPath, source);
  assert.equal(config.inputModuleApiVersion, 1);
  await close();
  open();
  state = await rpc("application.snapshot");
  assert.equal(state.capabilities.runtime, true);
  assert.equal(state.runtimeConnection.configuredPath, host);
  assert.equal(state.session.state, "failed");
  state = await action("session.dismiss");
  assert.equal(state.session.fixture, false);
  assert.equal(state.session.state, "idle");
  await writeFile(
    join(output, "result.json"),
    JSON.stringify(
      {
        host,
        hostSha256: createHash("sha256")
          .update(await readFile(host))
          .digest("hex"),
        result:
          "Real host consumed normal-Play configuration and reported an expected invalid-content failure. This is not game compatibility validation.",
        session,
        config,
      },
      null,
      2,
    ) + "\n",
  );
  console.log(
    "Real Xenon host selection, normal-Play config, authoritative failure/logs, saved selection/restart and non-fixture classification passed. Evidence: " +
      output,
  );
} finally {
  if (session?.sessionDir && !["failed", "stopped"].includes(session.state))
    await writeFile(join(session.sessionDir, "stop.signal"), "stop").catch(
      () => {},
    );
  await close();
}

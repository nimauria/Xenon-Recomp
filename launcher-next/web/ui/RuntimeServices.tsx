import { useEffect, useState } from "react";
import type { Page } from "../../contracts/launcher";
import type { ServiceOperation } from "../../contracts/services";
import { isActiveJob } from "../jobs";
import { useLauncher } from "./context";

export function InputSettings() {
  const { state, action, busy } = useLauncher();
  const provider = state.runtimeServices;
  const input = provider?.input,
    blocked =
      busy ||
      !provider?.capabilities.input ||
      state.jobs?.some(
        (job) => job.kind === "service.operation" && isActiveJob(job),
      );
  const [devices, setDevices] = useState<Record<number, string>>({});
  const send = (operation: ServiceOperation) =>
    action(operation.command, operation.data);
  return (
    <section aria-label="Input routing">
      <h3>Devices and player routing</h3>
      <p>{input?.status ?? "Runtime input provider unavailable"}</p>
      {provider?.fixture && (
        <p className="status">
          Explicit fixture provider. These controls do not drive physical
          devices.
        </p>
      )}
      <div className="actions">
        <button
          disabled={blocked}
          onClick={() => void send({ command: "input.refresh", data: {} })}
        >
          Refresh input devices
        </button>
        <button
          disabled={blocked}
          onClick={() => void send({ command: "input.reconfigure", data: {} })}
        >
          Reconfigure input
        </button>
      </div>
      <ul>
        {input?.devices.map((device) => (
          <li key={device.id}>
            {device.name} · {device.kind} ·{" "}
            {device.connected ? "Connected" : "Disconnected"} ·{" "}
            {device.provider}
          </li>
        ))}
      </ul>
      {input?.users.map((user) => (
        <fieldset key={user.index} disabled={blocked} className="input-player">
          <legend>Player {user.index + 1}</legend>
          <p>
            {user.automatic
              ? "Automatic device selection"
              : user.sources.length
                ? "Assigned sources"
                : "No sources assigned"}
          </p>
          <label>
            Device
            <select
              aria-label={`Player ${user.index + 1} device`}
              value={devices[user.index] ?? input.devices[0]?.id ?? ""}
              onChange={(event) =>
                setDevices((previous) => ({
                  ...previous,
                  [user.index]: event.target.value,
                }))
              }
            >
              {input.devices.map((device) => (
                <option value={device.id} key={device.id}>
                  {device.name}
                </option>
              ))}
            </select>
          </label>
          <div className="actions">
            <button
              onClick={() =>
                void send({
                  command: "input.assign",
                  data: {
                    user: user.index,
                    deviceId: devices[user.index] ?? input.devices[0]?.id ?? "",
                  },
                })
              }
            >
              Assign device
            </button>
            <button
              onClick={() =>
                void send({
                  command: "input.sourceAdd",
                  data: {
                    user: user.index,
                    deviceId: devices[user.index] ?? input.devices[0]?.id ?? "",
                  },
                })
              }
            >
              Add source
            </button>
            <button
              onClick={() =>
                void send({
                  command: "input.automatic",
                  data: { user: user.index },
                })
              }
            >
              Use automatic
            </button>
            <button
              onClick={() =>
                void send({
                  command: "input.clear",
                  data: { user: user.index },
                })
              }
            >
              Clear sources
            </button>
            <button
              onClick={() =>
                void send({
                  command: "input.vibrate",
                  data: { user: user.index },
                })
              }
            >
              {provider?.fixture
                ? "Record fixture vibration"
                : "Test vibration"}
            </button>
          </div>
          <ul>
            {user.sources.map((id) => (
              <li key={id}>
                {input.devices.find((device) => device.id === id)?.name ?? id}{" "}
                <button
                  aria-label={`Remove ${id} from player ${user.index + 1}`}
                  onClick={() =>
                    void send({
                      command: "input.sourceRemove",
                      data: { user: user.index, deviceId: id },
                    })
                  }
                >
                  Remove source
                </button>
              </li>
            ))}
          </ul>
          <label>
            Bindings profile
            <select
              aria-label={`Player ${user.index + 1} bindings profile`}
              value={user.profileId}
              onChange={(event) =>
                void send(
                  event.target.value
                    ? {
                        command: "input.profileBind",
                        data: {
                          user: user.index,
                          profileId: event.target.value,
                        },
                      }
                    : {
                        command: "input.profileClear",
                        data: { user: user.index },
                      },
                )
              }
            >
              <option value="">Launcher default</option>
              {input.profiles.map((profile) => (
                <option key={profile.id} value={profile.id}>
                  {profile.name}
                </option>
              ))}
            </select>
          </label>
        </fieldset>
      ))}
      {input?.profileStorePath && (
        <p className="path">Bindings store: {input.profileStorePath}</p>
      )}
      {input?.moduleApi && (
        <details>
          <summary>Input module API</summary>
          <pre>{JSON.stringify(input.moduleApi, null, 2)}</pre>
        </details>
      )}
    </section>
  );
}
export function RuntimeServices({ page }: { page: Page }) {
  const { state, action, busy, pick } = useLauncher();
  const provider = state.runtimeServices;
  const blocked = Boolean(
    busy ||
    state.jobs?.some(
      (job) => job.kind === "service.operation" && isActiveJob(job),
    ),
  );
  const [mountPoint, mount] = useState("game:"),
    [kind, setKind] = useState<"host" | "gdfx" | "stfs">("host"),
    [source, setSource] = useState(""),
    [readOnly, setReadOnly] = useState(true);
  const [alias, setAlias] = useState("dvd:"),
    [target, setTarget] = useState("game:/"),
    [path, setPath] = useState("game:/");
  const [online, setOnline] = useState(navigator.onLine);
  useEffect(() => {
    const update = () => setOnline(navigator.onLine);
    window.addEventListener("online", update);
    window.addEventListener("offline", update);
    return () => {
      window.removeEventListener("online", update);
      window.removeEventListener("offline", update);
    };
  }, []);
  const send = (operation: ServiceOperation) =>
    action(operation.command, operation.data);
  if (page === "Network")
    return (
      <section className="card">
        <h2>Network service</h2>
        <p>
          Host network link: {online ? "Connected" : "Disconnected"}. A host
          connection does not prove Internet access or Xenon service readiness.
        </p>
        {provider?.fixture && (
          <p className="status">
            Explicit network fixture. No remote service is contacted.
          </p>
        )}
        <button
          disabled={blocked || !provider?.capabilities.network}
          onClick={() => void send({ command: "network.refresh", data: {} })}
        >
          Refresh network status
        </button>
        <dl>
          {Object.entries(provider?.network ?? {}).map(([key, value]) => (
            <div key={key}>
              <dt>{key.replace(/([A-Z])/g, " $1")}</dt>
              <dd>
                {Array.isArray(value)
                  ? value.join(", ") || "None"
                  : typeof value === "boolean"
                    ? value
                      ? "Yes"
                      : "No"
                    : String(value || "—")}
              </dd>
            </div>
          ))}
        </dl>
        {!provider?.capabilities.network && (
          <p>
            The Xenon Network adapter is not connected. Online module downloads
            are a separate desktop service.
          </p>
        )}
        {provider?.fixture && (
          <label>
            Network fixture scenario
            <select
              aria-label="Network fixture scenario"
              disabled={blocked}
              defaultValue="offline"
              onChange={(event) =>
                void send({
                  command: "network.fixtureScenario",
                  data: { scenario: event.target.value as "offline" },
                })
              }
            >
              {[
                "offline",
                "transport-only",
                "ready",
                "protocol-mismatch",
                "auth-failed",
              ].map((scenario) => (
                <option key={scenario}>{scenario}</option>
              ))}
            </select>
          </label>
        )}
      </section>
    );
  if (page === "Filesystem")
    return (
      <section className="card">
        <h2>Runtime filesystem</h2>
        <p>
          {provider?.fixture
            ? "Explicit VFS fixture. Host-folder checks are real; image containers are not decoded."
            : "The Xenon virtual-filesystem adapter is not connected."}
        </p>
        <p>
          Initialized: {provider?.filesystem.initialized ? "Yes" : "No"} ·
          Working directory:{" "}
          {provider?.filesystem.workingDirectory || "Not set"}
        </p>
        <fieldset
          disabled={blocked || !provider?.capabilities.filesystem}
          className="form"
        >
          <legend>Mount content</legend>
          <label>
            Guest mount point
            <input
              value={mountPoint}
              onChange={(event) => mount(event.target.value)}
            />
          </label>
          <label>
            Mount kind
            <select
              value={kind}
              onChange={(event) => setKind(event.target.value as typeof kind)}
            >
              <option value="host">Host folder</option>
              <option value="gdfx">GDFX image</option>
              <option value="stfs">STFS package</option>
            </select>
          </label>
          <label>
            Mount source
            <input
              value={source}
              onChange={(event) => setSource(event.target.value)}
            />
          </label>
          <button
            onClick={async () => {
              const [file] = await pick(kind === "host" ? "folder" : "open");
              if (file) setSource(file);
            }}
          >
            Choose mount source
          </button>
          <label className="row">
            Read only
            <input
              type="checkbox"
              checked={readOnly}
              onChange={(event) => setReadOnly(event.target.checked)}
            />
          </label>
          <button
            onClick={() =>
              void send({
                command: "filesystem.mount",
                data: { mountPoint, kind, source, readOnly },
              })
            }
          >
            Mount source
          </button>
        </fieldset>
        <h3>Mounted devices</h3>
        {provider?.filesystem.mounts.map((entry) => (
          <article className="card" key={entry.mountPoint}>
            <strong>{entry.mountPoint}</strong>
            <p className="path">{entry.source}</p>
            <p>
              {entry.kind} · {entry.readOnly ? "Read only" : "Writable"}
            </p>
            <button
              disabled={blocked}
              onClick={() =>
                void send({
                  command: "filesystem.unmount",
                  data: { mountPoint: entry.mountPoint },
                })
              }
            >
              Unmount {entry.mountPoint}
            </button>
          </article>
        ))}
        <fieldset
          disabled={blocked || !provider?.capabilities.filesystem}
          className="form"
        >
          <legend>Guest symbolic links</legend>
          <label>
            Guest alias
            <input
              value={alias}
              onChange={(event) => setAlias(event.target.value)}
            />
          </label>
          <label>
            Guest target
            <input
              value={target}
              onChange={(event) => setTarget(event.target.value)}
            />
          </label>
          <button
            onClick={() =>
              void send({ command: "filesystem.link", data: { alias, target } })
            }
          >
            Register guest link
          </button>
        </fieldset>
        {provider?.filesystem.symbolicLinks.map((link) => (
          <p key={link.alias}>
            {link.alias} → {link.target}{" "}
            <button
              disabled={blocked}
              onClick={() =>
                void send({
                  command: "filesystem.unlink",
                  data: { alias: link.alias },
                })
              }
            >
              Remove {link.alias}
            </button>
          </p>
        ))}
        <fieldset
          disabled={blocked || !provider?.capabilities.filesystem}
          className="form"
        >
          <legend>Guest path</legend>
          <label>
            Guest path
            <input
              value={path}
              onChange={(event) => setPath(event.target.value)}
            />
          </label>
          <div className="actions">
            <button
              onClick={() =>
                void send({ command: "filesystem.cwd", data: { path } })
              }
            >
              Set working directory
            </button>
            <button
              onClick={() =>
                void send({ command: "filesystem.test", data: { path } })
              }
            >
              Test guest path
            </button>
          </div>
        </fieldset>
        {state.serviceResult?.command.startsWith("filesystem.") && (
          <pre aria-label="Filesystem result">
            {JSON.stringify(state.serviceResult.result, null, 2)}
          </pre>
        )}
      </section>
    );
  return (
    <section className="card">
      <h2>Developer diagnostics</h2>
      <pre>
        {JSON.stringify(
          {
            revision: state.revision,
            protocolVersion: 1,
            capabilities: state.capabilities,
            runtimeServices: provider,
            serviceResult: state.serviceResult,
          },
          null,
          2,
        )}
      </pre>
    </section>
  );
}
